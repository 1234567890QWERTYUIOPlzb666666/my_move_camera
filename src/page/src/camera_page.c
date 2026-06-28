#include "../inc/camera_page.h"
#include "../inc/quick_settings_page.h"
#include "../../camera_media.h"
#include "lvgl/lvgl.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdbool.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#ifndef WSL
#include <gst/gst.h>
#include <gst/app/gstappsink.h>
#endif

#define UI_TEXT_FONT &lv_font_montserrat_14
#define UI_CJK_FONT  &font_alibaba_puhuiti_20
#define SETTINGS_PANEL_W 400
#define SETTINGS_EDGE_TOUCH_W 18
#define QUICK_EDGE_TOUCH_H 96
#define RECORD_SETTINGS_EDGE_TOUCH_H 32
#define RECORD_SETTINGS_PANEL_H 206
#define QUICK_SWIPE_MIN_DIST 52
#define QUICK_DRAG_START_DIST 10
#define PLAYBACK_FPS 30
#define PLAYBACK_PROGRESS_RANGE 1000

typedef enum {
    MODE_VIDEO,
    MODE_PHOTO
} camera_mode_t;

static bool is_recording = false;
static uint32_t rec_seconds = 0;
static lv_obj_t *rec_dot;
static lv_obj_t *rec_time_label;
static lv_obj_t *mode_label;
static lv_obj_t *mode_detail_label;
static lv_obj_t *shutter_btn;
static lv_obj_t *shutter_icon;
static lv_obj_t *album_btn;
static lv_obj_t *album_overlay;
static lv_obj_t *album_view_image;
static lv_obj_t *album_index_label;
static lv_obj_t *album_path_label;
static lv_obj_t *album_delete_btn;
static lv_obj_t *album_play_btn;
static lv_obj_t *album_progress_slider;
static lv_obj_t *album_progress_time_label;
static lv_color_t *album_view_pixels = NULL;
static lv_img_dsc_t album_view_dsc;
static lv_timer_t *album_playback_timer = NULL;
static pthread_t album_playback_thread;
static bool album_playback_thread_started = false;
static pid_t album_playback_pid = -1;
static pthread_mutex_t album_playback_mutex = PTHREAD_MUTEX_INITIALIZER;
static bool album_playback_dirty = false;
static bool album_playback_finished = false;
static bool album_playback_failed = false;
static bool album_playback_active = false;
static bool album_playback_paused = false;
static bool album_progress_dragging = false;
static uint32_t album_playback_current_ms = 0;
static uint32_t album_playback_duration_ms = 0;
static bool album_playback_stop_requested = false;
#ifndef WSL
static GstElement *album_playback_pipeline = NULL;
#endif
static lv_obj_t *settings_panel;
static lv_obj_t *record_settings_panel;
static lv_obj_t *record_resolution_btns[3];
static lv_obj_t *record_fps_btns[2];
static lv_timer_t *rec_timer;
static bool settings_panel_open = false;
static bool record_settings_panel_open = false;
static camera_mode_t current_mode = MODE_VIDEO;
static bool camera_page_active = false;
static bool quick_swipe_tracking = false;
static lv_point_t quick_swipe_start;
static bool global_quick_swipe_tracking = false;
static bool global_record_settings_swipe_tracking = false;
static bool global_quick_open_pending = false;
static int32_t global_quick_swipe_start_y = 0;
static lv_obj_t *quick_curtain = NULL;
static lv_obj_t *preview_container = NULL;
static lv_obj_t *preview_image = NULL;
static const lv_img_dsc_t *preview_image_source = NULL;
static const lv_img_dsc_t *album_image_source = NULL;
static char album_image_path[160];
static char **album_photo_paths = NULL;
static size_t album_photo_count = 0;
static size_t album_photo_index = 0;
static camera_page_capture_cb_t capture_callback = NULL;
static void *capture_callback_user_data = NULL;
static camera_page_record_cb_t record_callback = NULL;
static void *record_callback_user_data = NULL;
static camera_page_play_video_cb_t play_video_callback = NULL;
static void *play_video_callback_user_data = NULL;
static bool quick_curtain_dragging = false;
static int32_t quick_curtain_start_y = 0;
static bool record_settings_dragging = false;
static int32_t record_settings_start_y = 0;
static int32_t record_settings_drag_start_y = UI_HEIGHT;

typedef struct {
    const char * label;
    uint32_t width;
    uint32_t height;
} record_resolution_option_t;

static const record_resolution_option_t record_resolution_options[] = {
    {"720p", 1280, 720},
    {"1080p", 1920, 1080},
    {"2K", 2592, 1944},
};

static const uint32_t record_fps_options[] = {30, 15};
static uint8_t record_resolution_index = 0;
static uint8_t record_fps_index = 0;

static bool is_quick_settings_swipe(int32_t start_y, int32_t end_y);
static void quick_curtain_set_delta(int32_t delta_y);
static void record_settings_set_drag_y(int32_t y);
static void record_settings_finish(void);
static void album_photo_list_clear(void);
static void album_playback_stop(void);
static bool album_playback_start(const char * path);
static int32_t clamp_i32(int32_t value, int32_t min, int32_t max);

typedef struct {
    char *path;
    uint32_t start_ms;
    uint32_t duration_ms;
} playback_request_t;

static lv_color_t c_bg(void) { return lv_color_make(12, 13, 14); }
static lv_color_t c_yellow(void) { return lv_color_make(255, 210, 48); }
static lv_color_t c_red(void) { return lv_color_make(236, 30, 36); }
static lv_color_t c_dji_tile(void) { return lv_color_make(78, 126, 150); }

static void clear_camera_page_refs(void)
{
    album_playback_stop();
    rec_dot = NULL;
    rec_time_label = NULL;
    mode_label = NULL;
    mode_detail_label = NULL;
    shutter_btn = NULL;
    shutter_icon = NULL;
    album_btn = NULL;
    album_overlay = NULL;
    album_view_image = NULL;
    album_index_label = NULL;
    album_path_label = NULL;
    album_delete_btn = NULL;
    album_play_btn = NULL;
    album_progress_slider = NULL;
    album_progress_time_label = NULL;
    record_settings_panel = NULL;
    memset(record_resolution_btns, 0, sizeof(record_resolution_btns));
    memset(record_fps_btns, 0, sizeof(record_fps_btns));
    free(album_view_pixels);
    album_view_pixels = NULL;
    album_photo_list_clear();
    settings_panel = NULL;
    preview_container = NULL;
    preview_image = NULL;
    settings_panel_open = false;
    record_settings_panel_open = false;
    record_settings_dragging = false;
    global_record_settings_swipe_tracking = false;
}

