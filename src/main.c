#include "lvgl/lvgl.h"
#include "lvgl/src/extra/lv_extra.h"
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <stdbool.h>
#include <string.h>
#include <errno.h>
#include <math.h>
#include <sys/stat.h>
#include <time.h>
#include "page/inc/camera_page.h"
#include "camera_capture.h"
#include "camera_media.h"
#include "camera_recorder.h"
#include "drm_display.h"

// 条件宏配置开关：WSL 使用 SDL2，RK3566 使用 DRM/KMS。
#ifdef WSL
#define USE_SDL 1
#else
#define USE_SDL 0
#endif

static lv_color_t *camera_preview_pixels = NULL;
static lv_color_t *last_photo_pixels = NULL;
static lv_img_dsc_t camera_preview_image;
static lv_img_dsc_t last_photo_image;
static char last_video_device[64];
static bool preview_paused_for_recording = false;

#if !USE_SDL
static char *fbp;
static int fbdev_init(void);
#endif

#if !USE_SDL
static void camera_preview_timer_cb(lv_timer_t * timer)
{
    (void)timer;
    if (camera_preview_pixels != NULL &&
        camera_capture_copy_latest(camera_preview_pixels, UI_WIDTH * UI_HEIGHT)) {
        camera_page_refresh_preview();
    }
}
#endif

#if USE_SDL
static void create_wsl_default_image(lv_color_t * pixels)
{
    for (uint32_t y = 0; y < UI_HEIGHT; y++) {
        for (uint32_t x = 0; x < UI_WIDTH; x++) {
            int horizon = 250 + (int)(18.0 * sin((double)x / 95.0));
            lv_color_t color;

            if ((int)y < horizon) {
                uint8_t shade = (uint8_t)(42 + y * 74 / UI_HEIGHT);
                color = lv_color_make(shade / 2, shade, shade + 45);
            } else {
                uint8_t shade = (uint8_t)(72 - (y - horizon) / 8);
                color = lv_color_make(shade / 2, shade, shade / 2);
            }

            if (x > 300 && x < 500 && y > 150 && y < 330) {
                int dx = (int)x - 400;
                int dy = (int)y - 240;
                if (dx * dx + dy * dy < 72 * 72) {
                    color = lv_color_make(238, 184, 52);
                }
                if (dx * dx + dy * dy < 48 * 48) {
                    color = lv_color_make(34, 40, 44);
                }
            }
            pixels[(size_t)y * UI_WIDTH + x] = color;
        }
    }
}
#endif

static bool capture_current_photo(void * user_data)
{
    (void)user_data;
    char path[160];
    char timestamp[32];
    const char * photo_dir = getenv("MY_MOVE_CAMERA_PHOTO_DIR");
    time_t now = time(NULL);
    struct tm local_time;
    size_t pixel_count = (size_t)UI_WIDTH * UI_HEIGHT;

    if (camera_preview_pixels == NULL || last_photo_pixels == NULL) {
        return false;
    }

    if (photo_dir == NULL || photo_dir[0] == '\0') {
        photo_dir = "photos";
    }
    if (mkdir(photo_dir, 0755) != 0 && errno != EEXIST) {
        perror("Unable to create photos directory");
        return false;
    }

    localtime_r(&now, &local_time);
    strftime(timestamp, sizeof(timestamp), "%Y%m%d_%H%M%S", &local_time);
    snprintf(path, sizeof(path), "%s/IMG_%s.jpg", photo_dir, timestamp);

    memcpy(last_photo_pixels, camera_preview_pixels,
           pixel_count * sizeof(lv_color_t));
    if (camera_media_save_jpeg(path, last_photo_pixels,
                               UI_WIDTH, UI_HEIGHT, 90) != 0) {
        return false;
    }

    camera_page_set_album_image(&last_photo_image, path);
    printf("Photo saved as MJPEG frame: %s\n", path);
    return true;
}

static const char *video_dir_get(void)
{
    const char * video_dir = getenv("MY_MOVE_CAMERA_VIDEO_DIR");
    if (video_dir == NULL || video_dir[0] == '\0') {
        video_dir = "videos";
    }
    return video_dir;
}

