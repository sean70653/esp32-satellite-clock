#include "lvgl.h"
#include "pins_config.h"
#include "AXS15231B.h"
#include "WiFi.h"
#include "satellite_img_clock_gui.h"
#include "satellite.h"
#include "sntp.h"
#include "time.h"
#include "zones.h"
#include <Arduino.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <esp_wifi.h>
#include <Wire.h>
#include "freertos/semphr.h"

SemaphoreHandle_t xSemaphore = NULL;

static lv_disp_draw_buf_t draw_buf;
static lv_color_t        *buf;
static lv_color_t        *buf1;

#define AXS_TOUCH_ONE_POINT_LEN 6
#define AXS_TOUCH_BUF_HEAD_LEN  2

#define AXS_TOUCH_GESTURE_POS 0
#define AXS_TOUCH_POINT_NUM   1
#define AXS_TOUCH_EVENT_POS   2
#define AXS_TOUCH_X_H_POS     2
#define AXS_TOUCH_X_L_POS     3
#define AXS_TOUCH_ID_POS      4
#define AXS_TOUCH_Y_H_POS     4
#define AXS_TOUCH_Y_L_POS     5
#define AXS_TOUCH_WEIGHT_POS  6
#define AXS_TOUCH_AREA_POS    7

#define AXS_GET_POINT_NUM(buf)                buf[AXS_TOUCH_POINT_NUM]
#define AXS_GET_GESTURE_TYPE(buf)             buf[AXS_TOUCH_GESTURE_POS]
#define AXS_GET_POINT_X(buf, point_index)     (((uint16_t)(buf[AXS_TOUCH_ONE_POINT_LEN * point_index + AXS_TOUCH_X_H_POS] & 0x0F) << 8) + (uint16_t)buf[AXS_TOUCH_ONE_POINT_LEN * point_index + AXS_TOUCH_X_L_POS])
#define AXS_GET_POINT_Y(buf, point_index)     (((uint16_t)(buf[AXS_TOUCH_ONE_POINT_LEN * point_index + AXS_TOUCH_Y_H_POS] & 0x0F) << 8) + (uint16_t)buf[AXS_TOUCH_ONE_POINT_LEN * point_index + AXS_TOUCH_Y_L_POS])
#define AXS_GET_POINT_EVENT(buf, point_index) (buf[AXS_TOUCH_ONE_POINT_LEN * point_index + AXS_TOUCH_EVENT_POS] >> 6)

void wifi_scan_and_connect(void);
void setTimezone();

static uint32_t last_tick;
struct tm       timeinfo;

void my_disp_flush(lv_disp_drv_t *disp, const lv_area_t *area,
                   lv_color_t *color_p)
{
    uint32_t w = (area->x2 - area->x1 + 1);
    uint32_t h = (area->y2 - area->y1 + 1);

#ifdef LCD_SPI_DMA
    char i = 0;
    while (get_lcd_spi_dma_write()) {
        i = i >> 1;
        lcd_PushColors(0, 0, 0, 0, NULL);
    }
#endif
    lcd_PushColors(area->x1, area->y1, w, h, (uint16_t *)&color_p->full);

#ifdef LCD_SPI_DMA

#else
    lv_disp_flush_ready(disp);
#endif
}

uint8_t read_touchpad_cmd[11] = {0xb5, 0xab, 0xa5, 0x5a, 0x0, 0x0, 0x0, 0x8};
void    my_touchpad_read(lv_indev_drv_t *indev_driver, lv_indev_data_t *data)
{
    xSemaphoreTake(xSemaphore, portMAX_DELAY);
    uint8_t buff[20] = {0};

    Wire.beginTransmission(0x3B);
    Wire.write(read_touchpad_cmd, 8);
    Wire.endTransmission();
    Wire.requestFrom(0x3B, 8);
    while (!Wire.available())
        ;
    Wire.readBytes(buff, 8);

    uint16_t pointX;
    uint16_t pointY;
    uint16_t type = 0;

    type   = AXS_GET_GESTURE_TYPE(buff);
    pointX = AXS_GET_POINT_X(buff, 0);
    pointY = AXS_GET_POINT_Y(buff, 0);

    if (!type && (pointX || pointY)) {
        pointX = (640 - pointX);
        if (pointX > 640)
            pointX = 640;
        if (pointY > 180)
            pointY = 180;
        data->state   = LV_INDEV_STATE_PR;
        data->point.x = pointY;
        data->point.y = pointX;
    } else {
        data->state = LV_INDEV_STATE_REL;
    }
    xSemaphoreGive(xSemaphore);
}

