#pragma once

#include <stdint.h>
#include <stdbool.h>

#define SAT_FRAME_COUNT    18
#define SAT_FRAME_W        640
#define SAT_FRAME_H        180
#define SAT_FRAME_BYTES    (SAT_FRAME_W * SAT_FRAME_H * 2)

#define SAT_SRC_SIZE       2750
#define SAT_CROP_H         773
#define SAT_CROP_Y_START   ((SAT_SRC_SIZE - SAT_CROP_H) / 2)

#define SAT_URL_BASE       "https://www.cwa.gov.tw/Data/satellite/LCC_TRGB_2750/LCC_TRGB_2750"
#define SAT_JPEG_BUF_SIZE  (2048 * 1024)

void     sat_init(void);
bool     sat_is_ready(void);
int      sat_get_frame_count(void);
uint16_t *sat_get_frame(int index);
bool     sat_frame_is_valid(int index);
const char *sat_get_frame_time_str(int index);
void     sat_start_initial_download(void);
void     sat_check_schedule(struct tm *now);
