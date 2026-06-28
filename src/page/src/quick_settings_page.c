#include "../inc/quick_settings_page.h"
#include "../inc/camera_page.h"
#include "../../wifi/inc/wifi_manager.h"
#include "lvgl/lvgl.h"
#include "lvgl/src/extra/lv_extra.h"

#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define UI_TEXT_FONT &lv_font_montserrat_14
#define UI_ICON_FONT &lv_font_montserrat_48
#define UI_TITLE_FONT &lv_font_montserrat_28
#define BACK_EDGE_TOUCH_H 96
#define MAX_SCAN_ITEMS 16
#define MAX_VISIBLE_DEVICE_ITEMS 5
#define DEVICE_LIST_W 760
#define DEVICE_LIST_H 350
#define DEVICE_ROW_H 70

static bool back_swipe_tracking = false;
static bool back_swipe_from_bottom = false;
static lv_point_t back_swipe_start;
static bool camera_open_pending = false;
static lv_obj_t *active_quick_page = NULL;
static lv_obj_t *device_page = NULL;
static lv_obj_t *device_list = NULL;
static lv_obj_t *device_status_label = NULL;
static lv_obj_t *device_result_label = NULL;
static lv_obj_t *password_dialog = NULL;
static lv_obj_t *password_textarea = NULL;
static lv_obj_t *password_keyboard = NULL;
static lv_timer_t *device_scan_start_timer = NULL;
static lv_obj_t *quick_grid = NULL;
static lv_obj_t *device_rows[MAX_SCAN_ITEMS];
static char pending_wifi_ssid[96];

typedef enum {
    DEVICE_PAGE_WIFI,
    DEVICE_PAGE_BLUETOOTH,
} device_page_kind_t;

typedef struct {
    char title[128];
    char detail[128];
    char id[96];
    bool needs_password;
} scan_item_t;

typedef enum {
    DEVICE_TASK_NONE,
    DEVICE_TASK_WIFI_SCAN,
    DEVICE_TASK_BLUETOOTH_SCAN,
    DEVICE_TASK_WIFI_CONNECT,
    DEVICE_TASK_BLUETOOTH_CONNECT,
} device_task_type_t;

static scan_item_t scan_items[MAX_SCAN_ITEMS];
static size_t scan_item_count = 0;
static device_page_kind_t active_device_page_kind = DEVICE_PAGE_WIFI;
static device_page_kind_t pending_device_page_kind = DEVICE_PAGE_WIFI;
static bool device_page_open_pending = false;
static pthread_mutex_t device_worker_mutex = PTHREAD_MUTEX_INITIALIZER;
static bool device_worker_busy = false;
static bool device_worker_done = false;
static device_task_type_t device_worker_task = DEVICE_TASK_NONE;
static scan_item_t device_worker_items[MAX_SCAN_ITEMS];
static size_t device_worker_item_count = 0;
static char device_worker_status[128];

static void request_camera_page_open(void);
static void device_page_close(void);
static void device_scan_start_timer_cb(lv_timer_t * timer);
static void device_page_open(device_page_kind_t kind);
static void device_page_request_open(device_page_kind_t kind);
static bool quick_grid_hit_device_tile(const lv_point_t * point,
                                       device_page_kind_t * kind);
static bool quick_grid_open_device_from_point(const lv_point_t * point);

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

static void shell_quote_append(char * out, size_t out_size, const char * text)
{
    size_t used = strlen(out);

    if (used + 2 >= out_size) return;
    out[used++] = '\'';
    out[used] = '\0';

    for (const char * p = text; *p != '\0' && used + 5 < out_size; p++) {
        if (*p == '\'') {
            snprintf(out + used, out_size - used, "'\\''");
            used = strlen(out);
        } else {
            out[used++] = *p;
            out[used] = '\0';
        }
    }

    if (used + 2 < out_size) {
        out[used++] = '\'';
        out[used] = '\0';
    }
}

static void text_trim(char * text)
{
    char * start = text;
    char * end;

    while (*start == ' ' || *start == '\t' || *start == '\r' || *start == '\n') {
        start++;
    }
    if (start != text) {
        memmove(text, start, strlen(start) + 1);
    }

    end = text + strlen(text);
    while (end > text &&
           (end[-1] == ' ' || end[-1] == '\t' || end[-1] == '\r' || end[-1] == '\n')) {
        *--end = '\0';
    }
}

static bool command_run(const char * command)
{
    int rc = system(command);
    return rc == 0;
}

static void status_set(const char * text)
{
    if (device_status_label != NULL) {
        lv_label_set_text(device_status_label, text);
    }
}