static bool handle_recording(bool start, void * user_data)
{
    (void)user_data;

#if USE_SDL
    (void)start;
    return false;
#else
    if (start) {
        char path[160];
        char timestamp[32];
        const char * video_dir = video_dir_get();
        const char * preview_device_path = camera_capture_get_device_path();
        const char * requested_record_device = getenv("MY_MOVE_CAMERA_RECORD_VIDEO");
        char record_device_path[64];
        time_t now = time(NULL);
        struct tm local_time;

        if (preview_device_path == NULL || preview_device_path[0] == '\0') {
            preview_device_path = getenv("MY_MOVE_CAMERA_VIDEO");
        }
        if (preview_device_path == NULL || preview_device_path[0] == '\0') {
            preview_device_path = "/dev/video0";
        }

        if (requested_record_device != NULL && requested_record_device[0] != '\0') {
            snprintf(record_device_path, sizeof(record_device_path),
                     "%s", requested_record_device);
        } else if (!camera_capture_find_alternate_device(preview_device_path,
                                                         record_device_path,
                                                         sizeof(record_device_path))) {
            snprintf(record_device_path, sizeof(record_device_path),
                     "%s", preview_device_path);
        }

        if (mkdir(video_dir, 0755) != 0 && errno != EEXIST) {
            perror("Unable to create videos directory");
            return false;
        }

        localtime_r(&now, &local_time);
        strftime(timestamp, sizeof(timestamp), "%Y%m%d_%H%M%S", &local_time);
        snprintf(path, sizeof(path), "%s/VID_%s.mp4", video_dir, timestamp);
        snprintf(last_video_device, sizeof(last_video_device), "%s", record_device_path);

        preview_paused_for_recording =
            strcmp(record_device_path, preview_device_path) == 0;
        if (preview_paused_for_recording) {
            camera_capture_stop();
        }
        if (camera_recorder_start(last_video_device, path) != 0) {
            perror("Unable to start GStreamer recording");
            if (preview_paused_for_recording) {
                camera_capture_start(UI_WIDTH, UI_HEIGHT);
                preview_paused_for_recording = false;
            }
            return false;
        }

        printf("Video recording to: %s via %s%s\n",
               path,
               last_video_device,
               preview_paused_for_recording ? " (preview paused)" : "");
        return true;
    }

    camera_recorder_stop();
    if (preview_paused_for_recording) {
        if (camera_capture_start(UI_WIDTH, UI_HEIGHT) != 0) {
            fprintf(stderr, "Warning: camera preview did not restart after recording.\n");
        }
        preview_paused_for_recording = false;
    }
    return true;
#endif
}

static bool play_video(const char * path, void * user_data)
{
    (void)user_data;
    if (camera_recorder_is_recording()) {
        return false;
    }

#if USE_SDL
    return camera_recorder_play(path) == 0;
#else
    const char * release_drm = getenv("MY_MOVE_CAMERA_PLAY_RELEASE_DRM");
    if (release_drm != NULL && strcmp(release_drm, "1") == 0) {
        camera_capture_stop();
        fbp = NULL;
        drm_display_deinit();

        int result = camera_recorder_play_blocking(path);

        if (fbdev_init() != 0) {
            fprintf(stderr, "Warning: DRM display did not restart after playback.\n");
            return false;
        }
        if (camera_capture_start(UI_WIDTH, UI_HEIGHT) != 0) {
            fprintf(stderr, "Warning: camera preview did not restart after playback.\n");
        }
        lv_obj_invalidate(lv_scr_act());
        return result == 0;
    }

    return camera_recorder_play(path) == 0;
#endif
}

#if USE_SDL
#include <SDL2/SDL.h>
#else
#include <fcntl.h>
#include <sys/ioctl.h>
#include <linux/input.h>
#endif

// ============================================================================
// 全局变量定义 (通过条件预编译指令完全隔离)
// ============================================================================
#if USE_SDL
static SDL_Window * window = NULL;
static SDL_Renderer * renderer = NULL;
static SDL_Texture * texture = NULL;
static uint32_t * tbuf = NULL; // 800 * 480 的虚拟 ARGB 帧缓冲区
// 鼠标交互输入数据
static bool mouse_pressed = false;
static int16_t mouse_x = 0;
static int16_t mouse_y = 0;
#else
static drm_display_buffer_t drm_buffer;
static char *fbp = NULL;
static size_t screensize = 0;
static uint32_t physical_width;
static uint32_t physical_height;
static uint32_t display_pitch;
static int touchfd = -1;
static struct input_absinfo abs_x_info;
static struct input_absinfo abs_y_info;
static int32_t touch_raw_x = 0;
static int32_t touch_raw_y = 0;
static bool touch_pressed = false;
#endif