static void create_preview_image(void)
{
    if (preview_container == NULL || preview_image != NULL || preview_image_source == NULL) {
        return;
    }

    preview_image = lv_img_create(preview_container);
    lv_img_set_src(preview_image, preview_image_source);
    lv_obj_set_size(preview_image, UI_WIDTH, UI_HEIGHT);
    lv_obj_align(preview_image, LV_ALIGN_CENTER, 0, 0);
    lv_obj_clear_flag(preview_image, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_move_background(preview_image);
}

void camera_page_set_preview_image(const lv_img_dsc_t * image)
{
    preview_image_source = image;
    create_preview_image();
    if (preview_image != NULL && image != NULL) {
        lv_img_set_src(preview_image, image);
        lv_obj_invalidate(preview_image);
    }
}

void camera_page_refresh_preview(void)
{
    if (preview_image != NULL && preview_image_source != NULL) {
        lv_obj_invalidate(preview_image);
    }
}

void camera_page_set_capture_callback(camera_page_capture_cb_t callback, void * user_data)
{
    capture_callback = callback;
    capture_callback_user_data = user_data;
}

void camera_page_set_record_callback(camera_page_record_cb_t callback, void * user_data)
{
    record_callback = callback;
    record_callback_user_data = user_data;
}

void camera_page_set_play_video_callback(camera_page_play_video_cb_t callback, void * user_data)
{
    play_video_callback = callback;
    play_video_callback_user_data = user_data;
}

void camera_page_set_album_image(const lv_img_dsc_t * image, const char * path)
{
    album_image_source = image;
    if (path == NULL) {
        album_image_path[0] = '\0';
    } else {
        snprintf(album_image_path, sizeof(album_image_path), "%s", path);
    }

    if (album_btn != NULL && current_mode == MODE_PHOTO) {
        lv_obj_clear_flag(album_btn, LV_OBJ_FLAG_HIDDEN);
    }
}

static const char *photo_dir_get(void)
{
    const char * photo_dir = getenv("MY_MOVE_CAMERA_PHOTO_DIR");
    if (photo_dir == NULL || photo_dir[0] == '\0') {
        photo_dir = "photos";
    }
    return photo_dir;
}

static const char *video_dir_get(void)
{
    const char * video_dir = getenv("MY_MOVE_CAMERA_VIDEO_DIR");
    if (video_dir == NULL || video_dir[0] == '\0') {
        video_dir = "videos";
    }
    return video_dir;
}

static bool has_jpeg_extension(const char * name)
{
    const char * dot = strrchr(name, '.');
    if (dot == NULL) return false;
    return strcmp(dot, ".jpg") == 0 ||
           strcmp(dot, ".jpeg") == 0 ||
           strcmp(dot, ".JPG") == 0 ||
           strcmp(dot, ".JPEG") == 0;
}

static bool has_video_extension(const char * name)
{
    const char * dot = strrchr(name, '.');
    if (dot == NULL) return false;
    return strcmp(dot, ".mp4") == 0 ||
           strcmp(dot, ".MP4") == 0 ||
           strcmp(dot, ".mkv") == 0 ||
           strcmp(dot, ".MKV") == 0 ||
           strcmp(dot, ".h264") == 0 ||
           strcmp(dot, ".H264") == 0;
}

static char *string_duplicate(const char * text)
{
    size_t length = strlen(text) + 1;
    char * copy = malloc(length);
    if (copy != NULL) {
        memcpy(copy, text, length);
    }
    return copy;
}

#ifdef WSL
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

static void playback_command_build(char * out, size_t out_size, const char * path)
{
    const char * pipeline = getenv("MY_MOVE_CAMERA_INLINE_PLAY_PIPELINE");

    if (pipeline == NULL || pipeline[0] == '\0') {
        pipeline =
            "gst-launch-1.0 -q "
            "filesrc location={path} ! decodebin ! "
            "videoconvert ! videoscale ! "
            "video/x-raw,format=RGB,width=800,height=480 ! "
            "fdsink fd=1 sync=true";
    }

    out[0] = '\0';
    for (const char * p = pipeline; *p != '\0' && strlen(out) + 1 < out_size;) {
        if (strncmp(p, "{path}", 6) == 0) {
            shell_quote_append(out, out_size, path);
            p += 6;
        } else {
            size_t used = strlen(out);
            out[used] = *p++;
            out[used + 1] = '\0';
        }
    }
}
#endif

static void playback_time_format(char * out, size_t out_size, uint32_t ms)
{
    uint32_t seconds = ms / 1000;
    uint32_t h = seconds / 3600;
    uint32_t m = (seconds / 60) % 60;
    uint32_t s = seconds % 60;

    if (h > 0) {
        snprintf(out, out_size, "%u:%02u:%02u", h, m, s);
    } else {
        snprintf(out, out_size, "%02u:%02u", m, s);
    }
}

static void playback_progress_update_ui(uint32_t current_ms, uint32_t duration_ms)
{
    char current_text[16];
    char duration_text[16];

    if (album_progress_slider == NULL || album_progress_time_label == NULL) {
        return;
    }

    if (current_mode != MODE_VIDEO || album_photo_count == 0 || duration_ms == 0) {
        lv_obj_add_flag(album_progress_slider, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(album_progress_time_label, LV_OBJ_FLAG_HIDDEN);
        return;
    }

    lv_obj_clear_flag(album_progress_slider, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(album_progress_time_label, LV_OBJ_FLAG_HIDDEN);

    if (!album_progress_dragging) {
        int32_t slider_value =
            (int32_t)((uint64_t)current_ms * PLAYBACK_PROGRESS_RANGE / duration_ms);
        lv_slider_set_value(album_progress_slider,
                            clamp_i32(slider_value, 0, PLAYBACK_PROGRESS_RANGE),
                            LV_ANIM_OFF);
    }

    playback_time_format(current_text, sizeof(current_text), current_ms);
    playback_time_format(duration_text, sizeof(duration_text), duration_ms);
    lv_label_set_text_fmt(album_progress_time_label, "%s / %s",
                          current_text, duration_text);
}

static pid_t spawn_stdout_command(const char * command, int * out_fd)
{
    int pipefd[2];
    pid_t pid;

    if (pipe(pipefd) != 0) {
        return -1;
    }

    pid = fork();
    if (pid < 0) {
        close(pipefd[0]);
        close(pipefd[1]);
        return -1;
    }

    if (pid == 0) {
        int nullfd;

        setpgid(0, 0);
        close(pipefd[0]);
        dup2(pipefd[1], STDOUT_FILENO);
        close(pipefd[1]);

        nullfd = open("/dev/null", O_WRONLY);
        if (nullfd >= 0) {
            dup2(nullfd, STDERR_FILENO);
            close(nullfd);
        }

        execlp("sh", "sh", "-c", command, (char *)NULL);
        _exit(127);
    }

    setpgid(pid, pid);
    close(pipefd[1]);
    *out_fd = pipefd[0];
    return pid;
}

static bool read_exact(int fd, uint8_t * buffer, size_t size)
{
    size_t offset = 0;

    while (offset < size) {
        ssize_t result = read(fd, buffer + offset, size - offset);
        if (result == 0) return false;
        if (result < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        offset += (size_t)result;
    }

    return true;
}

static void *album_playback_thread_main(void * data)
{
    playback_request_t * request = data;
#ifdef WSL
    const char * path = request->path;
    const size_t frame_rgb_size = (size_t)UI_WIDTH * UI_HEIGHT * 3;
    uint8_t * frame_rgb = malloc(frame_rgb_size);
    char command[1024];
    int fd = -1;
    size_t frame_count = 0;
    pid_t pid;

    if (frame_rgb == NULL) {
        pthread_mutex_lock(&album_playback_mutex);
        album_playback_failed = true;
        album_playback_finished = true;
        pthread_mutex_unlock(&album_playback_mutex);
        free(request->path);
        free(request);
        return NULL;
    }

    pthread_mutex_lock(&album_playback_mutex);
    album_playback_current_ms = request->start_ms;
    album_playback_duration_ms = request->duration_ms;
    pthread_mutex_unlock(&album_playback_mutex);

    playback_command_build(command, sizeof(command), path);
    pid = spawn_stdout_command(command, &fd);
    pthread_mutex_lock(&album_playback_mutex);
    album_playback_pid = pid;
    pthread_mutex_unlock(&album_playback_mutex);

    if (pid < 0) {
        pthread_mutex_lock(&album_playback_mutex);
        album_playback_failed = true;
        album_playback_finished = true;
        pthread_mutex_unlock(&album_playback_mutex);
        free(frame_rgb);
        free(request->path);
        free(request);
        return NULL;
    }

    printf("Inline playback command: %s\n", command);
    while (read_exact(fd, frame_rgb, frame_rgb_size)) {
        pthread_mutex_lock(&album_playback_mutex);
        if (album_view_pixels != NULL) {
            for (size_t i = 0; i < (size_t)UI_WIDTH * UI_HEIGHT; i++) {
                const uint8_t * rgb = frame_rgb + i * 3;
                album_view_pixels[i] = lv_color_make(rgb[0], rgb[1], rgb[2]);
            }
            album_playback_dirty = true;
            frame_count++;
            album_playback_current_ms =
                request->start_ms + (uint32_t)(frame_count * 1000 / PLAYBACK_FPS);
            if (request->duration_ms > 0 &&
                album_playback_current_ms > request->duration_ms) {
                album_playback_current_ms = request->duration_ms;
            }
        }
        pthread_mutex_unlock(&album_playback_mutex);
    }

    close(fd);
    waitpid(pid, NULL, 0);
    pthread_mutex_lock(&album_playback_mutex);
    album_playback_pid = -1;
    album_playback_failed = frame_count == 0;
    album_playback_finished = true;
    album_playback_active = false;
    album_playback_paused = false;
    pthread_mutex_unlock(&album_playback_mutex);

    free(frame_rgb);
    free(request->path);
    free(request);
    return NULL;
#else
    GstElement *pipeline = NULL;
    GstElement *source = NULL;
    GstElement *sink = NULL;
    GstBus *bus = NULL;
    GError *error = NULL;
    char pipeline_text[512];
    const size_t frame_rgb_size = (size_t)UI_WIDTH * UI_HEIGHT * 3;
    size_t frame_count = 0;
    bool failed = false;

    gst_init(NULL, NULL);
    snprintf(pipeline_text, sizeof(pipeline_text),
             "filesrc name=src ! decodebin ! videoconvert ! videoscale ! videorate ! "
             "video/x-raw,format=RGB,width=%d,height=%d,framerate=%d/1 ! "
             "appsink name=sink sync=true max-buffers=1 drop=true",
             UI_WIDTH, UI_HEIGHT, PLAYBACK_FPS);

    pipeline = gst_parse_launch(pipeline_text, &error);
    if (pipeline == NULL) {
        if (error != NULL) {
            fprintf(stderr, "GStreamer playback pipeline error: %s\n", error->message);
            g_error_free(error);
        }
        failed = true;
        goto done;
    }

    source = gst_bin_get_by_name(GST_BIN(pipeline), "src");
    sink = gst_bin_get_by_name(GST_BIN(pipeline), "sink");
    if (source == NULL || sink == NULL) {
        failed = true;
        goto done;
    }

    g_object_set(source, "location", request->path, NULL);
    g_object_set(sink, "emit-signals", FALSE, NULL);

    pthread_mutex_lock(&album_playback_mutex);
    album_playback_pipeline = pipeline;
    album_playback_current_ms = request->start_ms;
    album_playback_duration_ms = request->duration_ms;
    album_playback_stop_requested = false;
    pthread_mutex_unlock(&album_playback_mutex);

    printf("Inline playback with GStreamer appsink: %s\n", request->path);
    gst_element_set_state(pipeline, GST_STATE_PAUSED);
    gst_element_get_state(pipeline, NULL, NULL, 3 * GST_SECOND);

    gint64 duration_ns = 0;
    if (request->duration_ms == 0 &&
        gst_element_query_duration(pipeline, GST_FORMAT_TIME, &duration_ns) &&
        duration_ns > 0) {
        request->duration_ms = (uint32_t)(duration_ns / GST_MSECOND);
        pthread_mutex_lock(&album_playback_mutex);
        album_playback_duration_ms = request->duration_ms;
        pthread_mutex_unlock(&album_playback_mutex);
    }

    if (request->start_ms > 0) {
        gst_element_seek_simple(pipeline, GST_FORMAT_TIME,
                                GST_SEEK_FLAG_FLUSH | GST_SEEK_FLAG_KEY_UNIT,
                                (gint64)request->start_ms * GST_MSECOND);
    }

    gst_element_set_state(pipeline, GST_STATE_PLAYING);
    bus = gst_element_get_bus(pipeline);

    while (true) {
        pthread_mutex_lock(&album_playback_mutex);
        bool stop_requested = album_playback_stop_requested;
        pthread_mutex_unlock(&album_playback_mutex);
        if (stop_requested) {
            break;
        }

        if (bus != NULL) {
            GstMessage *message =
                gst_bus_pop_filtered(bus, GST_MESSAGE_ERROR | GST_MESSAGE_EOS);
            if (message != NULL) {
                if (GST_MESSAGE_TYPE(message) == GST_MESSAGE_ERROR) {
                    GError *msg_error = NULL;
                    gchar *debug = NULL;
                    gst_message_parse_error(message, &msg_error, &debug);
                    if (msg_error != NULL) {
                        fprintf(stderr, "GStreamer playback error: %s\n", msg_error->message);
                        g_error_free(msg_error);
                    }
                    g_free(debug);
                    failed = true;
                }
                gst_message_unref(message);
                break;
            }
        }

        GstSample *sample =
            gst_app_sink_try_pull_sample(GST_APP_SINK(sink), 100 * GST_MSECOND);
        if (sample == NULL) {
            continue;
        }

        GstBuffer *buffer = gst_sample_get_buffer(sample);
        GstMapInfo map;
        if (buffer != NULL && gst_buffer_map(buffer, &map, GST_MAP_READ)) {
            pthread_mutex_lock(&album_playback_mutex);
            if (album_view_pixels != NULL && map.size >= frame_rgb_size) {
                const uint8_t *frame_rgb = map.data;
                for (size_t i = 0; i < (size_t)UI_WIDTH * UI_HEIGHT; i++) {
                    const uint8_t *rgb = frame_rgb + i * 3;
                    album_view_pixels[i] = lv_color_make(rgb[0], rgb[1], rgb[2]);
                }
                gint64 position_ns = GST_CLOCK_TIME_NONE;
                if (gst_element_query_position(pipeline, GST_FORMAT_TIME, &position_ns) &&
                    position_ns > 0) {
                    album_playback_current_ms = (uint32_t)(position_ns / GST_MSECOND);
                } else if (GST_BUFFER_PTS_IS_VALID(buffer)) {
                    album_playback_current_ms =
                        request->start_ms + (uint32_t)(GST_BUFFER_PTS(buffer) / GST_MSECOND);
                } else {
                    album_playback_current_ms =
                        request->start_ms + (uint32_t)(frame_count * 1000 / PLAYBACK_FPS);
                }
                if (request->duration_ms > 0 &&
                    album_playback_current_ms > request->duration_ms) {
                    album_playback_current_ms = request->duration_ms;
                }
                album_playback_dirty = true;
                frame_count++;
            }
            pthread_mutex_unlock(&album_playback_mutex);
            gst_buffer_unmap(buffer, &map);
        }
        gst_sample_unref(sample);
    }

done:
    if (pipeline != NULL) {
        gst_element_set_state(pipeline, GST_STATE_NULL);
    }
    pthread_mutex_lock(&album_playback_mutex);
    if (album_playback_pipeline == pipeline) {
        album_playback_pipeline = NULL;
    }
    album_playback_failed = failed || frame_count == 0;
    album_playback_finished = true;
    album_playback_active = false;
    album_playback_paused = false;
    album_playback_stop_requested = false;
    pthread_mutex_unlock(&album_playback_mutex);

    if (bus != NULL) gst_object_unref(bus);
    if (sink != NULL) gst_object_unref(sink);
    if (source != NULL) gst_object_unref(source);
    if (pipeline != NULL) gst_object_unref(pipeline);
    free(request->path);
    free(request);
    return NULL;
#endif
}

static void album_playback_timer_cb(lv_timer_t * timer)
{
    (void)timer;

    pthread_mutex_lock(&album_playback_mutex);
    bool dirty = album_playback_dirty;
    bool finished = album_playback_finished;
    bool failed = album_playback_failed;
    uint32_t current_ms = album_playback_current_ms;
    uint32_t duration_ms = album_playback_duration_ms;
    album_playback_dirty = false;
    pthread_mutex_unlock(&album_playback_mutex);

    playback_progress_update_ui(current_ms, duration_ms);

    if (dirty && album_view_image != NULL) {
        lv_obj_clear_flag(album_view_image, LV_OBJ_FLAG_HIDDEN);
        lv_img_cache_invalidate_src(&album_view_dsc);
        lv_img_set_src(album_view_image, &album_view_dsc);
        lv_obj_invalidate(album_view_image);
    }

    if (finished) {
        if (failed && album_path_label != NULL) {
            lv_label_set_text(album_path_label, "Playback failed");
        } else if (album_path_label != NULL) {
            lv_label_set_text(album_path_label, "Playback finished");
        }
        if (album_play_btn != NULL && current_mode == MODE_VIDEO) {
            lv_obj_clear_flag(album_play_btn, LV_OBJ_FLAG_HIDDEN);
        }
        if (album_playback_thread_started) {
            pthread_join(album_playback_thread, NULL);
            album_playback_thread_started = false;
        }
        if (album_playback_timer != NULL) {
            lv_timer_del(album_playback_timer);
            album_playback_timer = NULL;
        }
    }
}

static void album_playback_stop(void)
{
    pthread_mutex_lock(&album_playback_mutex);
    pid_t pid = album_playback_pid;
#ifndef WSL
    GstElement *pipeline = album_playback_pipeline;
    album_playback_stop_requested = true;
#endif
    pthread_mutex_unlock(&album_playback_mutex);

#ifndef WSL
    if (pipeline != NULL) {
        gst_element_set_state(pipeline, GST_STATE_NULL);
    }
#endif
    if (pid > 0) {
        kill(-pid, SIGTERM);
        kill(-pid, SIGCONT);
    }
    if (album_playback_thread_started) {
        pthread_join(album_playback_thread, NULL);
        album_playback_thread_started = false;
    }
    if (album_playback_timer != NULL) {
        lv_timer_del(album_playback_timer);
        album_playback_timer = NULL;
    }

    pthread_mutex_lock(&album_playback_mutex);
    album_playback_pid = -1;
    album_playback_dirty = false;
    album_playback_finished = false;
    album_playback_failed = false;
    album_playback_active = false;
    album_playback_paused = false;
    album_playback_current_ms = 0;
    album_playback_stop_requested = false;
#ifndef WSL
    album_playback_pipeline = NULL;
#endif
    pthread_mutex_unlock(&album_playback_mutex);

    playback_progress_update_ui(0, album_playback_duration_ms);
}

static bool album_playback_set_paused(bool paused)
{
    pthread_mutex_lock(&album_playback_mutex);
    pid_t pid = album_playback_pid;
    bool active = album_playback_active;
#ifndef WSL
    GstElement *pipeline = album_playback_pipeline;
#endif
    pthread_mutex_unlock(&album_playback_mutex);

    if (!active) {
        return false;
    }

#ifndef WSL
    if (pipeline == NULL) {
        return false;
    }
    if (gst_element_set_state(pipeline, paused ? GST_STATE_PAUSED : GST_STATE_PLAYING) ==
        GST_STATE_CHANGE_FAILURE) {
        return false;
    }
#else
    if (pid <= 0) {
        return false;
    }
    if (kill(-pid, paused ? SIGSTOP : SIGCONT) != 0) {
        return false;
    }
#endif

    pthread_mutex_lock(&album_playback_mutex);
    album_playback_paused = paused;
    pthread_mutex_unlock(&album_playback_mutex);

    if (album_path_label != NULL) {
        lv_label_set_text(album_path_label, paused ? "Paused" : "Playing");
    }
    if (album_play_btn != NULL) {
        if (paused) {
            lv_obj_clear_flag(album_play_btn, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(album_play_btn, LV_OBJ_FLAG_HIDDEN);
        }
    }
    return true;
}

static void album_view_event_cb(lv_event_t * e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) {
        return;
    }

    pthread_mutex_lock(&album_playback_mutex);
    bool active = album_playback_active;
    bool paused = album_playback_paused;
    pthread_mutex_unlock(&album_playback_mutex);

    if (active && !paused) {
        album_playback_set_paused(true);
    }
}

static void album_progress_event_cb(lv_event_t * e)
{
    lv_event_code_t code = lv_event_get_code(e);

    if (current_mode != MODE_VIDEO || album_photo_count == 0 ||
        album_playback_duration_ms == 0) {
        return;
    }

    if (code == LV_EVENT_PRESSED) {
        album_progress_dragging = true;
        return;
    }

    if (code == LV_EVENT_VALUE_CHANGED && album_progress_dragging) {
        int32_t slider_value = lv_slider_get_value(album_progress_slider);
        uint32_t target_ms =
            (uint32_t)((uint64_t)clamp_i32(slider_value, 0, PLAYBACK_PROGRESS_RANGE) *
                       album_playback_duration_ms / PLAYBACK_PROGRESS_RANGE);
        playback_progress_update_ui(target_ms, album_playback_duration_ms);
        return;
    }

    if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
        int32_t slider_value = lv_slider_get_value(album_progress_slider);
        uint32_t target_ms =
            (uint32_t)((uint64_t)clamp_i32(slider_value, 0, PLAYBACK_PROGRESS_RANGE) *
                       album_playback_duration_ms / PLAYBACK_PROGRESS_RANGE);
        bool active;

        album_progress_dragging = false;
        pthread_mutex_lock(&album_playback_mutex);
        album_playback_current_ms = target_ms;
        active = album_playback_active;
#ifndef WSL
        GstElement *pipeline = album_playback_pipeline;
#endif
        pthread_mutex_unlock(&album_playback_mutex);

#ifndef WSL
        if (active && pipeline != NULL) {
            if (!gst_element_seek_simple(pipeline, GST_FORMAT_TIME,
                                         GST_SEEK_FLAG_FLUSH | GST_SEEK_FLAG_KEY_UNIT,
                                         (gint64)target_ms * GST_MSECOND)) {
                lv_label_set_text(album_path_label, "Seek failed");
            } else {
                lv_label_set_text(album_path_label, "Playing");
                playback_progress_update_ui(target_ms, album_playback_duration_ms);
            }
            return;
        }
#else
        (void)active;
#endif

        if (!album_playback_start(album_photo_paths[album_photo_index])) {
            lv_label_set_text(album_path_label, "Playback failed");
            playback_progress_update_ui(target_ms, album_playback_duration_ms);
        } else {
            lv_label_set_text(album_path_label, "Playing");
        }
    }
}

static bool album_playback_start(const char * path)
{
    playback_request_t * request;
    uint32_t start_ms = album_playback_current_ms;
    uint32_t duration_ms = album_playback_duration_ms;

    if (album_view_pixels == NULL || path == NULL) {
        return false;
    }

    album_playback_stop();

    request = calloc(1, sizeof(*request));
    if (request == NULL) return false;
    request->path = string_duplicate(path);
    if (request->path == NULL) {
        free(request);
        return false;
    }
    request->start_ms = start_ms;
    request->duration_ms = duration_ms;

    memset(album_view_pixels, 0, (size_t)UI_WIDTH * UI_HEIGHT * sizeof(lv_color_t));
    lv_obj_clear_flag(album_view_image, LV_OBJ_FLAG_HIDDEN);
    lv_img_set_src(album_view_image, &album_view_dsc);
    lv_obj_invalidate(album_view_image);

    pthread_mutex_lock(&album_playback_mutex);
    album_playback_dirty = false;
    album_playback_finished = false;
    album_playback_failed = false;
    album_playback_active = true;
    album_playback_paused = false;
    album_playback_current_ms = start_ms;
    album_playback_duration_ms = duration_ms;
    album_playback_stop_requested = false;
    pthread_mutex_unlock(&album_playback_mutex);
    playback_progress_update_ui(start_ms, duration_ms);
    if (album_play_btn != NULL) {
        lv_obj_add_flag(album_play_btn, LV_OBJ_FLAG_HIDDEN);
    }

    if (pthread_create(&album_playback_thread, NULL,
                       album_playback_thread_main, request) != 0) {
        pthread_mutex_lock(&album_playback_mutex);
        album_playback_active = false;
        pthread_mutex_unlock(&album_playback_mutex);
        free(request->path);
        free(request);
        return false;
    }
    album_playback_thread_started = true;
    album_playback_timer = lv_timer_create(album_playback_timer_cb, 33, NULL);
    return album_playback_timer != NULL;
}

static void album_photo_list_clear(void)
{
    for (size_t i = 0; i < album_photo_count; i++) {
        free(album_photo_paths[i]);
    }
    free(album_photo_paths);
    album_photo_paths = NULL;
    album_photo_count = 0;
    album_photo_index = 0;
}

static int album_path_compare_desc(const void * left, const void * right)
{
    const char * const * a = left;
    const char * const * b = right;
    return strcmp(*b, *a);
}

static bool album_photo_list_add(const char * path)
{
    char ** next = realloc(album_photo_paths,
                           (album_photo_count + 1) * sizeof(album_photo_paths[0]));
    if (next == NULL) return false;

    album_photo_paths = next;
    album_photo_paths[album_photo_count] = string_duplicate(path);
    if (album_photo_paths[album_photo_count] == NULL) return false;

    album_photo_count++;
    return true;
}

static void album_photo_list_scan(void)
{
    const char * media_dir = current_mode == MODE_VIDEO ? video_dir_get() : photo_dir_get();
    DIR * dir;
    struct dirent * entry;

    album_photo_list_clear();
    dir = opendir(media_dir);
    if (dir == NULL) {
        return;
    }

    while ((entry = readdir(dir)) != NULL) {
        char path[192];

        if (entry->d_name[0] == '.') {
            continue;
        }
        if (current_mode == MODE_VIDEO && !has_video_extension(entry->d_name)) {
            continue;
        }
        if (current_mode == MODE_PHOTO && !has_jpeg_extension(entry->d_name)) {
            continue;
        }

        snprintf(path, sizeof(path), "%s/%s", media_dir, entry->d_name);
        if (!album_photo_list_add(path)) {
            break;
        }
    }
    closedir(dir);

    if (album_photo_count > 1) {
        qsort(album_photo_paths, album_photo_count,
              sizeof(album_photo_paths[0]), album_path_compare_desc);
    }

    if (current_mode == MODE_PHOTO && album_photo_count == 0 && album_image_path[0] != '\0') {
        album_photo_list_add(album_image_path);
    }
}

static void album_view_update(void)
{
    if (album_overlay == NULL || album_view_image == NULL ||
        album_index_label == NULL || album_path_label == NULL) {
        return;
    }

    if (album_photo_count == 0) {
        lv_obj_add_flag(album_view_image, LV_OBJ_FLAG_HIDDEN);
        if (album_delete_btn != NULL) {
            lv_obj_add_flag(album_delete_btn, LV_OBJ_FLAG_HIDDEN);
        }
        if (album_play_btn != NULL) {
            lv_obj_add_flag(album_play_btn, LV_OBJ_FLAG_HIDDEN);
        }
        lv_label_set_text(album_index_label, "0 / 0");
        lv_label_set_text(album_path_label, current_mode == MODE_VIDEO ? "No videos" : "No photos");
        album_playback_current_ms = 0;
        album_playback_duration_ms = 0;
        playback_progress_update_ui(0, 0);
        return;
    }

    lv_label_set_text_fmt(album_index_label, "%u / %u",
                          (unsigned)(album_photo_index + 1),
                          (unsigned)album_photo_count);
    lv_label_set_text(album_path_label, album_photo_paths[album_photo_index]);
    if (album_delete_btn != NULL) {
        lv_obj_clear_flag(album_delete_btn, LV_OBJ_FLAG_HIDDEN);
    }
    if (album_play_btn != NULL) {
        if (current_mode == MODE_VIDEO) {
            lv_obj_clear_flag(album_play_btn, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(album_play_btn, LV_OBJ_FLAG_HIDDEN);
        }
    }

    if (current_mode == MODE_VIDEO) {
        lv_obj_add_flag(album_view_image, LV_OBJ_FLAG_HIDDEN);
        album_playback_current_ms = 0;
        album_playback_duration_ms = 0;
        playback_progress_update_ui(0, album_playback_duration_ms);
        return;
    }

    album_playback_current_ms = 0;
    album_playback_duration_ms = 0;
    playback_progress_update_ui(0, 0);

    if (album_view_pixels == NULL ||
        camera_media_load_jpeg(album_photo_paths[album_photo_index],
                               album_view_pixels, UI_WIDTH, UI_HEIGHT) != 0) {
        lv_obj_add_flag(album_view_image, LV_OBJ_FLAG_HIDDEN);
        if (album_delete_btn != NULL) {
            lv_obj_add_flag(album_delete_btn, LV_OBJ_FLAG_HIDDEN);
        }
        if (album_play_btn != NULL) {
            lv_obj_add_flag(album_play_btn, LV_OBJ_FLAG_HIDDEN);
        }
        lv_label_set_text(album_path_label, "Unable to load photo");
        return;
    }

    lv_obj_clear_flag(album_view_image, LV_OBJ_FLAG_HIDDEN);
    lv_img_cache_invalidate_src(&album_view_dsc);
    lv_img_set_src(album_view_image, &album_view_dsc);
    lv_obj_invalidate(album_view_image);
}

static int32_t clamp_i32(int32_t value, int32_t min, int32_t max)
{
    if (value < min) return min;
    if (value > max) return max;
    return value;
}

static void quick_curtain_set_y(void * var, int32_t y)
{
    lv_obj_set_y((lv_obj_t *)var, y);
}

static void settings_panel_set_x(void * var, int32_t x)
{
    lv_obj_set_x((lv_obj_t *)var, x);
}

static void record_settings_panel_set_y(void * var, int32_t y)
{
    lv_obj_set_y((lv_obj_t *)var, y);
}

static void quick_curtain_delete_ready_cb(lv_anim_t * anim)
{
    lv_obj_t * curtain = (lv_obj_t *)anim->var;
    if (curtain != NULL) {
        lv_obj_del(curtain);
    }
    if (quick_curtain == curtain) {
        quick_curtain = NULL;
    }
    quick_curtain_dragging = false;
    global_quick_swipe_tracking = false;
    global_quick_open_pending = false;
}

static void quick_curtain_open_ready_cb(lv_anim_t * anim)
{
    (void)anim;
    quick_curtain_dragging = false;
    global_quick_swipe_tracking = false;
    global_quick_open_pending = false;
}

static void quick_curtain_anim_to(int32_t target_y, lv_anim_ready_cb_t ready_cb)
{
    if (quick_curtain == NULL) {
        return;
    }

    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, quick_curtain);
    lv_anim_set_exec_cb(&a, quick_curtain_set_y);
    lv_anim_set_values(&a, lv_obj_get_y(quick_curtain), target_y);
    lv_anim_set_time(&a, 180);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_set_ready_cb(&a, ready_cb);
    lv_anim_start(&a);
}

static void quick_curtain_ensure(void)
{
    if (quick_curtain != NULL) {
        return;
    }

    quick_curtain = quick_settings_page_create_curtain(lv_scr_act());
    lv_obj_set_y(quick_curtain, -UI_HEIGHT);
    lv_obj_move_foreground(quick_curtain);
}

static void quick_curtain_set_delta(int32_t delta_y)
{
    if (quick_curtain == NULL) {
        return;
    }

    int32_t clamped = clamp_i32(delta_y, 0, UI_HEIGHT);
    lv_obj_set_y(quick_curtain, clamped - UI_HEIGHT);
}

static void quick_curtain_finish(int32_t delta_y)
{
    if (quick_curtain == NULL) {
        quick_curtain_dragging = false;
        return;
    }

    if (delta_y >= UI_HEIGHT / 2) {
        camera_page_active = false;
        clear_camera_page_refs();
        quick_curtain_anim_to(0, quick_curtain_open_ready_cb);
    } else {
        quick_curtain_anim_to(-UI_HEIGHT, quick_curtain_delete_ready_cb);
    }
}

static void quick_settings_swipe_self_test(void)
{
    assert(is_quick_settings_swipe(0, 80));
    assert(is_quick_settings_swipe(90, 150));
    assert(!is_quick_settings_swipe(120, 220));
    assert(!is_quick_settings_swipe(20, 52));
    assert(!is_quick_settings_swipe(20, 0));
}

static lv_obj_t *label_create(lv_obj_t * parent, const char * text, lv_color_t color, const lv_font_t * font)
{
    lv_obj_t * label = lv_label_create(parent);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_color(label, color, 0);
    lv_obj_set_style_text_font(label, font, 0);
    return label;
}

static void record_settings_get_text(char * out, size_t out_size)
{
    snprintf(out,
             out_size,
             "%s%u",
             record_resolution_options[record_resolution_index].label,
             record_fps_options[record_fps_index]);
}

static void record_option_button_set_selected(lv_obj_t * btn, bool selected)
{
    if (btn == NULL) {
        return;
    }

    lv_obj_set_style_bg_color(btn, selected ? c_yellow() : lv_color_black(), 0);
    lv_obj_set_style_bg_opa(btn, selected ? 255 : 132, 0);
    lv_obj_set_style_border_color(btn, selected ? c_yellow() : lv_color_white(), 0);
    lv_obj_set_style_border_opa(btn, selected ? 255 : 112, 0);

    lv_obj_t * label = lv_obj_get_child(btn, 0);
    if (label != NULL) {
        lv_obj_set_style_text_color(label, selected ? lv_color_black() : lv_color_white(), 0);
    }
}

static void record_settings_update_ui(void)
{
    char detail[16];

    for (size_t i = 0; i < sizeof(record_resolution_btns) / sizeof(record_resolution_btns[0]); i++) {
        record_option_button_set_selected(record_resolution_btns[i],
                                          i == record_resolution_index);
    }
    for (size_t i = 0; i < sizeof(record_fps_btns) / sizeof(record_fps_btns[0]); i++) {
        record_option_button_set_selected(record_fps_btns[i], i == record_fps_index);
    }

    if (mode_detail_label != NULL && current_mode == MODE_VIDEO) {
        record_settings_get_text(detail, sizeof(detail));
        lv_label_set_text(mode_detail_label, detail);
    }
}

static void record_resolution_event_cb(lv_event_t * e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED || is_recording) {
        return;
    }

    uintptr_t index = (uintptr_t)lv_event_get_user_data(e);
    if (index < sizeof(record_resolution_options) / sizeof(record_resolution_options[0])) {
        record_resolution_index = (uint8_t)index;
        record_settings_update_ui();
    }
}

static void record_fps_event_cb(lv_event_t * e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED || is_recording) {
        return;
    }

    uintptr_t index = (uintptr_t)lv_event_get_user_data(e);
    if (index < sizeof(record_fps_options) / sizeof(record_fps_options[0])) {
        record_fps_index = (uint8_t)index;
        record_settings_update_ui();
    }
}

static void update_record_time(void)
{
    uint32_t m = rec_seconds / 60;
    uint32_t s = rec_seconds % 60;
    uint32_t h = m / 60;
    m = m % 60;

    if (h > 0) {
        lv_label_set_text_fmt(rec_time_label, "%02u:%02u:%02u", h, m, s);
    } else {
        lv_label_set_text_fmt(rec_time_label, "%02u:%02u", m, s);
    }
}

static void update_mode_ui(void)
{
    lv_obj_clear_flag(rec_time_label, LV_OBJ_FLAG_HIDDEN);
    if (current_mode == MODE_VIDEO) {
        char detail[16];
        lv_label_set_text(mode_label, LV_SYMBOL_VIDEO);
        lv_label_set_text(rec_time_label, "00:00");
        record_settings_get_text(detail, sizeof(detail));
        lv_label_set_text(mode_detail_label, detail);
        lv_obj_set_style_bg_color(shutter_icon, c_red(), 0);
        lv_obj_set_style_bg_opa(shutter_icon, 255, 0);
        lv_obj_set_style_border_width(shutter_icon, 0, 0);
        lv_obj_set_style_radius(shutter_icon, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_size(shutter_icon, 52, 52);
        lv_obj_clear_flag(album_btn, LV_OBJ_FLAG_HIDDEN);
    } else if (current_mode == MODE_PHOTO) {
        lv_label_set_text(mode_label, LV_SYMBOL_IMAGE);
        lv_label_set_text(rec_time_label, "12MP");
        lv_label_set_text(mode_detail_label, "PHOTO");
        lv_obj_set_style_bg_color(shutter_icon, lv_color_white(), 0);
        lv_obj_set_style_bg_opa(shutter_icon, 255, 0);
        lv_obj_set_style_border_width(shutter_icon, 0, 0);
        lv_obj_set_style_radius(shutter_icon, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_size(shutter_icon, 54, 54);
        lv_obj_clear_flag(album_btn, LV_OBJ_FLAG_HIDDEN);
    }
    lv_obj_center(shutter_icon);
    lv_obj_add_flag(rec_dot, LV_OBJ_FLAG_HIDDEN);
}

static void mode_btn_event_cb(lv_event_t * e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) {
        return;
    }

    if (is_recording && record_callback != NULL) {
        camera_page_record_settings_t settings = {
            record_resolution_options[record_resolution_index].width,
            record_resolution_options[record_resolution_index].height,
            record_fps_options[record_fps_index],
        };
        record_callback(false, &settings, record_callback_user_data);
    }
    is_recording = false;
    rec_seconds = 0;
    current_mode = current_mode == MODE_VIDEO ? MODE_PHOTO : MODE_VIDEO;
    update_mode_ui();
}

static void album_close_event_cb(lv_event_t * e)
{
    if (lv_event_get_code(e) == LV_EVENT_CLICKED && album_overlay != NULL) {
        album_playback_stop();
        lv_obj_del(album_overlay);
        album_overlay = NULL;
        album_view_image = NULL;
        album_index_label = NULL;
        album_path_label = NULL;
        album_delete_btn = NULL;
        album_play_btn = NULL;
        album_progress_slider = NULL;
        album_progress_time_label = NULL;
        free(album_view_pixels);
        album_view_pixels = NULL;
        album_photo_list_clear();
    }
}

static void album_prev_event_cb(lv_event_t * e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED || album_photo_count == 0) {
        return;
    }

    album_playback_stop();
    if (album_photo_index == 0) {
        album_photo_index = album_photo_count - 1;
    } else {
        album_photo_index--;
    }
    album_view_update();
}

static void album_next_event_cb(lv_event_t * e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED || album_photo_count == 0) {
        return;
    }

    album_playback_stop();
    album_photo_index = (album_photo_index + 1) % album_photo_count;
    album_view_update();
}

