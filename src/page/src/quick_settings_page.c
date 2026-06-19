#include "../inc/quick_settings_page.h"
#include "../inc/camera_page.h"
#include "lvgl/lvgl.h"

#define UI_TEXT_FONT &lv_font_montserrat_14
#define UI_ICON_FONT &lv_font_montserrat_48
#define BACK_EDGE_TOUCH_H 96

static bool back_swipe_tracking = false;
static bool back_swipe_from_bottom = false;
static lv_point_t back_swipe_start;
static bool camera_open_pending = false;
static lv_obj_t *active_quick_page = NULL;

static void request_camera_page_open(void);

static void quick_page_set_y(void * var, int32_t y)
{
    lv_obj_set_y((lv_obj_t *)var, y);
}

static lv_obj_t *label_create(lv_obj_t * parent, const char * text, lv_color_t color, const lv_font_t * font)
{
    lv_obj_t * label = lv_label_create(parent);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_color(label, color, 0);
    lv_obj_set_style_text_font(label, font, 0);
    return label;
}

static void open_camera_page_async(void * user_data)
{
    (void)user_data;
    camera_open_pending = false;
    active_quick_page = NULL;
    camera_page_create(UI_WIDTH, UI_HEIGHT);
}

static void quick_page_open_camera_ready_cb(lv_anim_t * anim)
{
    (void)anim;
    lv_async_call(open_camera_page_async, NULL);
}

static void quick_page_drag_anim_to(int32_t target_y, lv_anim_ready_cb_t ready_cb)
{
    if (active_quick_page == NULL) {
        request_camera_page_open();
        return;
    }

    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, active_quick_page);
    lv_anim_set_exec_cb(&a, quick_page_set_y);
    lv_anim_set_values(&a, lv_obj_get_y(active_quick_page), target_y);
    lv_anim_set_time(&a, 180);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_set_ready_cb(&a, ready_cb);
    lv_anim_start(&a);
}

static void request_camera_page_open(void)
{
    if (camera_open_pending) {
        return;
    }

    camera_open_pending = true;
    back_swipe_tracking = false;
    back_swipe_from_bottom = false;

    if (active_quick_page != NULL) {
        quick_page_drag_anim_to(-UI_HEIGHT, quick_page_open_camera_ready_cb);
    } else {
        lv_async_call(open_camera_page_async, NULL);
    }
}

static int32_t clamp_i32(int32_t value, int32_t min, int32_t max)
{
    if (value < min) return min;
    if (value > max) return max;
    return value;
}

static void quick_page_set_drag_delta(int32_t delta_y)
{
    if (active_quick_page == NULL) {
        return;
    }

    lv_obj_set_y(active_quick_page, -clamp_i32(delta_y, 0, UI_HEIGHT));
}

static void quick_page_finish_drag(int32_t delta_y)
{
    if (active_quick_page == NULL) {
        return;
    }

    if (delta_y >= UI_HEIGHT / 2) {
        request_camera_page_open();
    } else {
        quick_page_drag_anim_to(0, NULL);
    }
}

static void quick_page_gesture_cb(lv_event_t * e)
{
    lv_event_code_t code = lv_event_get_code(e);
    lv_indev_t * indev = lv_indev_get_act();

    if (indev == NULL) {
        return;
    }

    if (code == LV_EVENT_CLICKED) {
        request_camera_page_open();
        return;
    }

    if (code == LV_EVENT_PRESSED) {
        lv_indev_get_point(indev, &back_swipe_start);
        back_swipe_from_bottom = back_swipe_start.y >= UI_HEIGHT - BACK_EDGE_TOUCH_H;
        back_swipe_tracking = back_swipe_from_bottom;
        return;
    }

    if (code == LV_EVENT_PRESSING) {
        lv_point_t now;
        lv_indev_get_point(indev, &now);
        if (back_swipe_tracking) {
            quick_page_set_drag_delta(back_swipe_start.y - now.y);
        }
        return;
    }

    if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
        lv_point_t end;
        lv_indev_get_point(indev, &end);
        if (back_swipe_tracking) {
            quick_page_finish_drag(back_swipe_start.y - end.y);
        }
        back_swipe_tracking = false;
        back_swipe_from_bottom = false;
        return;
    }

    if (code == LV_EVENT_GESTURE) {
        lv_dir_t dir = lv_indev_get_gesture_dir(indev);
        if (dir == LV_DIR_TOP) {
            request_camera_page_open();
        }
        return;
    }
}

static void bottom_back_edge_cb(lv_event_t * e)
{
    if (lv_event_get_code(e) == LV_EVENT_CLICKED) {
        request_camera_page_open();
        return;
    }

    quick_page_gesture_cb(e);
}

static void add_icon_tile(lv_obj_t * parent, const char * icon, int32_t col, int32_t row)
{
    lv_obj_t * tile = lv_btn_create(parent);
    lv_obj_set_size(tile, 92, 92);
    lv_obj_set_pos(tile, col * 112, row * 112);
    lv_obj_set_style_bg_color(tile, lv_color_make(66, 76, 96), 0);
    lv_obj_set_style_bg_opa(tile, 222, 0);
    lv_obj_set_style_border_width(tile, 0, 0);
    lv_obj_set_style_radius(tile, 14, 0);
    lv_obj_set_style_shadow_width(tile, 0, 0);
    lv_obj_set_style_pad_all(tile, 0, 0);

    lv_obj_t * label = label_create(tile, icon, lv_color_white(), UI_ICON_FONT);
    lv_obj_set_style_text_opa(label, 245, 0);
    lv_obj_center(label);
}