static bool scan_item_add(scan_item_t * items,
                          size_t * count,
                          const char * title,
                          const char * detail,
                          const char * id,
                          bool needs_password)
{
    scan_item_t * item;

    if (*count >= MAX_SCAN_ITEMS || title == NULL || title[0] == '\0') {
        return false;
    }

    item = &items[(*count)++];
    snprintf(item->title, sizeof(item->title), "%s", title);
    snprintf(item->detail, sizeof(item->detail), "%s", detail != NULL ? detail : "");
    snprintf(item->id, sizeof(item->id), "%s", id != NULL ? id : title);
    item->needs_password = needs_password;
    return true;
}

static size_t bluetooth_scan(scan_item_t * items, size_t max_count)
{
#ifdef WSL
    memset(items, 0, max_count * sizeof(items[0]));
    return 0;
#else
    FILE * fp;
    char line[256];
    size_t count = 0;

    memset(items, 0, max_count * sizeof(items[0]));
    command_run("bluetoothctl --timeout 5 scan on >/dev/null 2>&1");
    fp = popen("bluetoothctl devices 2>/dev/null", "r");
    if (fp == NULL) {
        return 0;
    }

    while (fgets(line, sizeof(line), fp) != NULL && count < max_count) {
        char mac[32];
        char name[128];
        char * p = line;

        text_trim(line);
        if (strncmp(line, "Device ", 7) != 0) continue;
        p += 7;
        if (sscanf(p, "%31s %127[^\n]", mac, name) != 2) continue;
        scan_item_add(items, &count, name, mac, mac, false);
    }
    pclose(fp);
    return count;
#endif
}

static bool bluetooth_connect(const char * mac)
{
#ifdef WSL
    (void)mac;
    return false;
#else
    char command[192] = "bluetoothctl connect ";

    shell_quote_append(command, sizeof(command), mac);
    strncat(command, " >/dev/null 2>&1", sizeof(command) - strlen(command) - 1);
    return command_run(command);
#endif
}

typedef struct {
    device_task_type_t task;
    char id[96];
    char password[96];
} device_worker_request_t;

static void *device_worker_main(void * data)
{
    device_worker_request_t * request = data;
    scan_item_t items[MAX_SCAN_ITEMS];
    size_t item_count = 0;
    char status[128];
    bool ok = false;

    status[0] = '\0';
    memset(items, 0, sizeof(items));

    if (request->task == DEVICE_TASK_WIFI_SCAN) {
        wifi_manager_network_t wifi_items[MAX_SCAN_ITEMS];
        size_t wifi_count = wifi_manager_scan(wifi_items, MAX_SCAN_ITEMS);
        for (size_t i = 0; i < wifi_count && item_count < MAX_SCAN_ITEMS; i++) {
            scan_item_add(items,
                          &item_count,
                          wifi_items[i].ssid,
                          wifi_items[i].detail,
                          wifi_items[i].ssid,
                          wifi_items[i].needs_password);
        }
        snprintf(status,
                 sizeof(status),
                 "%s",
                 item_count > 0 ? "WiFi scan complete" : "No WiFi found or iw unavailable");
    } else if (request->task == DEVICE_TASK_BLUETOOTH_SCAN) {
        item_count = bluetooth_scan(items, MAX_SCAN_ITEMS);
        snprintf(status,
                 sizeof(status),
                 "%s",
                 item_count > 0 ? "Bluetooth scan complete" :
                                  "No Bluetooth device found or bluetoothctl unavailable");
    } else if (request->task == DEVICE_TASK_WIFI_CONNECT) {
        ok = wifi_manager_connect(request->id, request->password);
        snprintf(status, sizeof(status), "%s", ok ? "WiFi connected" : "WiFi connect failed");
    } else if (request->task == DEVICE_TASK_BLUETOOTH_CONNECT) {
        ok = bluetooth_connect(request->id);
        snprintf(status,
                 sizeof(status),
                 "%s",
                 ok ? "Bluetooth connected" : "Bluetooth connect failed");
    }

    pthread_mutex_lock(&device_worker_mutex);
    device_worker_task = request->task;
    memcpy(device_worker_items, items, sizeof(device_worker_items));
    device_worker_item_count = item_count;
    snprintf(device_worker_status, sizeof(device_worker_status), "%s", status);
    device_worker_busy = false;
    device_worker_done = true;
    pthread_mutex_unlock(&device_worker_mutex);

    free(request);
    return NULL;
}

