#ifndef CAMERA_MEDIA_H
#define CAMERA_MEDIA_H

#include "lvgl/lvgl.h"

#include <stddef.h>
#include <stdint.h>

int camera_media_save_jpeg(const char * path,
                           const lv_color_t * pixels,
                           uint32_t width,
                           uint32_t height,
                           int quality);
int camera_media_load_jpeg(const char * path,
                           lv_color_t * pixels,
                           uint32_t width,
                           uint32_t height);

#endif