extern uint32_t transfer_num;
extern size_t   lcd_PushColors_len;

void lv_delay_ms(int x)
{
    do {
        uint32_t t = x;
        while (t--) {
            delay(1);
            if (transfer_num <= 0 && lcd_PushColors_len <= 0)
                lv_timer_handler();

            if (transfer_num <= 1 && lcd_PushColors_len > 0) {
                lcd_PushColors(0, 0, 0, 0, NULL);
            }
        }
    } while (0);
}

void setup()
{
    xSemaphore = xSemaphoreCreateBinary();
    xSemaphoreGive(xSemaphore);

    Serial.begin(115200);
    Serial.println("satellite_img_clock start");

    pinMode(PIN_BAT_VOLT, ANALOG);

    pinMode(TOUCH_RES, OUTPUT);
    digitalWrite(TOUCH_RES, HIGH);
    delay(2);
    digitalWrite(TOUCH_RES, LOW);
    delay(10);
    digitalWrite(TOUCH_RES, HIGH);
    delay(2);

    Wire.begin(TOUCH_IICSDA, TOUCH_IICSCL);

    configTzTime(CUSTOM_TIMEZONE, NTP_SERVER1, NTP_SERVER2);

    axs15231_init();

    lv_init();
    size_t buffer_size =
        sizeof(lv_color_t) * EXAMPLE_LCD_H_RES * EXAMPLE_LCD_V_RES;
    buf = (lv_color_t *)ps_malloc(buffer_size);
    if (buf == NULL) {
        while (1) {
            Serial.println("buf NULL");
            delay(500);
        }
    }

    buf1 = (lv_color_t *)ps_malloc(buffer_size);
    if (buf1 == NULL) {
        while (1) {
            Serial.println("buf1 NULL");
            delay(500);
        }
    }

    lv_disp_draw_buf_init(&draw_buf, buf, buf1, buffer_size);

    static lv_disp_drv_t disp_drv;
    lv_disp_drv_init(&disp_drv);
    disp_drv.hor_res      = EXAMPLE_LCD_H_RES;
    disp_drv.ver_res      = EXAMPLE_LCD_V_RES;
    disp_drv.flush_cb     = my_disp_flush;
    disp_drv.draw_buf     = &draw_buf;
    disp_drv.sw_rotate    = 1;
    disp_drv.rotated      = LV_DISP_ROT_90;
    disp_drv.full_refresh = 1;
    lv_disp_drv_register(&disp_drv);

    static lv_indev_drv_t indev_drv;
    lv_indev_drv_init(&indev_drv);
    indev_drv.type    = LV_INDEV_TYPE_POINTER;
    indev_drv.read_cb = my_touchpad_read;
    lv_indev_drv_register(&indev_drv);

    setenv("TZ", CUSTOM_TIMEZONE, 1);
    tzset();

    sat_init();

    Serial.println("init done");
}

void loop()
{
    delay(1);
    if (transfer_num <= 0 && lcd_PushColors_len <= 0)
        lv_timer_handler();

    if (transfer_num <= 1 && lcd_PushColors_len > 0) {
        lcd_PushColors(0, 0, 0, 0, NULL);
    }

    if (millis() - last_tick > 100) {
        if (getLocalTime(&timeinfo, 2)) {
            lv_msg_send(MSG_NEW_TIME, &timeinfo);
            sat_check_schedule(&timeinfo);
        }
        last_tick = millis();
    }

    static int           flag_bl = 0;
    static unsigned long cnt     = 0;

    cnt++;
    if (cnt >= 100) {
        if (flag_bl == 0) {
            pinMode(TFT_BL, OUTPUT);
            digitalWrite(TFT_BL, HIGH);
            flag_bl = 1;
            wifi_scan_and_connect();
            lv_delay_ms(500);
            setTimezone();
            ui_begin();
            sat_start_initial_download();
        }
    }
}

