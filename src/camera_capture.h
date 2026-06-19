#ifndef CAMERA_CAPTURE_H
#define CAMERA_CAPTURE_H

#include "lvgl/lvgl.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

int camera_capture_start(uint32_t output_width, uint32_t output_height);
bool camera_capture_copy_latest(lv_color_t * destination, size_t pixel_count);
const char *camera_capture_get_device_path(void);
bool camera_capture_find_alternate_device(const char * avoid_path,
                                          char * selected_path,
                                          size_t selected_path_size);
void camera_capture_stop(void);

#endif
