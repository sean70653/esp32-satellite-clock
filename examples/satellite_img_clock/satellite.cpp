#include "satellite.h"
#include <Arduino.h>
#include <HTTPClient.h>
#include <WiFi.h>
#include "esp32s3/rom/tjpgd.h"
#include "time.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

static uint16_t *frame_bufs[SAT_FRAME_COUNT];
static bool      frame_valid[SAT_FRAME_COUNT];
static char      frame_time_str[SAT_FRAME_COUNT][20]; // "YYYY-MM-DD HH:MM"
static uint8_t  *jpeg_buf = NULL;
static size_t    jpeg_len = 0;
static int       valid_frames = 0;
static bool      sat_ready = false;
static int       last_download_hour = -1;
static int       last_download_third = -1;
static bool      initial_download_done = false;
static TaskHandle_t dl_task_handle = NULL;

static SemaphoreHandle_t frame_mutex;

struct JpegDecodeCtx {
    const uint8_t *data;
    size_t         data_len;
    size_t         data_pos;
    uint16_t      *out_buf;
    uint32_t       block_count;
};

static UINT jpeg_input_func(JDEC *jd, BYTE *buff, UINT ndata)
{
    JpegDecodeCtx *ctx = (JpegDecodeCtx *)jd->device;
    size_t remain = ctx->data_len - ctx->data_pos;
    if (ndata > remain) ndata = remain;
    if (buff) {
        memcpy(buff, ctx->data + ctx->data_pos, ndata);
    }
    ctx->data_pos += ndata;
    return ndata;
}

static inline uint16_t rgb888_to_rgb565_swap(uint8_t r, uint8_t g, uint8_t b)
{
    uint16_t c = ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3);
    return (c >> 8) | (c << 8);
}

static UINT jpeg_output_func(JDEC *jd, void *bitmap, JRECT *rect)
{
    JpegDecodeCtx *ctx = (JpegDecodeCtx *)jd->device;
    uint8_t *src = (uint8_t *)bitmap;

    uint16_t src_w = jd->width;

    int crop_y0 = SAT_CROP_Y_START;
    int crop_y1 = crop_y0 + SAT_CROP_H;

    ctx->block_count++;
    if (ctx->block_count % 50 == 0) vTaskDelay(1);

    if (rect->bottom < (uint16_t)crop_y0 || rect->top >= (uint16_t)crop_y1) {
        return 1;
    }

    for (uint16_t y = rect->top; y <= rect->bottom; y++) {
        if (y < (uint16_t)crop_y0 || y >= (uint16_t)crop_y1) continue;
        for (uint16_t x = rect->left; x <= rect->right; x++) {
            int src_idx = ((y - rect->top) * (rect->right - rect->left + 1) + (x - rect->left)) * 3;
            uint8_t r = src[src_idx];
            uint8_t g = src[src_idx + 1];
            uint8_t b = src[src_idx + 2];

            int dst_x = (int)((uint32_t)x * SAT_FRAME_W / src_w);
            int dst_y = (int)((uint32_t)(y - crop_y0) * SAT_FRAME_H / SAT_CROP_H);

            if (dst_x >= SAT_FRAME_W) dst_x = SAT_FRAME_W - 1;
            if (dst_y >= SAT_FRAME_H) dst_y = SAT_FRAME_H - 1;

            ctx->out_buf[dst_y * SAT_FRAME_W + dst_x] = rgb888_to_rgb565_swap(r, g, b);
        }
    }

    return 1;
}

static bool decode_jpeg_to_frame(uint16_t *out_buf)
{
    JpegDecodeCtx ctx;
    ctx.data        = jpeg_buf;
    ctx.data_len    = jpeg_len;
    ctx.data_pos    = 0;
    ctx.out_buf     = out_buf;
    ctx.block_count = 0;

    memset(out_buf, 0, SAT_FRAME_BYTES);

    void *work = malloc(3100);
    if (!work) {
        Serial.println("SAT: work buf alloc failed");
        return false;
    }

    JDEC jd;
    JRESULT res = jd_prepare(&jd, jpeg_input_func, work, 3100, &ctx);
    if (res != JDR_OK) {
        Serial.printf("SAT: jd_prepare failed: %d\n", res);
        free(work);
        return false;
    }

    Serial.printf("SAT: JPEG %dx%d, decoding...\n", jd.width, jd.height);

    res = jd_decomp(&jd, jpeg_output_func, 0);
    free(work);

    if (res != JDR_OK) {
        Serial.printf("SAT: jd_decomp failed: %d\n", res);
        return false;
    }
    Serial.printf("SAT: decoded OK (%u blocks)\n", ctx.block_count);
    return true;
}