void wifi_scan_and_connect(void)
{
    String    text;
    lv_obj_t *log_label = lv_label_create(lv_scr_act());
    lv_obj_align(log_label, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_set_width(log_label, LV_PCT(100));
    lv_label_set_long_mode(log_label, LV_LABEL_LONG_SCROLL);
    lv_label_set_recolor(log_label, true);

    lv_label_set_text(log_label, "Scanning WiFi...");
    lv_delay_ms(1);

    WiFi.mode(WIFI_STA);
    WiFi.disconnect();
    lv_delay_ms(100);

    int n = WiFi.scanNetworks();
    Serial.println("scan done");

    if (n == 0) {
        text = "No networks found";
    } else {
        text = String(n) + " networks found\n";
        for (int i = 0; i < n; ++i) {
            text += String(i + 1) + ": " + WiFi.SSID(i);
            text += " (" + String(WiFi.RSSI(i)) + ")";
            text += (WiFi.encryptionType(i) == WIFI_AUTH_OPEN) ? " \n" : "*\n";
            lv_delay_ms(10);
        }
    }

    lv_label_set_text(log_label, text.c_str());
    Serial.println(text);
    lv_delay_ms(2000);

    wifi_config_t current_conf = {0};
    esp_wifi_get_config(WIFI_IF_STA, &current_conf);
    if (strlen((const char *)current_conf.sta.ssid) == 0) {
        Serial.println("Using default WiFi SSID & PASSWORD");
        memcpy((char *)(current_conf.sta.ssid), (const char *)WIFI_SSID, strlen(WIFI_SSID) + 1);
        memcpy((char *)(current_conf.sta.password), (const char *)WIFI_PASSWORD, strlen(WIFI_PASSWORD) + 1);
        WiFi.begin((char *)(current_conf.sta.ssid), (char *)(current_conf.sta.password));
    } else {
        Serial.println("Begin WiFi (saved credentials)");
        WiFi.begin();
    }

    text = "Connecting to ";
    text += (char *)(current_conf.sta.ssid);
    text += "\n";

    uint32_t connect_start          = millis();
    bool     is_smartconfig_connect = false;
    lv_label_set_long_mode(log_label, LV_LABEL_LONG_WRAP);

    while (WiFi.status() != WL_CONNECTED) {
        Serial.print(".");
        text += ".";
        lv_label_set_text(log_label, text.c_str());
        lv_delay_ms(100);
        if (millis() - connect_start > WIFI_CONNECT_WAIT_MAX) {
            text += "\nConnection timed out, starting SmartConfig";
            lv_label_set_text(log_label, text.c_str());
            lv_delay_ms(100);
            is_smartconfig_connect = true;
            WiFi.mode(WIFI_AP_STA);
            Serial.println("\nWaiting for SmartConfig...");
            text += "\nPlease use #ff0000 EspTouch# App to configure";
            lv_label_set_text(log_label, text.c_str());
            WiFi.beginSmartConfig();
            while (1) {
                lv_delay_ms(100);
                if (WiFi.smartConfigDone()) {
                    Serial.println("SmartConfig Success");
                    Serial.printf("SSID:%s\r\n", WiFi.SSID().c_str());
                    Serial.printf("PSW:%s\r\n", WiFi.psk().c_str());
                    text += "\nSmartConfig OK!";
                    text += "\nSSID: " + WiFi.SSID();
                    lv_label_set_text(log_label, text.c_str());
                    lv_delay_ms(1000);
                    connect_start = millis();
                    break;
                }
            }
        }
    }

    if (!is_smartconfig_connect) {
        text += "\nCONNECTED! (" + String(millis() - connect_start) + " ms)";
        Serial.println("\nConnected");
    }
    text += "\nIP: " + WiFi.localIP().toString();
    lv_label_set_text(log_label, text.c_str());
    Serial.println("IP: " + WiFi.localIP().toString());
    lv_delay_ms(2000);

    lv_obj_del(log_label);
}

void setTimezone()
{
#ifdef CUSTOM_TIMEZONE
    String timezone = CUSTOM_TIMEZONE;
#else
    const char *rootCACertificate = R"string_literal(
-----BEGIN CERTIFICATE-----
MIICnzCCAiWgAwIBAgIQf/MZd5csIkp2FV0TttaF4zAKBggqhkjOPQQDAzBHMQsw
CQYDVQQGEwJVUzEiMCAGA1UEChMZR29vZ2xlIFRydXN0IFNlcnZpY2VzIExMQzEU
MBIGA1UEAxMLR1RTIFJvb3QgUjQwHhcNMjMxMjEzMDkwMDAwWhcNMjkwMjIwMTQw
MDAwWjA7MQswCQYDVQQGEwJVUzEeMBwGA1UEChMVR29vZ2xlIFRydXN0IFNlcnZp
Y2VzMQwwCgYDVQQDEwNXRTEwWTATBgcqhkjOPQIBBggqhkjOPQMBBwNCAARvzTr+
Z1dHTCEDhUDCR127WEcPQMFcF4XGGTfn1XzthkubgdnXGhOlCgP4mMTG6J7/EFmP
LCaY9eYmJbsPAvpWo4H+MIH7MA4GA1UdDwEB/wQEAwIBhjAdBgNVHSUEFjAUBggr
BgEFBQcDAQYIKwYBBQUHAwIwEgYDVR0TAQH/BAgwBgEB/wIBADAdBgNVHQ4EFgQU
kHeSNWfE/6jMqeZ72YB5e8yT+TgwHwYDVR0jBBgwFoAUgEzW63T/STaj1dj8tT7F
avCUHYwwNAYIKwYBBQUHAQEEKDAmMCQGCCsGAQUFBzAChhhodHRwOi8vaS5wa2ku
Z29vZy9yNC5jcnQwKwYDVR0fBCQwIjAgoB6gHIYaaHR0cDovL2MucGtpLmdvb2cv
ci9yNC5jcmwwEwYDVR0gBAwwCjAIBgZngQwBAgEwCgYIKoZIzj0EAwMDaAAwZQIx
AOcCq1HW90OVznX+0RGU1cxAQXomvtgM8zItPZCuFQ8jSBJSjz5keROv9aYsAm5V
sQIwJonMaAFi54mrfhfoFNZEfuNMSQ6/bIBiNLiyoX46FohQvKeIoJ99cx7sUkFN
7uJW
-----END CERTIFICATE-----
)string_literal";

    WiFiClientSecure *client = new WiFiClientSecure;
    String timezone;
    if (client) {
        client->setCACert(rootCACertificate);
        HTTPClient https;
        if (https.begin(*client, "https://ipapi.co/timezone/")) {
            int httpCode = https.GET();
            if (httpCode > 0) {
                Serial.printf("[HTTPS] GET timezone... code: %d\n", httpCode);
                if (httpCode == HTTP_CODE_OK || httpCode == HTTP_CODE_MOVED_PERMANENTLY) {
                    String payload = https.getString();
                    Serial.println(payload);
                    timezone = payload;
                }
            } else {
                Serial.printf("[HTTPS] GET timezone failed: %s\n", https.errorToString(httpCode).c_str());
            }
            https.end();
        }
        delete client;
    }
    for (uint32_t i = 0; i < sizeof(zones); i++) {
        if (timezone == "None") {
            timezone = "CST-8";
            break;
        }
        if (timezone == zones[i].name) {
            timezone = zones[i].zones;
            break;
        }
    }
#endif

    if (timezone.length() > 0) {
        Serial.println("timezone: " + timezone);
        setenv("TZ", timezone.c_str(), 1);
        tzset();
    } else {
        Serial.println("Failed to fetch timezone, using default");
    }
}