static void album_play_event_cb(lv_event_t * e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED ||
        album_photo_count == 0 ||
        current_mode != MODE_VIDEO) {
        return;
    }

    pthread_mutex_lock(&album_playback_mutex);
    bool active = album_playback_active;
    bool paused = album_playback_paused;
    pthread_mutex_unlock(&album_playback_mutex);

    if (active && paused) {
        if (!album_playback_set_paused(false)) {
            lv_label_set_text(album_path_label, "Playback failed");
        }
        return;
    }
    if (active) {
        return;
    }

    if (album_playback_duration_ms > 0 &&
        album_playback_current_ms >= album_playback_duration_ms) {
        album_playback_current_ms = 0;
    }
    bool ok = album_playback_start(album_photo_paths[album_photo_index]);
    if (!ok) {
        lv_label_set_text(album_path_label, "Playback failed");
    } else {
        lv_label_set_text(album_path_label, "Playing");
    }
}

static void album_delete_event_cb(lv_event_t * e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED || album_photo_count == 0) {
        return;
    }

    album_playback_stop();
    char * deleted_path = album_photo_paths[album_photo_index];
    bool deleted_latest = strcmp(album_image_path, deleted_path) == 0;
    if (remove(deleted_path) != 0) {
        perror("Unable to delete photo");
        lv_label_set_text(album_path_label, "Delete failed");
        return;
    }

    free(deleted_path);
    for (size_t i = album_photo_index; i + 1 < album_photo_count; i++) {
        album_photo_paths[i] = album_photo_paths[i + 1];
    }
    album_photo_count--;
    if (album_photo_count == 0) {
        free(album_photo_paths);
        album_photo_paths = NULL;
        album_photo_index = 0;
    } else {
        char ** next = realloc(album_photo_paths,
                               album_photo_count * sizeof(album_photo_paths[0]));
        if (next != NULL) {
            album_photo_paths = next;
        }
        if (album_photo_index >= album_photo_count) {
            album_photo_index = album_photo_count - 1;
        }
    }

    if (deleted_latest) {
        album_image_path[0] = '\0';
        album_image_source = NULL;
    }
    album_view_update();
}