static bool device_worker_start(device_task_type_t task,
                                const char * id,
                                const char * password)
{
    pthread_t thread;
    device_worker_request_t * request;

    pthread_mutex_lock(&device_worker_mutex);
    if (device_worker_busy) {
        pthread_mutex_unlock(&device_worker_mutex);
        return false;
    }
    device_worker_busy = true;
    device_worker_done = false;
    device_worker_task = task;
    device_worker_item_count = 0;
    device_worker_status[0] = '\0';
    memset(device_worker_items, 0, sizeof(device_worker_items));
    pthread_mutex_unlock(&device_worker_mutex);

    request = calloc(1, sizeof(*request));
    if (request == NULL) {
        pthread_mutex_lock(&device_worker_mutex);
        device_worker_busy = false;
        pthread_mutex_unlock(&device_worker_mutex);
        return false;
    }

    request->task = task;
    if (id != NULL) {
        snprintf(request->id, sizeof(request->id), "%s", id);
    }
    if (password != NULL) {
        snprintf(request->password, sizeof(request->password), "%s", password);
    }

    if (pthread_create(&thread, NULL, device_worker_main, request) != 0) {
        free(request);
        pthread_mutex_lock(&device_worker_mutex);
        device_worker_busy = false;
        pthread_mutex_unlock(&device_worker_mutex);
        return false;
    }
    pthread_detach(thread);
    return true;
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

    device_page_close();
    camera_open_pending = true;
    device_page_open_pending = false;
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
    lv_point_t point;

    if (indev == NULL) {
        return;
    }

    if (device_page != NULL) {
        lv_event_stop_bubbling(e);
        return;
    }

    if (code == LV_EVENT_CLICKED) {
        lv_indev_get_point(indev, &point);
        if (quick_grid_open_device_from_point(&point)) {
            return;
        }
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

static bool quick_grid_hit_device_tile(const lv_point_t * point,
                                       device_page_kind_t * kind)
{
    lv_area_t area;
    int32_t local_x;
    int32_t local_y;

    if (quick_grid == NULL || point == NULL) {
        return false;
    }

    lv_obj_get_coords(quick_grid, &area);
    local_x = point->x - area.x1;
    local_y = point->y - area.y1;

    if (local_y < 96 || local_y > 214) {
        return false;
    }

    if (local_x >= 204 && local_x <= 326) {
        if (kind != NULL) {
            *kind = DEVICE_PAGE_WIFI;
        }
        return true;
    }

    if (local_x >= 326 && local_x <= 448) {
        if (kind != NULL) {
            *kind = DEVICE_PAGE_BLUETOOTH;
        }
        return true;
    }

    return false;
}

static bool quick_grid_open_device_from_point(const lv_point_t * point)
{
    device_page_kind_t kind;

    if (device_page != NULL || device_page_open_pending ||
        !quick_grid_hit_device_tile(point, &kind)) {
        return false;
    }

    printf("%s tile clicked\n", kind == DEVICE_PAGE_WIFI ? "WiFi" : "Bluetooth");
    fflush(stdout);
    device_page_request_open(kind);
    return true;
}

static void quick_grid_event_cb(lv_event_t * e)
{
    lv_event_code_t code = lv_event_get_code(e);
    lv_indev_t * indev = lv_indev_get_act();
    lv_point_t point;

    if (code != LV_EVENT_CLICKED || indev == NULL) {
        return;
    }

    lv_indev_get_point(indev, &point);
    if (quick_grid_open_device_from_point(&point)) {
        lv_event_stop_bubbling(e);
    }
}

void quick_settings_page_pointer_sample(bool pressed, int32_t x, int32_t y)
{
    static bool pending_device_tile = false;
    static device_page_kind_t pending_device_tile_kind = DEVICE_PAGE_WIFI;
    static uint32_t last_trigger_ms = 0;
    uint32_t now_ms;

    if (active_quick_page == NULL || device_page != NULL || device_page_open_pending) {
        pending_device_tile = false;
        return;
    }

    now_ms = lv_tick_get();
    if (last_trigger_ms != 0 && now_ms - last_trigger_ms < 350) {
        return;
    }

    lv_point_t point = {.x = x, .y = y};

    if (pressed) {
        pending_device_tile =
            quick_grid_hit_device_tile(&point, &pending_device_tile_kind);
        return;
    }

    if (pending_device_tile) {
        device_page_kind_t release_kind;
        pending_device_tile = false;
        if (quick_grid_hit_device_tile(&point, &release_kind) &&
            release_kind == pending_device_tile_kind &&
            quick_grid_open_device_from_point(&point)) {
            last_trigger_ms = now_ms;
        }
    }
}

static lv_obj_t *small_button_create(lv_obj_t * parent, const char * text, int32_t w, int32_t h)
{
    lv_obj_t * btn = lv_obj_create(parent);
    lv_obj_set_size(btn, w, h);
    lv_obj_set_style_bg_color(btn, lv_color_make(64, 74, 92), 0);
    lv_obj_set_style_bg_opa(btn, 230, 0);
    lv_obj_set_style_border_width(btn, 0, 0);
    lv_obj_set_style_radius(btn, 8, 0);
    lv_obj_set_style_shadow_width(btn, 0, 0);
    lv_obj_set_style_pad_all(btn, 0, 0);
    lv_obj_add_flag(btn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(btn, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_SCROLL_CHAIN);

    lv_obj_t * label = label_create(btn, text, lv_color_white(), UI_TEXT_FONT);
    lv_obj_center(label);
    return btn;
}

static void password_dialog_close(void)
{
    if (password_dialog != NULL) {
        lv_obj_del(password_dialog);
    }
    password_dialog = NULL;
    password_textarea = NULL;
    password_keyboard = NULL;
    pending_wifi_ssid[0] = '\0';
}

static void password_cancel_cb(lv_event_t * e)
{
    if (lv_event_get_code(e) == LV_EVENT_CLICKED) {
        password_dialog_close();
    }
}

static void password_connect_cb(lv_event_t * e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED || password_textarea == NULL) {
        return;
    }

    const char * password = lv_textarea_get_text(password_textarea);
    status_set("Connecting WiFi...");
    if (!device_worker_start(DEVICE_TASK_WIFI_CONNECT, pending_wifi_ssid, password)) {
        status_set("Device task is busy");
    }
    password_dialog_close();
}

static void password_dialog_open(const char * ssid)
{
    if (active_quick_page == NULL) {
        return;
    }

    password_dialog_close();
    snprintf(pending_wifi_ssid, sizeof(pending_wifi_ssid), "%s", ssid);

    password_dialog = lv_obj_create(active_quick_page);
    lv_obj_set_size(password_dialog, UI_WIDTH, UI_HEIGHT);
    lv_obj_align(password_dialog, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(password_dialog, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(password_dialog, 210, 0);
    lv_obj_set_style_border_width(password_dialog, 0, 0);
    lv_obj_set_style_radius(password_dialog, 0, 0);
    lv_obj_set_style_pad_all(password_dialog, 0, 0);
    lv_obj_clear_flag(password_dialog, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(password_dialog, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t * panel = lv_obj_create(password_dialog);
    lv_obj_set_size(panel, 560, 176);
    lv_obj_align(panel, LV_ALIGN_TOP_MID, 0, 36);
    lv_obj_set_style_bg_color(panel, lv_color_make(32, 38, 48), 0);
    lv_obj_set_style_bg_opa(panel, 245, 0);
    lv_obj_set_style_border_width(panel, 0, 0);
    lv_obj_set_style_radius(panel, 10, 0);
    lv_obj_set_style_pad_all(panel, 18, 0);
    lv_obj_clear_flag(panel, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t * title = label_create(panel, "WiFi Password", lv_color_white(), UI_TEXT_FONT);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 4, 0);

    lv_obj_t * ssid_label = label_create(panel, ssid, lv_color_make(210, 220, 232), UI_TEXT_FONT);
    lv_obj_set_width(ssid_label, 500);
    lv_label_set_long_mode(ssid_label, LV_LABEL_LONG_DOT);
    lv_obj_align(ssid_label, LV_ALIGN_TOP_LEFT, 4, 28);

    password_textarea = lv_textarea_create(panel);
    lv_obj_set_size(password_textarea, 348, 46);
    lv_obj_align(password_textarea, LV_ALIGN_BOTTOM_LEFT, 4, -2);
    lv_textarea_set_one_line(password_textarea, true);
    lv_textarea_set_password_mode(password_textarea, true);
    lv_textarea_set_placeholder_text(password_textarea, "password");

    lv_obj_t * connect_btn = small_button_create(panel, "Connect", 92, 46);
    lv_obj_align(connect_btn, LV_ALIGN_BOTTOM_RIGHT, -104, -2);
    lv_obj_add_event_cb(connect_btn, password_connect_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t * cancel_btn = small_button_create(panel, "Cancel", 82, 46);
    lv_obj_align(cancel_btn, LV_ALIGN_BOTTOM_RIGHT, 0, -2);
    lv_obj_add_event_cb(cancel_btn, password_cancel_cb, LV_EVENT_CLICKED, NULL);

    password_keyboard = lv_keyboard_create(password_dialog);
    lv_obj_set_size(password_keyboard, UI_WIDTH, 232);
    lv_obj_align(password_keyboard, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_keyboard_set_textarea(password_keyboard, password_textarea);
}

static void device_page_close(void)
{
    device_page_open_pending = false;
    if (device_scan_start_timer != NULL) {
        lv_timer_del(device_scan_start_timer);
        device_scan_start_timer = NULL;
    }
    password_dialog_close();
    if (device_page != NULL) {
        lv_obj_del(device_page);
    }
    device_page = NULL;
    device_list = NULL;
    device_status_label = NULL;
    device_result_label = NULL;
    memset(device_rows, 0, sizeof(device_rows));
}

static void device_close_cb(lv_event_t * e)
{
    if (lv_event_get_code(e) == LV_EVENT_CLICKED) {
        device_page_close();
    }
}

static void device_item_cb(lv_event_t * e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) {
        return;
    }

    uintptr_t index = (uintptr_t)lv_event_get_user_data(e);
    if (index >= scan_item_count) {
        return;
    }

    scan_item_t * item = &scan_items[index];
    if (active_device_page_kind == DEVICE_PAGE_WIFI) {
        if (item->needs_password) {
            password_dialog_open(item->id);
            return;
        }
        status_set("Connecting WiFi...");
        if (!device_worker_start(DEVICE_TASK_WIFI_CONNECT, item->id, NULL)) {
            status_set("Device task is busy");
        }
    } else {
        status_set("Connecting Bluetooth...");
        if (!device_worker_start(DEVICE_TASK_BLUETOOTH_CONNECT, item->id, NULL)) {
            status_set("Device task is busy");
        }
    }
}

static lv_obj_t *device_icon_button_create(lv_obj_t * parent, const char * icon)
{
    lv_obj_t * btn = lv_btn_create(parent);
    lv_obj_set_size(btn, 44, 44);
    lv_obj_set_style_bg_color(btn, lv_color_make(50, 58, 72), 0);
    lv_obj_set_style_bg_opa(btn, 0, 0);
    lv_obj_set_style_border_width(btn, 0, 0);
    lv_obj_set_style_shadow_width(btn, 0, 0);
    lv_obj_set_style_radius(btn, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_pad_all(btn, 0, 0);
    lv_obj_clear_flag(btn, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_SCROLL_CHAIN);

    lv_obj_t * label = label_create(btn, icon, lv_color_make(116, 177, 255), UI_TEXT_FONT);
    lv_obj_center(label);
    return btn;
}

static void device_list_add_item(size_t index)
{
    scan_item_t * item = &scan_items[index];
    lv_obj_t * row;
    lv_obj_t * icon_box;
    lv_obj_t * icon;
    lv_obj_t * title;
    lv_obj_t * detail;
    lv_obj_t * arrow;

    if (index >= MAX_SCAN_ITEMS || device_list == NULL) {
        return;
    }
    row = lv_obj_create(device_page);
    device_rows[index] = row;
    lv_obj_set_size(row, DEVICE_LIST_W, DEVICE_ROW_H);
    lv_obj_set_pos(row, (UI_WIDTH - DEVICE_LIST_W) / 2, 118 + (lv_coord_t)index * DEVICE_ROW_H);
    lv_obj_set_style_bg_color(row, lv_color_make(35, 42, 54), 0);
    lv_obj_set_style_bg_opa(row, 245, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_radius(row, 0, 0);
    lv_obj_set_style_pad_all(row, 0, 0);
    lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_SCROLL_CHAIN);
    lv_obj_add_event_cb(row, device_item_cb, LV_EVENT_CLICKED, (void *)(uintptr_t)index);

    icon_box = lv_obj_create(row);
    lv_obj_set_size(icon_box, 50, 50);
    lv_obj_align(icon_box, LV_ALIGN_LEFT_MID, 20, 0);
    lv_obj_set_style_bg_color(icon_box,
                              active_device_page_kind == DEVICE_PAGE_WIFI ?
                                  lv_color_make(0, 122, 255) :
                                  lv_color_make(52, 120, 246),
                              0);
    lv_obj_set_style_bg_opa(icon_box, 255, 0);
    lv_obj_set_style_border_width(icon_box, 0, 0);
    lv_obj_set_style_radius(icon_box, 8, 0);
    lv_obj_set_style_pad_all(icon_box, 0, 0);
    lv_obj_clear_flag(icon_box, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);

    icon = label_create(icon_box,
                        active_device_page_kind == DEVICE_PAGE_WIFI ? LV_SYMBOL_WIFI :
                                                                      LV_SYMBOL_BLUETOOTH,
                        lv_color_white(),
                        &lv_font_montserrat_36);
    lv_obj_center(icon);

    title = label_create(row, item->title, lv_color_make(244, 247, 252), &lv_font_montserrat_36);
    lv_obj_set_width(title, 580);
    lv_label_set_long_mode(title, LV_LABEL_LONG_DOT);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 88, 0);

    detail = label_create(row, item->detail, lv_color_make(158, 168, 184), UI_TEXT_FONT);
    lv_obj_set_width(detail, 580);
    lv_label_set_long_mode(detail, LV_LABEL_LONG_DOT);
    lv_obj_align(detail, LV_ALIGN_TOP_LEFT, 90, 50);

    arrow = label_create(row, LV_SYMBOL_RIGHT, lv_color_make(118, 130, 148), UI_TEXT_FONT);
    lv_obj_align(arrow, LV_ALIGN_RIGHT_MID, -18, 0);
}

static void device_page_refresh(void)
{
    device_task_type_t task;

    if (device_page == NULL) {
        return;
    }

    status_set(active_device_page_kind == DEVICE_PAGE_WIFI ? "Scanning WiFi..." : "Scanning Bluetooth...");

    scan_item_count = 0;
    memset(scan_items, 0, sizeof(scan_items));
    for (size_t i = 0; i < MAX_SCAN_ITEMS; i++) {
        if (device_rows[i] != NULL) {
            lv_obj_del(device_rows[i]);
            device_rows[i] = NULL;
        }
    }
    if (device_result_label != NULL) {
        lv_label_set_text(device_result_label,
                          active_device_page_kind == DEVICE_PAGE_WIFI ?
                              "Searching for nearby networks..." :
                              "Searching for nearby devices...");
        lv_obj_add_flag(device_result_label, LV_OBJ_FLAG_HIDDEN);
    }

    if (device_scan_start_timer != NULL) {
        lv_timer_del(device_scan_start_timer);
        device_scan_start_timer = NULL;
    }
    task = active_device_page_kind == DEVICE_PAGE_WIFI ? DEVICE_TASK_WIFI_SCAN :
                                                         DEVICE_TASK_BLUETOOTH_SCAN;
    printf("Start %s scan\n", task == DEVICE_TASK_WIFI_SCAN ? "WiFi" : "Bluetooth");
    fflush(stdout);
    if (!device_worker_start(task, NULL, NULL)) {
        status_set("Device task is busy");
    }
}

static void device_scan_start_timer_cb(lv_timer_t * timer)
{
    (void)timer;

    if (device_scan_start_timer != NULL) {
        lv_timer_del(device_scan_start_timer);
        device_scan_start_timer = NULL;
    }
    if (device_page == NULL) {
        return;
    }

    device_task_type_t task =
        active_device_page_kind == DEVICE_PAGE_WIFI ? DEVICE_TASK_WIFI_SCAN :
                                                      DEVICE_TASK_BLUETOOTH_SCAN;
    printf("Start %s scan\n", task == DEVICE_TASK_WIFI_SCAN ? "WiFi" : "Bluetooth");
    fflush(stdout);
    if (!device_worker_start(task, NULL, NULL)) {
        status_set("Device task is busy");
    }
}

void quick_settings_page_poll(void)
{
    bool done;
    device_task_type_t task;
    scan_item_t items[MAX_SCAN_ITEMS];
    size_t item_count;
    char status[128];

    if (device_page_open_pending && device_page == NULL) {
        device_page_kind_t kind = pending_device_page_kind;
        device_page_open_pending = false;
        device_page_open(kind);
    }

    pthread_mutex_lock(&device_worker_mutex);
    done = device_worker_done;
    if (!done) {
        pthread_mutex_unlock(&device_worker_mutex);
        return;
    }

    task = device_worker_task;
    memcpy(items, device_worker_items, sizeof(items));
    item_count = device_worker_item_count;
    snprintf(status, sizeof(status), "%s", device_worker_status);
    device_worker_done = false;
    device_worker_task = DEVICE_TASK_NONE;
    pthread_mutex_unlock(&device_worker_mutex);

    if (device_page == NULL) {
        return;
    }

    status_set(status);

    if (task != DEVICE_TASK_WIFI_SCAN && task != DEVICE_TASK_BLUETOOTH_SCAN) {
        return;
    }

    if ((task == DEVICE_TASK_WIFI_SCAN && active_device_page_kind != DEVICE_PAGE_WIFI) ||
        (task == DEVICE_TASK_BLUETOOTH_SCAN && active_device_page_kind != DEVICE_PAGE_BLUETOOTH)) {
        return;
    }

    memcpy(scan_items, items, sizeof(scan_items));
    scan_item_count = item_count;
    if (device_result_label != NULL) {
        lv_obj_add_flag(device_result_label, LV_OBJ_FLAG_HIDDEN);
    }
    if (scan_item_count > 0) {
        for (size_t i = 0; i < MAX_SCAN_ITEMS; i++) {
            if (device_rows[i] != NULL) {
                lv_obj_del(device_rows[i]);
                device_rows[i] = NULL;
            }
        }
        size_t visible_count = scan_item_count < MAX_VISIBLE_DEVICE_ITEMS ?
                                   scan_item_count :
                                   MAX_VISIBLE_DEVICE_ITEMS;
        for (size_t i = 0; i < visible_count; i++) {
            device_list_add_item(i);
        }
    } else if (device_result_label != NULL) {
        lv_label_set_text(device_result_label,
                          active_device_page_kind == DEVICE_PAGE_WIFI ?
                              "No WiFi network found" :
                              "No Bluetooth device found");
        lv_obj_clear_flag(device_result_label, LV_OBJ_FLAG_HIDDEN);
    }
}

static void device_refresh_cb(lv_event_t * e)
{
    if (lv_event_get_code(e) == LV_EVENT_CLICKED) {
        device_page_refresh();
    }
}

static void device_page_request_open(device_page_kind_t kind)
{
    if (device_page != NULL) {
        active_device_page_kind = kind;
        lv_obj_move_foreground(device_page);
        return;
    }

    pending_device_page_kind = kind;
    if (device_page_open_pending) {
        return;
    }

    device_page_open_pending = true;
}

static void device_page_open(device_page_kind_t kind)
{
    if (active_quick_page == NULL) {
        device_page_open_pending = false;
        return;
    }

    printf("Open %s page\n", kind == DEVICE_PAGE_WIFI ? "WiFi" : "Bluetooth");
    fflush(stdout);

    active_device_page_kind = kind;
    device_page_close();

    device_page = lv_obj_create(active_quick_page);
    lv_obj_set_size(device_page, UI_WIDTH, UI_HEIGHT);
    lv_obj_set_pos(device_page, 0, 0);
    lv_obj_set_style_bg_color(device_page, lv_color_make(18, 22, 30), 0);
    lv_obj_set_style_bg_opa(device_page, 255, 0);
    lv_obj_set_style_border_width(device_page, 0, 0);
    lv_obj_set_style_radius(device_page, 0, 0);
    lv_obj_set_style_pad_all(device_page, 0, 0);
    lv_obj_clear_flag(device_page, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_SCROLL_CHAIN);
    lv_obj_move_foreground(device_page);

    lv_obj_t * header = lv_obj_create(device_page);
    lv_obj_set_size(header, UI_WIDTH, 82);
    lv_obj_set_pos(header, 0, 0);
    lv_obj_set_style_bg_color(header, lv_color_make(22, 27, 36), 0);
    lv_obj_set_style_bg_opa(header, 255, 0);
    lv_obj_set_style_border_width(header, 0, 0);
    lv_obj_set_style_radius(header, 0, 0);
    lv_obj_set_style_pad_all(header, 0, 0);
    lv_obj_clear_flag(header, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_SCROLL_CHAIN);

    lv_obj_t * back_btn = device_icon_button_create(header, LV_SYMBOL_LEFT);
    lv_obj_align(back_btn, LV_ALIGN_LEFT_MID, 18, 10);
    lv_obj_add_event_cb(back_btn, device_close_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t * refresh_btn = device_icon_button_create(header, LV_SYMBOL_REFRESH);
    lv_obj_align(refresh_btn, LV_ALIGN_RIGHT_MID, -18, 10);
    lv_obj_add_event_cb(refresh_btn, device_refresh_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t * title = label_create(header,
                                    kind == DEVICE_PAGE_WIFI ? "WiFi" : "Bluetooth",
                                    lv_color_make(244, 247, 252),
                                    UI_TITLE_FONT);
    lv_obj_align(title, LV_ALIGN_CENTER, 0, 10);

    device_status_label =
        label_create(device_page,
                     kind == DEVICE_PAGE_WIFI ? "Scanning WiFi..." : "Scanning Bluetooth...",
                     lv_color_make(150, 160, 176),
                     UI_TEXT_FONT);
    lv_obj_align(device_status_label, LV_ALIGN_TOP_MID, 0, 96);

    device_list = lv_obj_create(device_page);
    lv_obj_set_size(device_list, DEVICE_LIST_W, DEVICE_LIST_H);
    lv_obj_align(device_list, LV_ALIGN_TOP_MID, 0, 118);
    lv_obj_set_style_bg_color(device_list, lv_color_make(30, 36, 48), 0);
    lv_obj_set_style_bg_opa(device_list, 245, 0);
    lv_obj_set_style_border_width(device_list, 0, 0);
    lv_obj_set_style_radius(device_list, 8, 0);
    lv_obj_set_style_pad_all(device_list, 0, 0);
    lv_obj_set_style_pad_row(device_list, 0, 0);
    lv_obj_clear_flag(device_list, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_SCROLL_CHAIN);

    device_result_label =
        label_create(device_page,
                     kind == DEVICE_PAGE_WIFI ? "Searching for nearby networks..." :
                                                "Searching for nearby devices...",
                     lv_color_make(150, 160, 176),
                     UI_TEXT_FONT);
    lv_obj_set_size(device_result_label, 560, 52);
    lv_obj_set_style_text_align(device_result_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(device_result_label, LV_ALIGN_TOP_MID, 0, 246);

    device_scan_start_timer = lv_timer_create(device_scan_start_timer_cb, 80, NULL);
    lv_timer_set_repeat_count(device_scan_start_timer, 1);

    printf("%s page created\n", kind == DEVICE_PAGE_WIFI ? "WiFi" : "Bluetooth");
    fflush(stdout);
    lv_obj_invalidate(device_page);
}

static void wifi_tile_cb(lv_event_t * e)
{
    lv_event_stop_bubbling(e);
    if (lv_event_get_code(e) == LV_EVENT_CLICKED) {
        device_page_request_open(DEVICE_PAGE_WIFI);
    }
}

static void bluetooth_tile_cb(lv_event_t * e)
{
    lv_event_stop_bubbling(e);
    if (lv_event_get_code(e) == LV_EVENT_CLICKED) {
        device_page_request_open(DEVICE_PAGE_BLUETOOTH);
    }
}

static void add_icon_tile(lv_obj_t * parent,
                          const char * icon,
                          int32_t col,
                          int32_t row,
                          lv_event_cb_t event_cb)
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
    lv_obj_add_flag(tile, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(tile, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_SCROLL_CHAIN);
    if (event_cb != NULL) {
        lv_obj_add_event_cb(tile, event_cb, LV_EVENT_PRESSED, NULL);
        lv_obj_add_event_cb(tile, event_cb, LV_EVENT_CLICKED, NULL);
    }

    lv_obj_t * label = label_create(tile, icon, lv_color_white(), UI_ICON_FONT);
    lv_obj_set_style_text_opa(label, 245, 0);
    lv_obj_clear_flag(label, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_center(label);
    lv_obj_move_foreground(tile);
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

    quick_grid = lv_obj_create(preview);
    lv_obj_set_size(quick_grid, 428, 204);
    lv_obj_align(quick_grid, LV_ALIGN_CENTER, 0, -10);
    lv_obj_set_style_bg_opa(quick_grid, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(quick_grid, 0, 0);
    lv_obj_set_style_radius(quick_grid, 0, 0);
    lv_obj_set_style_pad_all(quick_grid, 0, 0);
    lv_obj_add_flag(quick_grid, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(quick_grid, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_SCROLL_CHAIN);
    lv_obj_add_event_cb(quick_grid, quick_grid_event_cb, LV_EVENT_ALL, NULL);

    add_icon_tile(quick_grid, LV_SYMBOL_HOME, 0, 0, NULL);
    add_icon_tile(quick_grid, LV_SYMBOL_LOOP, 1, 0, NULL);
    add_icon_tile(quick_grid, LV_SYMBOL_SETTINGS, 2, 0, NULL);
    add_icon_tile(quick_grid, LV_SYMBOL_EYE_OPEN, 3, 0, NULL);
    add_icon_tile(quick_grid, LV_SYMBOL_POWER, 0, 1, NULL);
    add_icon_tile(quick_grid, LV_SYMBOL_IMAGE, 1, 1, NULL);
    add_icon_tile(quick_grid, LV_SYMBOL_WIFI, 2, 1, wifi_tile_cb);
    add_icon_tile(quick_grid, LV_SYMBOL_BLUETOOTH, 3, 1, bluetooth_tile_cb);
    lv_obj_move_foreground(quick_grid);

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

    lv_obj_move_foreground(quick_grid);
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
    quick_grid = NULL;
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