static void populate_quick_settings_content(lv_obj_t * preview, bool enable_back)
{
    lv_obj_t * overlay = lv_obj_create(preview);
    lv_obj_set_size(overlay, UI_WIDTH, UI_HEIGHT);
    lv_obj_align(overlay, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(overlay, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(overlay, 154, 0);
    lv_obj_set_style_border_width(overlay, 0, 0);
    lv_obj_set_style_radius(overlay, 0, 0);
    lv_obj_set_style_pad_all(overlay, 0, 0);
    lv_obj_clear_flag(overlay, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t * grid = lv_obj_create(preview);
    lv_obj_set_size(grid, 428, 204);
    lv_obj_align(grid, LV_ALIGN_CENTER, 0, -10);
    lv_obj_set_style_bg_opa(grid, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(grid, 0, 0);
    lv_obj_set_style_radius(grid, 0, 0);
    lv_obj_set_style_pad_all(grid, 0, 0);

    add_icon_tile(grid, LV_SYMBOL_HOME, 0, 0);
    add_icon_tile(grid, LV_SYMBOL_LOOP, 1, 0);
    add_icon_tile(grid, LV_SYMBOL_SETTINGS, 2, 0);
    add_icon_tile(grid, LV_SYMBOL_EYE_OPEN, 3, 0);
    add_icon_tile(grid, LV_SYMBOL_POWER, 0, 1);
    add_icon_tile(grid, LV_SYMBOL_IMAGE, 1, 1);
    add_icon_tile(grid, LV_SYMBOL_GPS, 2, 1);
    add_icon_tile(grid, LV_SYMBOL_AUDIO, 3, 1);

    lv_obj_t * indicator = lv_obj_create(preview);
    lv_obj_set_size(indicator, 86, 7);
    lv_obj_align(indicator, LV_ALIGN_BOTTOM_MID, 0, -50);
    lv_obj_set_style_bg_color(indicator, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(indicator, 220, 0);
    lv_obj_set_style_border_width(indicator, 0, 0);
    lv_obj_set_style_radius(indicator, 4, 0);
    lv_obj_add_flag(indicator, LV_OBJ_FLAG_CLICKABLE);
    if (enable_back) {
        lv_obj_add_event_cb(indicator, bottom_back_edge_cb, LV_EVENT_CLICKED, NULL);
    }

    if (!enable_back) {
        return;
    }

    lv_obj_t * top_edge_handle = lv_obj_create(preview);
    lv_obj_set_size(top_edge_handle, UI_WIDTH, BACK_EDGE_TOUCH_H);
    lv_obj_align(top_edge_handle, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_bg_opa(top_edge_handle, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(top_edge_handle, 0, 0);
    lv_obj_set_style_radius(top_edge_handle, 0, 0);
    lv_obj_add_flag(top_edge_handle, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(top_edge_handle, quick_page_gesture_cb, LV_EVENT_ALL, NULL);
    lv_obj_move_foreground(top_edge_handle);

    lv_obj_t * bottom_edge_handle = lv_obj_create(preview);
    lv_obj_set_size(bottom_edge_handle, UI_WIDTH, BACK_EDGE_TOUCH_H);
    lv_obj_align(bottom_edge_handle, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_opa(bottom_edge_handle, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(bottom_edge_handle, 0, 0);
    lv_obj_set_style_radius(bottom_edge_handle, 0, 0);
    lv_obj_add_flag(bottom_edge_handle, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(bottom_edge_handle, bottom_back_edge_cb, LV_EVENT_ALL, NULL);
    lv_obj_move_foreground(bottom_edge_handle);

    lv_obj_move_foreground(indicator);
}

lv_obj_t * quick_settings_page_create_curtain(lv_obj_t * parent)
{
    lv_obj_t * preview = lv_obj_create(parent);
    active_quick_page = preview;
    lv_obj_set_size(preview, UI_WIDTH, UI_HEIGHT);
    lv_obj_set_pos(preview, 0, 0);
    lv_obj_set_style_bg_color(preview, lv_color_make(42, 44, 48), 0);
    lv_obj_set_style_bg_grad_color(preview, lv_color_make(18, 18, 20), 0);
    lv_obj_set_style_bg_grad_dir(preview, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_border_width(preview, 0, 0);
    lv_obj_set_style_radius(preview, 0, 0);
    lv_obj_set_style_pad_all(preview, 0, 0);
    lv_obj_add_flag(preview, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(preview, quick_page_gesture_cb, LV_EVENT_ALL, NULL);

    populate_quick_settings_content(preview, true);
    return preview;
}

void quick_settings_page_create(uint32_t screen_w, uint32_t screen_h)
{
    camera_open_pending = false;
    back_swipe_tracking = false;
    back_swipe_from_bottom = false;
    active_quick_page = NULL;
    lv_obj_clean(lv_scr_act());

    lv_obj_t * root = lv_obj_create(lv_scr_act());
    lv_obj_set_size(root, screen_w, screen_h);
    lv_obj_align(root, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(root, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(root, 255, 0);
    lv_obj_set_style_border_width(root, 0, 0);
    lv_obj_set_style_radius(root, 0, 0);
    lv_obj_set_style_pad_all(root, 0, 0);
    lv_obj_add_flag(root, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(root, quick_page_gesture_cb, LV_EVENT_ALL, NULL);

    lv_obj_t * preview = quick_settings_page_create_curtain(root);
    lv_obj_align(preview, LV_ALIGN_CENTER, 0, 0);
}
