#pragma once

#include <stdint.h>
#include <time.h>

#define UI_BG_COLOR    lv_color_black()
#define UI_FRAME_COLOR lv_color_hex(0x282828)
#define UI_FONT_COLOR  lv_color_white()

#define MSG_NEW_HOUR   1
#define MSG_NEW_MIN    2
#define MSG_NEW_SEC    5
#define MSG_WIFI_INFO  6
#define MSG_NEW_TIME   7

#define SAT_SHOW_FRAME_TIME  1

void ui_begin();
void ui_set_sat_frame(uint16_t *rgb565_buf, int w, int h);