// ============================================================================
// 驱动程序实现
// ============================================================================

#if USE_SDL
// ------------------------------------------------------------
// WSL SDL2 桌面仿真端驱动
// ------------------------------------------------------------
static int sdl_init(void)
{
    if (SDL_Init(SDL_INIT_VIDEO) < 0) {
        fprintf(stderr, "SDL could not initialize! SDL_Error: %s\n", SDL_GetError());
        return -1;
    }

    // 创建仿真窗口 (800x480)
    window = SDL_CreateWindow("Action Camera Landscape Simulator", 
                              SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED, 
                              UI_WIDTH, UI_HEIGHT, SDL_WINDOW_SHOWN);
    if (window == NULL) {
        fprintf(stderr, "Window could not be created! SDL_Error: %s\n", SDL_GetError());
        return -1;
    }

    renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (renderer == NULL) {
        fprintf(stderr, "Renderer could not be created! SDL_Error: %s\n", SDL_GetError());
        return -1;
    }

    texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, UI_WIDTH, UI_HEIGHT);
    if (texture == NULL) {
        fprintf(stderr, "Texture could not be created! SDL_Error: %s\n", SDL_GetError());
        return -1;
    }

    tbuf = (uint32_t *)malloc(UI_WIDTH * UI_HEIGHT * sizeof(uint32_t));
    if (tbuf == NULL) {
        fprintf(stderr, "Failed to allocate memory for texture buffer!\n");
        return -1;
    }
    memset(tbuf, 0, UI_WIDTH * UI_HEIGHT * sizeof(uint32_t));

    printf("SDL2 Simulation Mode Initialized.\n");
    return 0;
}

static void sdl_flush(lv_disp_drv_t * disp_drv, const lv_area_t * area, lv_color_t * color_p)
{
    int32_t act_x1 = area->x1 < 0 ? 0 : area->x1;
    int32_t act_y1 = area->y1 < 0 ? 0 : area->y1;
    int32_t act_x2 = area->x2 >= UI_WIDTH ? UI_WIDTH - 1 : area->x2;
    int32_t act_y2 = area->y2 >= UI_HEIGHT ? UI_HEIGHT - 1 : area->y2;

    int32_t y;
    int32_t x;
    int32_t w = lv_area_get_width(area);

    for (y = act_y1; y <= act_y2; y++) {
        for (x = act_x1; x <= act_x2; x++) {
            lv_color_t color = color_p[(y - area->y1) * w + (x - area->x1)];
            tbuf[y * UI_WIDTH + x] = lv_color_to32(color);
        }
    }

    lv_disp_flush_ready(disp_drv);
}

static void sdl_mouse_read(lv_indev_drv_t * indev_drv, lv_indev_data_t * data)
{
    (void)indev_drv;
    data->point.x = mouse_x;
    data->point.y = mouse_y;
    data->state = mouse_pressed ? LV_INDEV_STATE_PR : LV_INDEV_STATE_REL;
    camera_page_pointer_sample(mouse_pressed, mouse_x, mouse_y);
}

#else
// ------------------------------------------------------------
// RK3566 开发板真实 Framebuffer 端驱动 (带自动 90 度旋转)
// ------------------------------------------------------------
static void fbdev_clear_screen(void)
{
    if (fbp == NULL || screensize <= 0) {
        return;
    }

    memset(fbp, 0, screensize);
}

static int32_t scale_abs_value(int32_t value, const struct input_absinfo * info, int32_t out_max)
{
    int32_t in_min = info->minimum;
    int32_t in_max = info->maximum;

    if (in_max <= in_min) {
        return value;
    }

    if (value < in_min) value = in_min;
    if (value > in_max) value = in_max;

    return (value - in_min) * (out_max - 1) / (in_max - in_min);
}

static int32_t clamp_coord(int32_t value, int32_t min, int32_t max)
{
    if (value < min) return min;
    if (value > max) return max;
    return value;
}

