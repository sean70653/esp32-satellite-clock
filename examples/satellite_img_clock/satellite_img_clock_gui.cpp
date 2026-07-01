#include "satellite_img_clock_gui.h"
#include "satellite.h"
#include "Arduino.h"
#include "lvgl.h"
#include <WiFi.h>

static lv_obj_t *date_label;
static lv_obj_t *digit_labels[6];
static lv_obj_t *bg_img;
#if SAT_SHOW_FRAME_TIME
static lv_obj_t *frame_time_label;
#endif

static lv_img_dsc_t bg_dsc;
static int anim_index = 0;
static lv_timer_t *anim_timer = NULL;

static const char *month_names[] = {
    "Jan", "Feb", "Mar", "Apr", "May", "Jun",
    "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"
};
static const char *wday_names[] = {
    "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"
};

static void time_msg_cb(void *s, lv_msg_t *msg)
{
    if (lv_msg_get_id(msg) != MSG_NEW_TIME) return;
    const struct tm *t = (const struct tm *)lv_msg_get_payload(msg);

    char buf[2] = {0, 0};
    int digits[6] = {
        t->tm_hour / 10, t->tm_hour % 10,
        t->tm_min  / 10, t->tm_min  % 10,
        t->tm_sec  / 10, t->tm_sec  % 10
    };
    for (int i = 0; i < 6; i++) {
        buf[0] = '0' + digits[i];
        lv_label_set_text(digit_labels[i], buf);
    }

    lv_label_set_text_fmt(date_label, "%d %s %d, %s",
                          t->tm_year + 1900,
                          month_names[t->tm_mon],
                          t->tm_mday,
                          wday_names[t->tm_wday]);
}

static void sat_anim_cb(lv_timer_t *t)
{
    int count = sat_get_frame_count();
    if (count <= 0) return;

    for (int tries = 0; tries < SAT_FRAME_COUNT; tries++) {
        anim_index--;
        if (anim_index < 0) anim_index = SAT_FRAME_COUNT - 1;

        if (sat_frame_is_valid(anim_index)) {
            uint16_t *fb = sat_get_frame(anim_index);
            if (fb) {
                ui_set_sat_frame(fb, SAT_FRAME_W, SAT_FRAME_H);
            }
#if SAT_SHOW_FRAME_TIME
            const char *ts = sat_get_frame_time_str(anim_index);
            if (ts && ts[0]) {
                lv_label_set_text(frame_time_label, ts);
            } else {
                lv_label_set_text(frame_time_label, "");
            }
#endif
            return;
        }
    }
}

void ui_set_sat_frame(uint16_t *rgb565_buf, int w, int h)
{
    if (!bg_img) return;
    bg_dsc.header.cf          = LV_IMG_CF_TRUE_COLOR;
    bg_dsc.header.always_zero = 0;
    bg_dsc.header.reserved    = 0;
    bg_dsc.header.w           = w;
    bg_dsc.header.h           = h;
    bg_dsc.data_size          = w * h * 2;
    bg_dsc.data               = (const uint8_t *)rgb565_buf;
    lv_img_set_src(bg_img, &bg_dsc);
}

void ui_begin()
{
    lv_obj_t *scr = lv_scr_act();
    lv_obj_set_style_bg_color(scr, UI_BG_COLOR, LV_PART_MAIN);

    bg_img = lv_img_create(scr);
    lv_obj_set_size(bg_img, LV_PCT(100), LV_PCT(100));
    lv_obj_align(bg_img, LV_ALIGN_CENTER, 0, 0);

#if SAT_SHOW_FRAME_TIME
    frame_time_label = lv_label_create(scr);
    lv_obj_set_style_text_font(frame_time_label, &lv_font_montserrat_8, 0);
    lv_obj_set_style_text_color(frame_time_label, lv_color_hex(0xFFFF00), 0);
    lv_label_set_text(frame_time_label, "");
    lv_obj_align(frame_time_label, LV_ALIGN_TOP_RIGHT, -4, 2);
#endif

    /* Date label: "2025 Jul 1, Tue" in Montserrat 14, above the clock */
    date_label = lv_label_create(scr);
    lv_obj_set_style_text_font(date_label, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(date_label, UI_FONT_COLOR, 0);
    lv_label_set_text(date_label, "");
    lv_obj_align(date_label, LV_ALIGN_BOTTOM_RIGHT, -8, -42);

    /*
     * Clock: 8 fixed-position labels (6 digits + 2 colons).
     * Montserrat 36: digit ~20px wide, colon ~9px wide.
     */
    #define CLK_FONT    lv_font_montserrat_36
    #define CLK_DW      22
    #define CLK_CW      12
    #define CLK_Y       -4
    #define CLK_TOTAL   (6 * CLK_DW + 2 * CLK_CW)
    #define CLK_RIGHT   8

    int clk_base_x = 640 - CLK_RIGHT - CLK_TOTAL;
    int cell_x[] = {
        clk_base_x,
        clk_base_x + CLK_DW,
        clk_base_x + 2*CLK_DW + CLK_CW,
        clk_base_x + 3*CLK_DW + CLK_CW,
        clk_base_x + 4*CLK_DW + 2*CLK_CW,
        clk_base_x + 5*CLK_DW + 2*CLK_CW,
    };
    int colon_x[] = {
        clk_base_x + 2*CLK_DW,
        clk_base_x + 4*CLK_DW + CLK_CW,
    };

    for (int i = 0; i < 6; i++) {
        digit_labels[i] = lv_label_create(scr);
        lv_obj_set_style_text_font(digit_labels[i], &CLK_FONT, 0);
        lv_obj_set_style_text_color(digit_labels[i], UI_FONT_COLOR, 0);
        lv_obj_set_width(digit_labels[i], CLK_DW);
        lv_obj_set_style_text_align(digit_labels[i], LV_TEXT_ALIGN_CENTER, 0);
        lv_label_set_text(digit_labels[i], "0");
        lv_obj_set_pos(digit_labels[i], cell_x[i], 180 - 36 + CLK_Y);
    }

    for (int i = 0; i < 2; i++) {
        lv_obj_t *c = lv_label_create(scr);
        lv_obj_set_style_text_font(c, &CLK_FONT, 0);
        lv_obj_set_style_text_color(c, UI_FONT_COLOR, 0);
        lv_obj_set_width(c, CLK_CW);
        lv_obj_set_style_text_align(c, LV_TEXT_ALIGN_CENTER, 0);
        lv_label_set_text(c, ":");
        lv_obj_set_pos(c, colon_x[i], 180 - 36 + CLK_Y);
    }

    lv_msg_subsribe(MSG_NEW_TIME, time_msg_cb, NULL);

    anim_timer = lv_timer_create(sat_anim_cb, 1000, NULL);
}
