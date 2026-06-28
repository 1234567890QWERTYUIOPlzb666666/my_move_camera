#ifndef QUICK_SETTINGS_PAGE_H
#define QUICK_SETTINGS_PAGE_H

#include <stdint.h>
#include "lvgl/lvgl.h"

void quick_settings_page_create(uint32_t screen_w, uint32_t screen_h);
lv_obj_t * quick_settings_page_create_curtain(lv_obj_t * parent);
void quick_settings_page_pointer_sample(bool pressed, int32_t x, int32_t y);
void quick_settings_page_poll(void);

#endif