#define BITS_PER_LONG (sizeof(unsigned long) * 8U)
#define BIT_ARRAY_SIZE(max_bit) (((max_bit) / BITS_PER_LONG) + 1U)

static bool input_bit_is_set(const unsigned long * bits, unsigned int bit)
{
    return (bits[bit / BITS_PER_LONG] &
            (1UL << (bit % BITS_PER_LONG))) != 0;
}

static int touchdev_open_device(const char * path)
{
    unsigned long key_bits[BIT_ARRAY_SIZE(KEY_MAX)];
    unsigned long abs_bits[BIT_ARRAY_SIZE(ABS_MAX)];
    char device_name[128] = "unknown";
    int fd = open(path, O_RDONLY | O_NONBLOCK);
    if (fd < 0) {
        return -1;
    }

    memset(key_bits, 0, sizeof(key_bits));
    memset(abs_bits, 0, sizeof(abs_bits));
    ioctl(fd, EVIOCGNAME(sizeof(device_name)), device_name);
    if (ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(key_bits)), key_bits) == -1 ||
        ioctl(fd, EVIOCGBIT(EV_ABS, sizeof(abs_bits)), abs_bits) == -1) {
        close(fd);
        return -1;
    }

    if (!input_bit_is_set(key_bits, BTN_TOUCH) &&
        !input_bit_is_set(abs_bits, ABS_MT_TRACKING_ID)) {
        close(fd);
        return -1;
    }

    if (ioctl(fd, EVIOCGABS(ABS_MT_POSITION_X), &abs_x_info) == -1 &&
        ioctl(fd, EVIOCGABS(ABS_X), &abs_x_info) == -1) {
        close(fd);
        return -1;
    }

    if (ioctl(fd, EVIOCGABS(ABS_MT_POSITION_Y), &abs_y_info) == -1 &&
        ioctl(fd, EVIOCGABS(ABS_Y), &abs_y_info) == -1) {
        close(fd);
        return -1;
    }

    touch_raw_x = abs_x_info.minimum;
    touch_raw_y = abs_y_info.minimum;
    touch_pressed = false;

    printf("Touch input initialized: %s (%s), X[%d..%d], Y[%d..%d]\n",
           path,
           device_name,
           abs_x_info.minimum,
           abs_x_info.maximum,
           abs_y_info.minimum,
           abs_y_info.maximum);
    return fd;
}

static void touchdev_init(void)
{
    const char * env_path = getenv("MY_MOVE_CAMERA_TOUCH");
    if (env_path != NULL && env_path[0] != '\0') {
        touchfd = touchdev_open_device(env_path);
        if (touchfd >= 0) {
            return;
        }
        fprintf(stderr, "Warning: failed to open touch device %s\n", env_path);
    }

    for (int i = 0; i < 32; i++) {
        char path[32];
        snprintf(path, sizeof(path), "/dev/input/event%d", i);
        touchfd = touchdev_open_device(path);
        if (touchfd >= 0) {
            return;
        }
    }

    fprintf(stderr, "Warning: no evdev touch input found; framebuffer display still works.\n");
}

static void touchdev_read(lv_indev_drv_t * indev_drv, lv_indev_data_t * data)
{
    (void)indev_drv;

    if (touchfd >= 0) {
        struct input_event ev;
        while (read(touchfd, &ev, sizeof(ev)) == (ssize_t)sizeof(ev)) {
            if (ev.type == EV_ABS) {
                if (ev.code == ABS_X || ev.code == ABS_MT_POSITION_X) {
                    touch_raw_x = ev.value;
                } else if (ev.code == ABS_Y || ev.code == ABS_MT_POSITION_Y) {
                    touch_raw_y = ev.value;
                } else if (ev.code == ABS_MT_TRACKING_ID) {
                    touch_pressed = ev.value >= 0;
                }
            } else if (ev.type == EV_KEY && ev.code == BTN_TOUCH) {
                touch_pressed = ev.value != 0;
            }
        }
    }

    int32_t physical_x = scale_abs_value(touch_raw_x, &abs_x_info, physical_width);
    int32_t physical_y = scale_abs_value(touch_raw_y, &abs_y_info, physical_height);
    int32_t logical_x;
    int32_t logical_y;

    if (physical_width < physical_height) {
        logical_x = (physical_height - 1 - physical_y) * UI_WIDTH / physical_height;
        logical_y = physical_x * UI_HEIGHT / physical_width;
        logical_x = clamp_coord(logical_x, 0, UI_WIDTH - 1);
        logical_y = clamp_coord(logical_y, 0, UI_HEIGHT - 1);
    } else {
        logical_x = physical_x;
        logical_y = physical_y;
        logical_x = clamp_coord(logical_x, 0, (int32_t)physical_width - 1);
        logical_y = clamp_coord(logical_y, 0, (int32_t)physical_height - 1);
    }

    data->point.x = logical_x;
    data->point.y = logical_y;
    data->state = touch_pressed ? LV_INDEV_STATE_PR : LV_INDEV_STATE_REL;
    camera_page_pointer_sample(touch_pressed, data->point.x, data->point.y);
}