static bool download_image(const char *url)
{
    Serial.printf("SAT: GET %s\n", url);
    HTTPClient http;
    http.setConnectTimeout(10000);
    http.setTimeout(30000);

    if (!http.begin(url)) {
        Serial.println("SAT: http.begin failed");
        return false;
    }

    int code = http.GET();
    if (code != HTTP_CODE_OK) {
        Serial.printf("SAT: HTTP %d\n", code);
        http.end();
        return false;
    }

    int len = http.getSize();
    if (len <= 0 || len > (int)SAT_JPEG_BUF_SIZE) {
        Serial.printf("SAT: bad size %d (max %d)\n", len, SAT_JPEG_BUF_SIZE);
        http.end();
        return false;
    }

    WiFiClient *stream = http.getStreamPtr();
    jpeg_len = 0;
    uint32_t read_chunks = 0;
    while (http.connected() && jpeg_len < (size_t)len) {
        size_t avail = stream->available();
        if (avail) {
            size_t to_read = avail;
            if (to_read > 4096) to_read = 4096;
            if (jpeg_len + to_read > SAT_JPEG_BUF_SIZE)
                to_read = SAT_JPEG_BUF_SIZE - jpeg_len;
            size_t got = stream->readBytes(jpeg_buf + jpeg_len, to_read);
            jpeg_len += got;
            read_chunks++;
            if (read_chunks % 8 == 0) vTaskDelay(1);
        } else {
            vTaskDelay(1);
        }
    }
    http.end();
    Serial.printf("SAT: downloaded %u bytes\n", jpeg_len);
    return jpeg_len > 0;
}

static void build_url(char *buf, size_t buflen, struct tm *t)
{
    snprintf(buf, buflen, "%s-%04d-%02d-%02d-%02d-%02d.jpg",
             SAT_URL_BASE,
             t->tm_year + 1900, t->tm_mon + 1, t->tm_mday,
             t->tm_hour, t->tm_min);
}

static void round_to_20min(struct tm *t)
{
    t->tm_min = (t->tm_min / 20) * 20;
    t->tm_sec = 0;
}

static void subtract_minutes(struct tm *t, int minutes)
{
    time_t epoch = mktime(t);
    epoch -= minutes * 60;
    localtime_r(&epoch, t);
}

static bool download_one_frame(int frame_idx, struct tm *target)
{
    char url[256];
    build_url(url, sizeof(url), target);

    if (!download_image(url)) return false;

    xSemaphoreTake(frame_mutex, portMAX_DELAY);
    bool ok = decode_jpeg_to_frame(frame_bufs[frame_idx]);
    if (ok) {
        frame_valid[frame_idx] = true;
        snprintf(frame_time_str[frame_idx], sizeof(frame_time_str[0]),
                 "%04d-%02d-%02d %02d:%02d",
                 target->tm_year + 1900, target->tm_mon + 1, target->tm_mday,
                 target->tm_hour, target->tm_min);
        Serial.printf("SAT: frame[%d] = %s OK\n", frame_idx, frame_time_str[frame_idx]);
        if (!sat_ready) {
            valid_frames = 1;
            sat_ready = true;
        }
    }
    xSemaphoreGive(frame_mutex);
    return ok;
}

static void update_valid_count(void)
{
    int vcount = 0;
    for (int i = 0; i < SAT_FRAME_COUNT; i++) {
        if (frame_valid[i]) vcount++;
    }
    valid_frames = vcount;
}