static void album_btn_event_cb(lv_event_t * e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED || album_overlay != NULL) {
        return;
    }

    album_photo_list_scan();
    album_view_pixels = malloc((size_t)UI_WIDTH * UI_HEIGHT * sizeof(lv_color_t));
    if (album_view_pixels == NULL) {
        album_photo_list_clear();
        return;
    }
    memset(&album_view_dsc, 0, sizeof(album_view_dsc));
    album_view_dsc.header.always_zero = 0;
    album_view_dsc.header.w = UI_WIDTH;
    album_view_dsc.header.h = UI_HEIGHT;
    album_view_dsc.header.cf = LV_IMG_CF_TRUE_COLOR;
    album_view_dsc.data_size = UI_WIDTH * UI_HEIGHT * sizeof(lv_color_t);
    album_view_dsc.data = (const uint8_t *)album_view_pixels;

    album_overlay = lv_obj_create(lv_scr_act());
    lv_obj_set_size(album_overlay, UI_WIDTH, UI_HEIGHT);
    lv_obj_center(album_overlay);
    lv_obj_set_style_bg_color(album_overlay, lv_color_black(), 0);
    lv_obj_set_style_border_width(album_overlay, 0, 0);
    lv_obj_set_style_radius(album_overlay, 0, 0);
    lv_obj_set_style_pad_all(album_overlay, 0, 0);
    lv_obj_move_foreground(album_overlay);

    album_view_image = lv_img_create(album_overlay);
    lv_obj_set_size(album_view_image, UI_WIDTH, UI_HEIGHT);
    lv_obj_center(album_view_image);
    lv_obj_add_flag(album_view_image, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(album_view_image, album_view_event_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t * close_btn = lv_btn_create(album_overlay);
    lv_obj_set_size(close_btn, 64, 52);
    lv_obj_align(close_btn, LV_ALIGN_TOP_LEFT, 18, 18);
    lv_obj_set_style_bg_color(close_btn, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(close_btn, 150, 0);
    lv_obj_set_style_border_width(close_btn, 0, 0);
    lv_obj_add_event_cb(close_btn, album_close_event_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t * close_label =
        label_create(close_btn, LV_SYMBOL_LEFT, lv_color_white(), &lv_font_montserrat_28);
    lv_obj_center(close_label);

    lv_obj_t * prev_btn = lv_btn_create(album_overlay);
    lv_obj_set_size(prev_btn, 64, 80);
    lv_obj_align(prev_btn, LV_ALIGN_LEFT_MID, 18, 0);
    lv_obj_set_style_bg_color(prev_btn, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(prev_btn, 120, 0);
    lv_obj_set_style_border_width(prev_btn, 0, 0);
    lv_obj_add_event_cb(prev_btn, album_prev_event_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t * prev_label =
        label_create(prev_btn, LV_SYMBOL_LEFT, lv_color_white(), &lv_font_montserrat_28);
    lv_obj_center(prev_label);

    lv_obj_t * next_btn = lv_btn_create(album_overlay);
    lv_obj_set_size(next_btn, 64, 80);
    lv_obj_align(next_btn, LV_ALIGN_RIGHT_MID, -18, 0);
    lv_obj_set_style_bg_color(next_btn, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(next_btn, 120, 0);
    lv_obj_set_style_border_width(next_btn, 0, 0);
    lv_obj_add_event_cb(next_btn, album_next_event_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t * next_label =
        label_create(next_btn, LV_SYMBOL_RIGHT, lv_color_white(), &lv_font_montserrat_28);
    lv_obj_center(next_label);

    album_delete_btn = lv_btn_create(album_overlay);
    lv_obj_set_size(album_delete_btn, 64, 52);
    lv_obj_align(album_delete_btn, LV_ALIGN_BOTTOM_LEFT, 18, -18);
    lv_obj_set_style_bg_color(album_delete_btn, c_red(), 0);
    lv_obj_set_style_bg_opa(album_delete_btn, 190, 0);
    lv_obj_set_style_border_width(album_delete_btn, 0, 0);
    lv_obj_add_event_cb(album_delete_btn, album_delete_event_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t * delete_label =
        label_create(album_delete_btn, LV_SYMBOL_TRASH, lv_color_white(), &lv_font_montserrat_28);
    lv_obj_center(delete_label);

    album_play_btn = lv_btn_create(album_overlay);
    lv_obj_set_size(album_play_btn, 90, 64);
    lv_obj_align(album_play_btn, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(album_play_btn, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(album_play_btn, 170, 0);
    lv_obj_set_style_border_color(album_play_btn, lv_color_white(), 0);
    lv_obj_set_style_border_width(album_play_btn, 2, 0);
    lv_obj_set_style_radius(album_play_btn, 8, 0);
    lv_obj_add_event_cb(album_play_btn, album_play_event_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t * play_label =
        label_create(album_play_btn, LV_SYMBOL_PLAY, lv_color_white(), &lv_font_montserrat_28);
    lv_obj_center(play_label);

    album_index_label = label_create(album_overlay, "", lv_color_white(), UI_TEXT_FONT);
    lv_obj_align(album_index_label, LV_ALIGN_TOP_MID, 0, 22);

    album_path_label = label_create(album_overlay, "", lv_color_white(), UI_TEXT_FONT);
    lv_obj_set_width(album_path_label, UI_WIDTH - 160);
    lv_label_set_long_mode(album_path_label, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_align(album_path_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(album_path_label, LV_ALIGN_BOTTOM_MID, 0, -18);

    album_progress_slider = lv_slider_create(album_overlay);
    lv_obj_set_size(album_progress_slider, 500, 18);
    lv_obj_align(album_progress_slider, LV_ALIGN_BOTTOM_MID, 0, -58);
    lv_slider_set_range(album_progress_slider, 0, PLAYBACK_PROGRESS_RANGE);
    lv_slider_set_value(album_progress_slider, 0, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(album_progress_slider, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(album_progress_slider, 160, LV_PART_MAIN);
    lv_obj_set_style_bg_color(album_progress_slider, c_yellow(), LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(album_progress_slider, lv_color_white(), LV_PART_KNOB);
    lv_obj_set_style_pad_all(album_progress_slider, 0, LV_PART_KNOB);
    lv_obj_add_event_cb(album_progress_slider, album_progress_event_cb, LV_EVENT_ALL, NULL);

    album_progress_time_label = label_create(album_overlay, "00:00 / 00:00",
                                             lv_color_white(), UI_TEXT_FONT);
    lv_obj_align(album_progress_time_label, LV_ALIGN_BOTTOM_MID, 0, -82);

    album_view_update();
}

static void rec_timer_cb(lv_timer_t * timer)
{
    (void)timer;

    if (!camera_page_active || rec_dot == NULL || rec_time_label == NULL) {
        return;
    }

    if (is_recording && current_mode == MODE_VIDEO) {
        rec_seconds++;
        update_record_time();
        if (lv_obj_has_flag(rec_dot, LV_OBJ_FLAG_HIDDEN)) {
            lv_obj_clear_flag(rec_dot, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(rec_dot, LV_OBJ_FLAG_HIDDEN);
        }
    } else {
        lv_obj_add_flag(rec_dot, LV_OBJ_FLAG_HIDDEN);
    }
}

static void shutter_btn_event_cb(lv_event_t * e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) {
        return;
    }

    if (current_mode == MODE_VIDEO) {
        bool next_recording = !is_recording;
        camera_page_record_settings_t settings = {
            record_resolution_options[record_resolution_index].width,
            record_resolution_options[record_resolution_index].height,
            record_fps_options[record_fps_index],
        };
        bool ok = record_callback == NULL ||
                  record_callback(next_recording, &settings, record_callback_user_data);
        if (!ok) {
            printf(next_recording ? "Camera recording start failed.\n"
                                  : "Camera recording stop failed.\n");
            return;
        }

        is_recording = next_recording;
        if (is_recording) {
            printf("Camera recording started...\n");
            lv_obj_set_style_radius(shutter_icon, 5, 0);
            lv_obj_set_size(shutter_icon, 30, 30);
            lv_obj_center(shutter_icon);
        } else {
            printf("Camera recording stopped...\n");
            is_recording = false;
            rec_seconds = 0;
            update_mode_ui();
        }
    } else if (current_mode == MODE_PHOTO) {
        bool saved = capture_callback != NULL &&
                     capture_callback(capture_callback_user_data);
        printf(saved ? "Photo captured.\n" : "Photo capture failed.\n");
        lv_obj_t * flash = lv_obj_create(lv_scr_act());
        lv_obj_set_size(flash, UI_WIDTH, UI_HEIGHT);
        lv_obj_set_style_bg_color(flash, lv_color_white(), 0);
        lv_obj_set_style_bg_opa(flash, 220, 0);
        lv_obj_set_style_border_width(flash, 0, 0);
        lv_obj_set_style_radius(flash, 0, 0);
        lv_obj_center(flash);
        lv_obj_del_delayed(flash, 120);
    }
}

static void zoom_btn_event_cb(lv_event_t * e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) {
        return;
    }

    lv_obj_t * btn = lv_event_get_target(e);
    lv_obj_t * label = lv_obj_get_child(btn, 0);
    const char * txt = lv_label_get_text(label);
    if (strcmp(txt, "1.0x") == 0) {
        lv_label_set_text(label, "2.0x");
    } else if (strcmp(txt, "2.0x") == 0) {
        lv_label_set_text(label, "4.0x");
    } else {
        lv_label_set_text(label, "1.0x");
    }
}

static void settings_panel_set_open(bool open)
{
    if (settings_panel == NULL || settings_panel_open == open) {
        return;
    }

    settings_panel_open = open;

    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, settings_panel);
    lv_anim_set_exec_cb(&a, settings_panel_set_x);
    lv_anim_set_values(&a, lv_obj_get_x(settings_panel), open ? UI_WIDTH - SETTINGS_PANEL_W : UI_WIDTH);
    lv_anim_set_time(&a, 180);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_start(&a);
}

static void record_settings_panel_set_open(bool open)
{
    if (record_settings_panel == NULL) {
        return;
    }
    if (record_settings_panel_open == open && !record_settings_dragging) {
        return;
    }

    record_settings_panel_open = open;

    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, record_settings_panel);
    lv_anim_set_exec_cb(&a, record_settings_panel_set_y);
    lv_anim_set_values(&a,
                       lv_obj_get_y(record_settings_panel),
                       open ? UI_HEIGHT - RECORD_SETTINGS_PANEL_H : UI_HEIGHT);
    lv_anim_set_time(&a, 180);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_start(&a);
}

static void record_settings_set_drag_y(int32_t y)
{
    if (record_settings_panel == NULL) {
        return;
    }

    int32_t next_y = record_settings_drag_start_y + y - record_settings_start_y;
    lv_obj_set_y(record_settings_panel,
                 clamp_i32(next_y, UI_HEIGHT - RECORD_SETTINGS_PANEL_H, UI_HEIGHT));
}

static void record_settings_finish(void)
{
    bool open = false;

    if (record_settings_panel != NULL) {
        int32_t current_y = lv_obj_get_y(record_settings_panel);
        open = current_y <= UI_HEIGHT - RECORD_SETTINGS_PANEL_H / 2;
    }

    record_settings_dragging = false;
    global_record_settings_swipe_tracking = false;
    record_settings_panel_set_open(open);
}

static void settings_event_cb(lv_event_t * e)
{
    lv_event_code_t code = lv_event_get_code(e);

    if (code == LV_EVENT_CLICKED) {
        settings_panel_set_open(!settings_panel_open);
        return;
    }

    if (code != LV_EVENT_GESTURE) {
        return;
    }

    lv_indev_t * indev = lv_indev_get_act();
    if (indev == NULL) {
        return;
    }

    lv_dir_t dir = lv_indev_get_gesture_dir(indev);
    if (dir == LV_DIR_LEFT) {
        settings_panel_set_open(true);
    } else if (dir == LV_DIR_RIGHT) {
        settings_panel_set_open(false);
    }
}

static bool is_quick_settings_swipe(int32_t start_y, int32_t end_y)
{
    return start_y <= QUICK_EDGE_TOUCH_H && end_y - start_y >= QUICK_SWIPE_MIN_DIST;
}

void camera_page_pointer_sample(bool pressed, int32_t x, int32_t y)
{
    (void)x;

    if (!camera_page_active) {
        global_quick_swipe_tracking = false;
        global_record_settings_swipe_tracking = false;
        return;
    }

    if (pressed && !global_quick_swipe_tracking && !global_record_settings_swipe_tracking) {
        bool record_panel_touch =
            record_settings_panel_open && y >= UI_HEIGHT - RECORD_SETTINGS_PANEL_H;
        global_quick_swipe_start_y = y;
        global_quick_swipe_tracking = y <= QUICK_EDGE_TOUCH_H;
        global_record_settings_swipe_tracking =
            record_panel_touch || y >= UI_HEIGHT - RECORD_SETTINGS_EDGE_TOUCH_H;
        quick_curtain_dragging = false;
        quick_curtain_start_y = y;
        record_settings_dragging = false;
        record_settings_start_y = y;
        record_settings_drag_start_y =
            record_settings_panel != NULL ? lv_obj_get_y(record_settings_panel) : UI_HEIGHT;
        return;
    }

    if (!pressed) {
        if (quick_curtain_dragging) {
            quick_curtain_finish(y - quick_curtain_start_y);
        }
        if (record_settings_dragging) {
            record_settings_finish();
        }
        quick_curtain_dragging = false;
        record_settings_dragging = false;
        global_quick_swipe_tracking = false;
        global_record_settings_swipe_tracking = false;
        return;
    }

    if (quick_curtain_dragging) {
        quick_curtain_set_delta(y - quick_curtain_start_y);
        return;
    }

    if (record_settings_dragging) {
        record_settings_set_drag_y(y);
        return;
    }

    if (global_quick_swipe_tracking && !global_quick_open_pending &&
        y - quick_curtain_start_y >= QUICK_DRAG_START_DIST) {
        quick_curtain_ensure();
        quick_curtain_dragging = true;
        is_recording = false;
        quick_curtain_set_delta(y - quick_curtain_start_y);
    }

    if (global_record_settings_swipe_tracking &&
        abs(y - record_settings_start_y) >= QUICK_DRAG_START_DIST) {
        record_settings_dragging = true;
        record_settings_set_drag_y(y);
    }
}

static void quick_settings_edge_cb(lv_event_t * e)
{
    lv_event_code_t code = lv_event_get_code(e);
    lv_indev_t * indev = lv_indev_get_act();

    if (indev == NULL) {
        return;
    }

    if (code == LV_EVENT_GESTURE && lv_indev_get_gesture_dir(indev) == LV_DIR_BOTTOM) {
        quick_curtain_ensure();
        quick_curtain_set_delta(UI_HEIGHT);
        camera_page_active = false;
        clear_camera_page_refs();
        quick_curtain_anim_to(0, quick_curtain_open_ready_cb);
    }
}

static void record_settings_edge_cb(lv_event_t * e)
{
    lv_event_code_t code = lv_event_get_code(e);
    lv_indev_t * indev = lv_indev_get_act();

    lv_event_stop_bubbling(e);

    if (indev == NULL) {
        return;
    }

    if (code == LV_EVENT_GESTURE) {
        lv_dir_t dir = lv_indev_get_gesture_dir(indev);
        if (dir == LV_DIR_TOP) {
            record_settings_panel_set_open(true);
        } else if (dir == LV_DIR_BOTTOM) {
            record_settings_panel_set_open(false);
        }
    }
}

static lv_obj_t *main_icon_create(lv_obj_t * parent, const char * icon, int32_t w, int32_t h)
{
    lv_obj_t * box = lv_obj_create(parent);
    lv_obj_set_size(box, w, h);
    lv_obj_set_style_bg_opa(box, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(box, 0, 0);
    lv_obj_set_style_radius(box, 0, 0);
    lv_obj_set_style_pad_all(box, 0, 0);

    lv_obj_t * label = label_create(box, icon, lv_color_white(), UI_TEXT_FONT);
    lv_obj_center(label);
    return box;
}

static lv_obj_t *zoom_control_create(lv_obj_t * parent)
{
    lv_obj_t * zoom = lv_obj_create(parent);
    lv_obj_set_size(zoom, 72, 156);
    lv_obj_set_style_bg_color(zoom, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(zoom, 86, 0);
    lv_obj_set_style_border_width(zoom, 0, 0);
    lv_obj_set_style_radius(zoom, 36, 0);
    lv_obj_set_style_pad_all(zoom, 0, 0);
    lv_obj_add_flag(zoom, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(zoom, zoom_btn_event_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t * value = label_create(zoom, "1.0x", lv_color_white(), UI_TEXT_FONT);
    lv_obj_align(value, LV_ALIGN_CENTER, 0, -20);

    lv_obj_t * minus = label_create(zoom, LV_SYMBOL_MINUS, lv_color_white(), UI_TEXT_FONT);
    lv_obj_align(minus, LV_ALIGN_CENTER, 0, 36);
    return zoom;
}

static lv_obj_t *top_menu_icon_create(lv_obj_t * parent, const char * text, bool active)
{
    lv_obj_t * box = lv_obj_create(parent);
    lv_obj_set_size(box, active ? 82 : 52, 52);
    lv_obj_set_style_bg_color(box, active ? c_yellow() : lv_color_black(), 0);
    lv_obj_set_style_bg_opa(box, active ? 255 : LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(box, 0, 0);
    lv_obj_set_style_radius(box, active ? 10 : 0, 0);
    lv_obj_set_style_pad_all(box, 0, 0);

    lv_obj_t * label = label_create(box, text, active ? lv_color_black() : lv_color_white(), UI_TEXT_FONT);
    lv_obj_center(label);
    return box;
}

static void add_settings_tile(lv_obj_t * parent,
                              const char * title,
                              const char * value,
                              const char * badge,
                              int32_t col,
                              int32_t row)
{
    lv_obj_t * tile = lv_btn_create(parent);
    lv_obj_set_size(tile, 164, 76);
    lv_obj_set_pos(tile, 24 + col * 184, 118 + row * 90);
    lv_obj_set_style_bg_color(tile, c_dji_tile(), 0);
    lv_obj_set_style_bg_opa(tile, 220, 0);
    lv_obj_set_style_border_width(tile, 0, 0);
    lv_obj_set_style_radius(tile, 8, 0);
    lv_obj_set_style_shadow_width(tile, 0, 0);
    lv_obj_set_style_pad_all(tile, 0, 0);

    lv_obj_t * title_label = label_create(tile, title, lv_color_white(), UI_CJK_FONT);
    lv_obj_align(title_label, LV_ALIGN_TOP_MID, 0, 12);

    lv_obj_t * value_label = label_create(tile, value, lv_color_white(), UI_CJK_FONT);
    lv_obj_align(value_label, LV_ALIGN_BOTTOM_MID, 0, -12);

    if (badge != NULL) {
        lv_obj_t * badge_obj = lv_obj_create(tile);
        lv_obj_set_size(badge_obj, 44, 28);
        lv_obj_align(badge_obj, LV_ALIGN_BOTTOM_RIGHT, -8, -8);
        lv_obj_set_style_bg_color(badge_obj, c_yellow(), 0);
        lv_obj_set_style_border_width(badge_obj, 0, 0);
        lv_obj_set_style_radius(badge_obj, 6, 0);
        lv_obj_set_style_pad_all(badge_obj, 0, 0);

        lv_obj_t * badge_label = label_create(badge_obj, badge, lv_color_black(), UI_TEXT_FONT);
        lv_obj_center(badge_label);
    }
}

static lv_obj_t *record_option_button_create(lv_obj_t * parent,
                                             const char * text,
                                             lv_event_cb_t event_cb,
                                             uintptr_t index)
{
    lv_obj_t * btn = lv_btn_create(parent);
    lv_obj_set_size(btn, 132, 50);
    lv_obj_set_style_bg_color(btn, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(btn, 132, 0);
    lv_obj_set_style_border_color(btn, lv_color_white(), 0);
    lv_obj_set_style_border_opa(btn, 112, 0);
    lv_obj_set_style_border_width(btn, 1, 0);
    lv_obj_set_style_radius(btn, 8, 0);
    lv_obj_set_style_shadow_width(btn, 0, 0);
    lv_obj_set_style_pad_all(btn, 0, 0);
    lv_obj_clear_flag(btn, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_SCROLL_CHAIN);
    lv_obj_add_event_cb(btn, event_cb, LV_EVENT_CLICKED, (void *)index);

    lv_obj_t * label = label_create(btn, text, lv_color_white(), UI_CJK_FONT);
    lv_obj_center(label);
    return btn;
}

static void create_record_settings_panel(lv_obj_t * parent)
{
    record_settings_panel = lv_obj_create(parent);
    lv_obj_set_size(record_settings_panel, UI_WIDTH, RECORD_SETTINGS_PANEL_H);
    lv_obj_set_pos(record_settings_panel, 0, UI_HEIGHT);
    lv_obj_set_style_bg_color(record_settings_panel, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(record_settings_panel, 196, 0);
    lv_obj_set_style_border_width(record_settings_panel, 0, 0);
    lv_obj_set_style_radius(record_settings_panel, 0, 0);
    lv_obj_set_style_pad_all(record_settings_panel, 0, 0);
    lv_obj_add_flag(record_settings_panel, LV_OBJ_FLAG_FLOATING);
    lv_obj_clear_flag(record_settings_panel, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_SCROLL_CHAIN);
    lv_obj_add_flag(record_settings_panel, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(record_settings_panel, record_settings_edge_cb, LV_EVENT_GESTURE, NULL);

    lv_obj_t * grab = lv_obj_create(record_settings_panel);
    lv_obj_set_size(grab, 86, 5);
    lv_obj_align(grab, LV_ALIGN_TOP_MID, 0, 12);
    lv_obj_set_style_bg_color(grab, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(grab, 160, 0);
    lv_obj_set_style_border_width(grab, 0, 0);
    lv_obj_set_style_radius(grab, 3, 0);
    lv_obj_clear_flag(grab, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_SCROLL_CHAIN);

    lv_obj_t * resolution_title =
        label_create(record_settings_panel, "像素规格", lv_color_white(), UI_CJK_FONT);
    lv_obj_align(resolution_title, LV_ALIGN_TOP_LEFT, 48, 36);

    lv_obj_t * fps_title =
        label_create(record_settings_panel, "帧率", lv_color_white(), UI_CJK_FONT);
    lv_obj_align(fps_title, LV_ALIGN_TOP_LEFT, 48, 116);

    for (size_t i = 0; i < sizeof(record_resolution_options) / sizeof(record_resolution_options[0]); i++) {
        record_resolution_btns[i] =
            record_option_button_create(record_settings_panel,
                                        record_resolution_options[i].label,
                                        record_resolution_event_cb,
                                        (uintptr_t)i);
        lv_obj_set_pos(record_resolution_btns[i], 174 + (int32_t)i * 150, 30);
    }

    for (size_t i = 0; i < sizeof(record_fps_options) / sizeof(record_fps_options[0]); i++) {
        char text[12];
        snprintf(text, sizeof(text), "%u帧", record_fps_options[i]);
        record_fps_btns[i] =
            record_option_button_create(record_settings_panel,
                                        text,
                                        record_fps_event_cb,
                                        (uintptr_t)i);
        lv_obj_set_pos(record_fps_btns[i], 174 + (int32_t)i * 150, 110);
    }

    record_settings_update_ui();
}

static void create_settings_panel(lv_obj_t * parent)
{
    settings_panel = lv_obj_create(parent);
    lv_obj_set_size(settings_panel, SETTINGS_PANEL_W, UI_HEIGHT);
    lv_obj_set_pos(settings_panel, UI_WIDTH, 0);
    lv_obj_set_style_bg_color(settings_panel, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(settings_panel, 178, 0);
    lv_obj_set_style_border_width(settings_panel, 0, 0);
    lv_obj_set_style_radius(settings_panel, 0, 0);
    lv_obj_set_style_pad_all(settings_panel, 0, 0);
    lv_obj_add_flag(settings_panel, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(settings_panel, settings_event_cb, LV_EVENT_GESTURE, NULL);

    lv_obj_t * exp_icon = top_menu_icon_create(settings_panel, LV_SYMBOL_SETTINGS, false);
    lv_obj_align(exp_icon, LV_ALIGN_TOP_LEFT, 42, 38);

    lv_obj_t * mic_icon = top_menu_icon_create(settings_panel, LV_SYMBOL_AUDIO, false);
    lv_obj_align(mic_icon, LV_ALIGN_TOP_LEFT, 126, 38);

    lv_obj_t * pro_icon = top_menu_icon_create(settings_panel, "PRO", true);
    lv_obj_align(pro_icon, LV_ALIGN_TOP_LEFT, 230, 38);

    add_settings_tile(settings_panel, "曝光", "Auto", NULL, 0, 0);
    add_settings_tile(settings_panel, "白平衡", "AWB", NULL, 1, 0);
    add_settings_tile(settings_panel, "美颜", "开", NULL, 0, 1);
    add_settings_tile(settings_panel, "色彩", "普通", "10bit", 1, 1);
    add_settings_tile(settings_panel, "补偿", "0.0", NULL, 0, 2);
    add_settings_tile(settings_panel, "图像调节", "标准", NULL, 1, 2);
}

void camera_page_create(uint32_t screen_w, uint32_t screen_h)
{
    camera_page_active = false;
    clear_camera_page_refs();
    quick_curtain = NULL;
    quick_curtain_dragging = false;
    record_settings_dragging = false;
    lv_obj_clean(lv_scr_act());
    lv_obj_clear_flag(lv_scr_act(), LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_SCROLL_CHAIN);
    global_quick_open_pending = false;
    global_quick_swipe_tracking = false;
    global_record_settings_swipe_tracking = false;

    lv_obj_t * main_bg = lv_obj_create(lv_scr_act());
    lv_obj_set_size(main_bg, screen_w, screen_h);
    lv_obj_align(main_bg, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(main_bg, lv_color_black(), 0);
    lv_obj_set_style_pad_all(main_bg, 0, 0);
    lv_obj_set_style_border_width(main_bg, 0, 0);
    lv_obj_set_style_radius(main_bg, 0, 0);
    lv_obj_set_style_clip_corner(main_bg, true, 0);
    lv_obj_clear_flag(main_bg, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_SCROLL_CHAIN);

    lv_obj_t * preview = lv_obj_create(main_bg);
    lv_obj_set_size(preview, UI_WIDTH, UI_HEIGHT);
    lv_obj_align(preview, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(preview, lv_color_make(48, 54, 58), 0);
    lv_obj_set_style_bg_grad_color(preview, lv_color_make(20, 25, 28), 0);
    lv_obj_set_style_bg_grad_dir(preview, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_pad_all(preview, 0, 0);
    lv_obj_set_style_border_width(preview, 0, 0);
    lv_obj_set_style_radius(preview, 0, 0);
    lv_obj_clear_flag(preview, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_SCROLL_CHAIN);
    lv_obj_add_flag(preview, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(preview, quick_settings_edge_cb, LV_EVENT_ALL, NULL);
    preview_container = preview;

    create_preview_image();

    lv_obj_t * top_left_scrim = lv_obj_create(preview);
    lv_obj_set_size(top_left_scrim, 328, 82);
    lv_obj_align(top_left_scrim, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_set_style_bg_color(top_left_scrim, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(top_left_scrim, 76, 0);
    lv_obj_set_style_border_width(top_left_scrim, 0, 0);
    lv_obj_set_style_radius(top_left_scrim, 0, 0);
    lv_obj_clear_flag(top_left_scrim, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t * sd_badge = lv_obj_create(preview);
    lv_obj_set_size(sd_badge, 42, 42);
    lv_obj_align(sd_badge, LV_ALIGN_TOP_LEFT, 34, 28);
    lv_obj_set_style_bg_color(sd_badge, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(sd_badge, 238, 0);
    lv_obj_set_style_border_width(sd_badge, 0, 0);
    lv_obj_set_style_radius(sd_badge, 6, 0);
    lv_obj_set_style_pad_all(sd_badge, 0, 0);

    lv_obj_t * sd_label = label_create(sd_badge, "SD", lv_color_make(80, 86, 92), UI_TEXT_FONT);
    lv_obj_center(sd_label);

    lv_obj_t * remain_label = label_create(preview, "1h56m", lv_color_white(), UI_CJK_FONT);
    lv_obj_align(remain_label, LV_ALIGN_TOP_LEFT, 88, 30);

    rec_dot = lv_obj_create(preview);
    lv_obj_set_size(rec_dot, 8, 8);
    lv_obj_align(rec_dot, LV_ALIGN_TOP_LEFT, 204, 47);
    lv_obj_set_style_bg_color(rec_dot, c_red(), 0);
    lv_obj_set_style_border_width(rec_dot, 0, 0);
    lv_obj_set_style_radius(rec_dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_add_flag(rec_dot, LV_OBJ_FLAG_HIDDEN);

    rec_time_label = label_create(preview, "00:00", lv_color_white(), UI_TEXT_FONT);
    lv_obj_align(rec_time_label, LV_ALIGN_TOP_LEFT, 220, 40);
    lv_obj_add_flag(rec_time_label, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t * face = main_icon_create(preview, LV_SYMBOL_EYE_OPEN, 42, 42);
    lv_obj_align(face, LV_ALIGN_TOP_RIGHT, -126, 30);

    lv_obj_t * battery = label_create(preview, LV_SYMBOL_BATTERY_3, lv_color_white(), UI_TEXT_FONT);
    lv_obj_align(battery, LV_ALIGN_TOP_RIGHT, -48, 42);

    lv_obj_t * edge_handle = lv_obj_create(preview);
    lv_obj_set_size(edge_handle, SETTINGS_EDGE_TOUCH_W, UI_HEIGHT);
    lv_obj_align(edge_handle, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_set_style_bg_opa(edge_handle, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(edge_handle, 0, 0);
    lv_obj_set_style_radius(edge_handle, 0, 0);
    lv_obj_clear_flag(edge_handle, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_SCROLL_CHAIN);
    lv_obj_add_flag(edge_handle, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(edge_handle, settings_event_cb, LV_EVENT_GESTURE, NULL);

    lv_obj_t * zoom = zoom_control_create(preview);
    lv_obj_align(zoom, LV_ALIGN_RIGHT_MID, -16, 46);

    mode_label = label_create(preview, LV_SYMBOL_VIDEO, lv_color_white(), &lv_font_montserrat_28);
    lv_obj_align(mode_label, LV_ALIGN_BOTTOM_LEFT, 48, -40);
    lv_obj_add_flag(mode_label, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_ext_click_area(mode_label, 24);
    lv_obj_add_event_cb(mode_label, mode_btn_event_cb, LV_EVENT_CLICKED, NULL);

    mode_detail_label = label_create(preview, "4K60", lv_color_white(), UI_CJK_FONT);
    lv_obj_align(mode_detail_label, LV_ALIGN_BOTTOM_MID, 0, -40);

    album_btn = lv_btn_create(preview);
    lv_obj_set_size(album_btn, 64, 64);
    lv_obj_align(album_btn, LV_ALIGN_BOTTOM_RIGHT, -40, -18);
    lv_obj_set_style_bg_color(album_btn, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(album_btn, 120, 0);
    lv_obj_set_style_border_color(album_btn, lv_color_white(), 0);
    lv_obj_set_style_border_width(album_btn, 2, 0);
    lv_obj_set_style_radius(album_btn, 8, 0);
    lv_obj_set_style_shadow_width(album_btn, 0, 0);
    lv_obj_add_event_cb(album_btn, album_btn_event_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t * album_label =
        label_create(album_btn, LV_SYMBOL_IMAGE, lv_color_white(), &lv_font_montserrat_28);
    lv_obj_center(album_label);
    lv_obj_add_flag(album_btn, LV_OBJ_FLAG_HIDDEN);

    shutter_btn = lv_obj_create(preview);
    lv_obj_set_size(shutter_btn, 86, 86);
    lv_obj_align(shutter_btn, LV_ALIGN_LEFT_MID, 12, 0);
    lv_obj_set_style_bg_color(shutter_btn, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(shutter_btn, 72, 0);
    lv_obj_set_style_border_color(shutter_btn, lv_color_white(), 0);
    lv_obj_set_style_border_opa(shutter_btn, 230, 0);
    lv_obj_set_style_border_width(shutter_btn, 4, 0);
    lv_obj_set_style_radius(shutter_btn, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_pad_all(shutter_btn, 0, 0);
    lv_obj_add_flag(shutter_btn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(shutter_btn, shutter_btn_event_cb, LV_EVENT_CLICKED, NULL);

    shutter_icon = lv_obj_create(shutter_btn);
    lv_obj_set_size(shutter_icon, 52, 52);
    lv_obj_center(shutter_icon);
    lv_obj_set_style_bg_color(shutter_icon, c_red(), 0);
    lv_obj_set_style_bg_opa(shutter_icon, 255, 0);
    lv_obj_set_style_border_width(shutter_icon, 0, 0);
    lv_obj_set_style_radius(shutter_icon, LV_RADIUS_CIRCLE, 0);
    lv_obj_clear_flag(shutter_icon, LV_OBJ_FLAG_CLICKABLE);

    create_settings_panel(preview);
    create_record_settings_panel(preview);

    lv_obj_t * top_edge_handle = lv_obj_create(preview);
    lv_obj_set_size(top_edge_handle, UI_WIDTH, QUICK_EDGE_TOUCH_H);
    lv_obj_align(top_edge_handle, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_bg_opa(top_edge_handle, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(top_edge_handle, 0, 0);
    lv_obj_set_style_radius(top_edge_handle, 0, 0);
    lv_obj_clear_flag(top_edge_handle, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_SCROLL_CHAIN);
    lv_obj_add_flag(top_edge_handle, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(top_edge_handle, quick_settings_edge_cb, LV_EVENT_ALL, NULL);
    lv_obj_move_foreground(top_edge_handle);

    lv_obj_t * bottom_edge_handle = lv_obj_create(preview);
    lv_obj_set_size(bottom_edge_handle, UI_WIDTH, RECORD_SETTINGS_EDGE_TOUCH_H);
    lv_obj_align(bottom_edge_handle, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_opa(bottom_edge_handle, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(bottom_edge_handle, 0, 0);
    lv_obj_set_style_radius(bottom_edge_handle, 0, 0);
    lv_obj_clear_flag(bottom_edge_handle, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_SCROLL_CHAIN);
    lv_obj_add_flag(bottom_edge_handle, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(bottom_edge_handle, record_settings_edge_cb, LV_EVENT_ALL, NULL);
    lv_obj_move_foreground(bottom_edge_handle);

    if (rec_timer == NULL) {
        quick_settings_swipe_self_test();
        rec_timer = lv_timer_create(rec_timer_cb, 1000, NULL);
    }
    update_mode_ui();
    camera_page_active = true;
}