static int fbdev_init(void)
{
    if (drm_display_init(&drm_buffer) != 0) {
        return -1;
    }

    fbp = (char *)drm_buffer.pixels;
    screensize = drm_buffer.size;
    physical_width = drm_buffer.width;
    physical_height = drm_buffer.height;
    display_pitch = drm_buffer.pitch;
    fbdev_clear_screen();
    printf("RK3566 DRM buffer cleared before first LVGL draw.\n");
    return 0;
}

static void fbdev_flush(lv_disp_drv_t * disp_drv, const lv_area_t * area, lv_color_t * color_p)
{
    if (fbp == NULL) {
        lv_disp_flush_ready(disp_drv);
        return;
    }

    int32_t w = lv_area_get_width(area);
    int32_t y;
    int32_t x;

    bool rotate_90 = (physical_width < physical_height);
    uint32_t * fbp32 = (uint32_t *)fbp;
    int32_t line_width = display_pitch / 4;
        
    if (rotate_90) {
        int32_t act_x1 = area->x1 < 0 ? 0 : area->x1;
        int32_t act_y1 = area->y1 < 0 ? 0 : area->y1;
        int32_t act_x2 = area->x2 >= UI_WIDTH ? UI_WIDTH - 1 : area->x2;
        int32_t act_y2 = area->y2 >= UI_HEIGHT ? UI_HEIGHT - 1 : area->y2;

        for (y = act_y1; y <= act_y2; y++) {
            uint32_t * dest = fbp32 +
                ((UI_WIDTH - 1 - act_x1) * line_width) + y;
            for (x = act_x1; x <= act_x2; x++) {
                lv_color_t color =
                    color_p[(y - area->y1) * w + (x - area->x1)];
                *dest = color.full;
                dest -= line_width;
            }
        }
    } else {
        int32_t act_x1 = area->x1 < 0 ? 0 : area->x1;
        int32_t act_y1 = area->y1 < 0 ? 0 : area->y1;
        int32_t act_x2 = area->x2 >= (int32_t)physical_width ? (int32_t)physical_width - 1 : area->x2;
        int32_t act_y2 = area->y2 >= (int32_t)physical_height ? (int32_t)physical_height - 1 : area->y2;
        for (y = act_y1; y <= act_y2; y++) {
            for (x = act_x1; x <= act_x2; x++) {
                lv_color_t color = color_p[(y - area->y1) * w + (x - area->x1)];
                fbp32[y * line_width + x] = lv_color_to32(color);
            }
        }
    }

    lv_disp_flush_ready(disp_drv);
}
#endif

// 精确的 LVGL 时间基准后台线程
static void * tick_thread(void * data) {
    (void)data;
    while(1) {
        usleep(5000); // 5毫秒
        lv_tick_inc(5);
    }
    return NULL;
}

// ============================================================================
// 程序入口 main 函数 (实现条件宏的分支启动)
// ============================================================================
int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;

#if USE_SDL
    printf("Starting my_move_camera: SDL2 Simulation Mode (WSL)...\n");
    // 1. 初始化 SDL2 仿真窗口 (固定 800x480 物理像素)
    if (sdl_init() != 0) {
        fprintf(stderr, "Fatal Error: Failed to initialize SDL2!\n");
        return -1;
    }
#else
    printf("Starting my_move_camera: Linux DRM/KMS Mode (RK3566)...\n");
    // 1. 初始化 DRM/KMS 显示，绕开 Rockchip fbdev mmap 空指针问题。
    if (fbdev_init() != 0) {
        fprintf(stderr, "Fatal Error: Failed to initialize DRM display!\n");
        return -1;
    }
    touchdev_init();