static void download_task(void *param)
{
    bool initial = (bool)(intptr_t)param;

    struct tm now;
    if (!getLocalTime(&now, 5000)) {
        Serial.println("SAT: getLocalTime failed");
        dl_task_handle = NULL;
        vTaskDelete(NULL);
        return;
    }

    Serial.printf("SAT: local time = %04d-%02d-%02d %02d:%02d:%02d\n",
                  now.tm_year + 1900, now.tm_mon + 1, now.tm_mday,
                  now.tm_hour, now.tm_min, now.tm_sec);

    if (initial) {
        round_to_20min(&now);
        subtract_minutes(&now, 40);

        struct tm frame_times[SAT_FRAME_COUNT];
        for (int i = 0; i < SAT_FRAME_COUNT; i++) {
            frame_times[i] = now;
            subtract_minutes(&frame_times[i], i * 20);
        }

        Serial.printf("SAT: initial download, newest=%02d:%02d oldest=%02d:%02d\n",
                      frame_times[0].tm_hour, frame_times[0].tm_min,
                      frame_times[SAT_FRAME_COUNT - 1].tm_hour, frame_times[SAT_FRAME_COUNT - 1].tm_min);

        for (int i = 0; i < SAT_FRAME_COUNT; i++) {
            download_one_frame(i, &frame_times[i]);
            vTaskDelay(pdMS_TO_TICKS(100));
        }

        for (int round = 2; round <= 6; round++) {
            update_valid_count();
            if (valid_frames >= SAT_FRAME_COUNT) break;

            int failed = SAT_FRAME_COUNT - valid_frames;
            Serial.printf("SAT: round %d, %d frames still missing, waiting %ds...\n",
                          round, failed, round * 5);
            vTaskDelay(pdMS_TO_TICKS(round * 5000));

            for (int i = 0; i < SAT_FRAME_COUNT; i++) {
                if (frame_valid[i]) continue;
                Serial.printf("SAT: retry frame[%d]\n", i);
                download_one_frame(i, &frame_times[i]);
                vTaskDelay(pdMS_TO_TICKS(100));
            }
        }

        initial_download_done = true;
        update_valid_count();
        Serial.printf("SAT: initial download complete, %d/%d frames valid\n", valid_frames, SAT_FRAME_COUNT);
    } else {
        round_to_20min(&now);
        subtract_minutes(&now, 40);

        xSemaphoreTake(frame_mutex, portMAX_DELAY);
        uint16_t *oldest = frame_bufs[SAT_FRAME_COUNT - 1];
        for (int i = SAT_FRAME_COUNT - 1; i > 0; i--) {
            frame_bufs[i] = frame_bufs[i - 1];
            frame_valid[i] = frame_valid[i - 1];
            memcpy(frame_time_str[i], frame_time_str[i - 1], sizeof(frame_time_str[0]));
        }
        frame_bufs[0] = oldest;
        frame_valid[0] = false;
        frame_time_str[0][0] = '\0';
        xSemaphoreGive(frame_mutex);

        struct tm target = now;
        for (int retry = 0; retry < 5; retry++) {
            if (download_one_frame(0, &target)) break;
            Serial.printf("SAT: update retry %d, waiting 10s...\n", retry + 1);
            vTaskDelay(pdMS_TO_TICKS(10000));
        }

        update_valid_count();
        Serial.printf("SAT: update complete, %d/%d frames valid\n", valid_frames, SAT_FRAME_COUNT);
    }

    dl_task_handle = NULL;
    vTaskDelete(NULL);
}

void sat_init(void)
{
    frame_mutex = xSemaphoreCreateMutex();
    jpeg_buf = (uint8_t *)ps_malloc(SAT_JPEG_BUF_SIZE);
    if (!jpeg_buf) {
        Serial.println("SAT: JPEG buf alloc failed!");
        return;
    }
    for (int i = 0; i < SAT_FRAME_COUNT; i++) {
        frame_bufs[i] = (uint16_t *)ps_malloc(SAT_FRAME_BYTES);
        frame_valid[i] = false;
        frame_time_str[i][0] = '\0';
        if (!frame_bufs[i]) {
            Serial.printf("SAT: frame[%d] alloc failed!\n", i);
            return;
        }
        memset(frame_bufs[i], 0, SAT_FRAME_BYTES);
    }
    Serial.println("SAT: init OK");
}

bool sat_is_ready(void)
{
    return sat_ready;
}

int sat_get_frame_count(void)
{
    return valid_frames;
}

uint16_t *sat_get_frame(int index)
{
    if (index < 0 || index >= SAT_FRAME_COUNT) return NULL;
    return frame_bufs[index];
}

bool sat_frame_is_valid(int index)
{
    if (index < 0 || index >= SAT_FRAME_COUNT) return false;
    return frame_valid[index];
}

const char *sat_get_frame_time_str(int index)
{
    if (index < 0 || index >= SAT_FRAME_COUNT) return "";
    return frame_time_str[index];
}

void sat_start_initial_download(void)
{
    xTaskCreatePinnedToCore(download_task, "sat_dl", 16384,
                            (void *)(intptr_t)true, 1, &dl_task_handle, 0);
}

void sat_check_schedule(struct tm *now)
{
    if (!initial_download_done) return;
    if (dl_task_handle != NULL) return;

    int m = now->tm_min;
    int h = now->tm_hour;
    /* Trigger at xx:01, xx:21, xx:41 */
    if (m != 1 && m != 21 && m != 41) return;

    int third = m / 20;   // 0, 1, or 2
    if (h == last_download_hour && third == last_download_third) return;

    last_download_hour = h;
    last_download_third = third;
    Serial.printf("SAT: scheduled update at %02d:%02d\n", h, m);
    xTaskCreatePinnedToCore(download_task, "sat_dl", 16384,
                            (void *)(intptr_t)false, 1, &dl_task_handle, 0);
}
