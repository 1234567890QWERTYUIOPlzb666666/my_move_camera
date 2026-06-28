#ifndef CAMERA_PAGE_H
#define CAMERA_PAGE_H

#include <stdbool.h>
#include <stdint.h>
#include "lvgl/lvgl.h"

#define UI_WIDTH  800
#define UI_HEIGHT 480

typedef bool (*camera_page_capture_cb_t)(void * user_data);

typedef struct {
    uint32_t width;
    uint32_t height;
    uint32_t fps;
} camera_page_record_settings_t;

typedef bool (*camera_page_record_cb_t)(bool start,
                                        const camera_page_record_settings_t * settings,
                                        void * user_data);
typedef bool (*camera_page_play_video_cb_t)(const char * path, void * user_data);

void camera_page_create(uint32_t screen_w, uint32_t screen_h);
void camera_page_pointer_sample(bool pressed, int32_t x, int32_t y);
void camera_page_set_preview_image(const lv_img_dsc_t * image);
void camera_page_refresh_preview(void);
void camera_page_set_capture_callback(camera_page_capture_cb_t callback, void * user_data);
void camera_page_set_record_callback(camera_page_record_cb_t callback, void * user_data);
void camera_page_set_play_video_callback(camera_page_play_video_cb_t callback, void * user_data);
void camera_page_set_album_image(const lv_img_dsc_t * image, const char * path);

#endif