#endif

    // 2. 初始化 LVGL 核心图形库
    lv_init();
    lv_extra_init();

    // 3. 注册 LVGL 显示缓冲区
    static lv_disp_draw_buf_t draw_buf;
#if USE_SDL
    uint32_t buf_width = UI_WIDTH;
#else
    // 物理竖屏下，如果做了 90 度旋转，LVGL 在逻辑层看到的一行宽度依然是 UI_WIDTH (800)！
    uint32_t buf_width = (physical_width < physical_height) ? UI_WIDTH : physical_width;
#endif
    // 增大缓冲区到 150 行，减少重绘时的 CPU 调度次数
    lv_color_t *buf1 = (lv_color_t *)malloc(buf_width * 150 * sizeof(lv_color_t));
    if (buf1 == NULL) {
        fprintf(stderr, "Fatal Error: Memory allocation for display buffer failed!\n");
#if USE_SDL
        free(tbuf);
        SDL_DestroyTexture(texture);
        SDL_DestroyRenderer(renderer);
        SDL_DestroyWindow(window);
        SDL_Quit();
#else
        if (touchfd >= 0) close(touchfd);
        drm_display_deinit();
#endif
        return -1;
    }
    lv_disp_draw_buf_init(&draw_buf, buf1, NULL, buf_width * 150);

    // 4. 注册显示驱动 (根据宏绑定不同的刷新回调)
    static lv_disp_drv_t disp_drv;
    lv_disp_drv_init(&disp_drv);
    disp_drv.draw_buf = &draw_buf;
#if USE_SDL
    disp_drv.flush_cb = sdl_flush;
    disp_drv.hor_res = UI_WIDTH;
    disp_drv.ver_res = UI_HEIGHT;
#else
    disp_drv.flush_cb = fbdev_flush;
    // 如果物理屏幕是 480x800 竖屏，我们在物理驱动 flush 时将其顺时针旋转 90 度呈现在屏幕上。
    // 这意味着 LVGL 内部依旧将其当作 800x480 的横屏来渲染，因此其逻辑宽高就是 UI_WIDTH x UI_HEIGHT！
    if (physical_width < physical_height) {
        disp_drv.hor_res = UI_WIDTH;
        disp_drv.ver_res = UI_HEIGHT;
    } else {
        disp_drv.hor_res = physical_width;
        disp_drv.ver_res = physical_height;
    }
#endif
    lv_disp_drv_register(&disp_drv);

    // 5. 注册指针输入驱动
#if USE_SDL
    static lv_indev_drv_t indev_drv;
    lv_indev_drv_init(&indev_drv);
    indev_drv.type = LV_INDEV_TYPE_POINTER;
    indev_drv.read_cb = sdl_mouse_read;
    lv_indev_drv_register(&indev_drv);
#else
    if (touchfd >= 0) {
        static lv_indev_drv_t indev_drv;
        lv_indev_drv_init(&indev_drv);
        indev_drv.type = LV_INDEV_TYPE_POINTER;
        indev_drv.read_cb = touchdev_read;
        lv_indev_drv_register(&indev_drv);
    }
#endif

    // 6. 启动高精度时间 Tick 线程
    pthread_t tid;
    if (pthread_create(&tid, NULL, tick_thread, NULL) != 0) {
        perror("Error: pthread_create failed");
        free(buf1);
#if USE_SDL
        free(tbuf);
        SDL_DestroyTexture(texture);
        SDL_DestroyRenderer(renderer);
        SDL_DestroyWindow(window);
        SDL_Quit();
#else
        if (touchfd >= 0) close(touchfd);
        drm_display_deinit();
#endif
        return -1;
    }

    // 7. 渲染运动相机 UI (根据显示器物理大小，自适应全屏对齐)
#if USE_SDL
    camera_page_create(UI_WIDTH, UI_HEIGHT);
#else
    // 核心修正：UI 应该按照 LVGL 的逻辑分辨率（800x480）创建
    if (physical_width < physical_height)
        camera_page_create(UI_WIDTH, UI_HEIGHT);
    else
        camera_page_create(physical_width, physical_height);

#endif

    camera_preview_pixels = calloc(UI_WIDTH * UI_HEIGHT, sizeof(lv_color_t));
    last_photo_pixels = calloc(UI_WIDTH * UI_HEIGHT, sizeof(lv_color_t));
    if (camera_preview_pixels != NULL) {
        memset(&camera_preview_image, 0, sizeof(camera_preview_image));
        camera_preview_image.header.always_zero = 0;
        camera_preview_image.header.w = UI_WIDTH;
        camera_preview_image.header.h = UI_HEIGHT;
        camera_preview_image.header.cf = LV_IMG_CF_TRUE_COLOR;
        camera_preview_image.data_size = UI_WIDTH * UI_HEIGHT * sizeof(lv_color_t);
        camera_preview_image.data = (const uint8_t *)camera_preview_pixels;
        camera_page_set_preview_image(&camera_preview_image);

#if USE_SDL
        create_wsl_default_image(camera_preview_pixels);
        camera_page_refresh_preview();
#else
        if (camera_capture_start(UI_WIDTH, UI_HEIGHT) == 0) {
            lv_timer_create(camera_preview_timer_cb, 33, NULL);
        } else {
            fprintf(stderr,
                    "Warning: camera preview unavailable. Set MY_MOVE_CAMERA_VIDEO=/dev/videoN "
                    "to select the RKISP output node explicitly.\n");
        }
#endif
    } else {
        fprintf(stderr, "Warning: unable to allocate the camera preview image.\n");
    }

    if (last_photo_pixels != NULL) {
        memset(&last_photo_image, 0, sizeof(last_photo_image));
        last_photo_image.header.always_zero = 0;
        last_photo_image.header.w = UI_WIDTH;
        last_photo_image.header.h = UI_HEIGHT;
        last_photo_image.header.cf = LV_IMG_CF_TRUE_COLOR;
        last_photo_image.data_size = UI_WIDTH * UI_HEIGHT * sizeof(lv_color_t);
        last_photo_image.data = (const uint8_t *)last_photo_pixels;
    }
    camera_page_set_capture_callback(capture_current_photo, NULL);
    camera_page_set_record_callback(handle_recording, NULL);
    camera_page_set_play_video_callback(play_video, NULL);

    printf("UI Rendered. Entering main loop...\n");

    // 8. 周期事件主循环
#if USE_SDL
    bool quit = false;
    SDL_Event event;
    while (!quit) {
        // A. 轮询同步鼠标与窗口事件
        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_QUIT) {
                quit = true;
            } else if (event.type == SDL_MOUSEBUTTONDOWN) {
                if (event.button.button == SDL_BUTTON_LEFT) {
                    mouse_pressed = true;
                    mouse_x = event.button.x;
                    mouse_y = event.button.y;
                    camera_page_pointer_sample(mouse_pressed, mouse_x, mouse_y);
                }
            } else if (event.type == SDL_MOUSEBUTTONUP) {
                if (event.button.button == SDL_BUTTON_LEFT) {
                    mouse_pressed = false;
                    camera_page_pointer_sample(mouse_pressed, mouse_x, mouse_y);
                }
            } else if (event.type == SDL_MOUSEMOTION) {
                mouse_x = event.motion.x;
                mouse_y = event.motion.y;
                camera_page_pointer_sample(mouse_pressed, mouse_x, mouse_y);
            }
        }

        // B. 调度 LVGL 计时器
        lv_timer_handler();

        // C. 将虚拟显存写入纹理并由 SDL 硬件渲染
        SDL_UpdateTexture(texture, NULL, tbuf, UI_WIDTH * sizeof(uint32_t));
        SDL_RenderClear(renderer);
        SDL_RenderCopy(renderer, texture, NULL, NULL);
        SDL_RenderPresent(renderer);

        usleep(5000); // 5毫秒
    }

    printf("Exiting Action Camera Simulator gracefully...\n");
    free(buf1);
    free(camera_preview_pixels);
    free(last_photo_pixels);
    free(tbuf);
    SDL_DestroyTexture(texture);
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
#else
    while (1) {
        lv_timer_handler();
        usleep(5000);
    }

    printf("Exiting Action Camera Board App gracefully...\n");
    camera_capture_stop();
    free(camera_preview_pixels);
    free(last_photo_pixels);
    free(buf1);
    if (touchfd >= 0) close(touchfd);
    drm_display_deinit();
#endif

    return 0;
}
