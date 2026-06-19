#include "camera_capture.h"

#ifdef RK3566

#include <errno.h>
#include <fcntl.h>
#include <linux/videodev2.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/select.h>
#include <unistd.h>

#define CAPTURE_BUFFER_COUNT 4
#define CAPTURE_WIDTH 1280
#define CAPTURE_HEIGHT 720

typedef struct {
    void * address[VIDEO_MAX_PLANES];
    size_t length[VIDEO_MAX_PLANES];
    unsigned int plane_count;
} mapped_buffer_t;

static int camera_fd = -1;
static char current_device_path[64];
static enum v4l2_buf_type capture_type;
static uint32_t capture_format;
static uint32_t capture_width;
static uint32_t capture_height;
static uint32_t capture_stride;
static uint32_t capture_uv_stride;
static mapped_buffer_t mapped_buffers[CAPTURE_BUFFER_COUNT];
static unsigned int mapped_buffer_count;
static pthread_t capture_thread;
static bool capture_thread_started;
static volatile bool capture_running;
static lv_color_t * converted_frame;
static lv_color_t * published_frame;
static uint32_t output_width;
static uint32_t output_height;
static uint64_t published_sequence;
static uint64_t copied_sequence;
static pthread_mutex_t frame_mutex = PTHREAD_MUTEX_INITIALIZER;

static int xioctl(int fd, unsigned long request, void * arg)
{
    int result;

    do {
        result = ioctl(fd, request, arg);
    } while (result == -1 && errno == EINTR);

    return result;
}

static int clamp_color(int value)
{
    if (value < 0) return 0;
    if (value > 255) return 255;
    return value;
}

static lv_color_t yuv_to_color(int y, int u, int v)
{
    int c = y - 16;
    int d = u - 128;
    int e = v - 128;
    int r;
    int g;
    int b;

    if (c < 0) c = 0;
    r = (298 * c + 409 * e + 128) >> 8;
    g = (298 * c - 100 * d - 208 * e + 128) >> 8;
    b = (298 * c + 516 * d + 128) >> 8;

    return lv_color_make(clamp_color(r), clamp_color(g), clamp_color(b));
}

static void calculate_source_crop(uint32_t * crop_x,
                                  uint32_t * crop_y,
                                  uint32_t * crop_width,
                                  uint32_t * crop_height)
{
    uint64_t source_ratio = (uint64_t)capture_width * output_height;
    uint64_t output_ratio = (uint64_t)output_width * capture_height;

    *crop_x = 0;
    *crop_y = 0;
    *crop_width = capture_width;
    *crop_height = capture_height;

    if (source_ratio > output_ratio) {
        *crop_width = (uint32_t)((uint64_t)capture_height * output_width / output_height);
        *crop_x = (capture_width - *crop_width) / 2;
    } else if (source_ratio < output_ratio) {
        *crop_height = (uint32_t)((uint64_t)capture_width * output_height / output_width);
        *crop_y = (capture_height - *crop_height) / 2;
    }

    *crop_x &= ~1U;
    *crop_y &= ~1U;
    *crop_width &= ~1U;
    *crop_height &= ~1U;
}

static void convert_nv12(const uint8_t * y_plane,
                         const uint8_t * uv_plane,
                         uint32_t y_stride,
                         uint32_t uv_stride,
                         bool nv21)
{
    uint32_t crop_x;
    uint32_t crop_y;
    uint32_t crop_width;
    uint32_t crop_height;

    calculate_source_crop(&crop_x, &crop_y, &crop_width, &crop_height);

    for (uint32_t dy = 0; dy < output_height; dy++) {
        uint32_t sy = crop_y + (uint64_t)dy * crop_height / output_height;
        const uint8_t * y_row = y_plane + sy * y_stride;
        const uint8_t * uv_row = uv_plane + (sy / 2) * uv_stride;

        for (uint32_t dx = 0; dx < output_width; dx++) {
            uint32_t sx = crop_x + (uint64_t)dx * crop_width / output_width;
            uint32_t uv_x = sx & ~1U;
            int first = uv_row[uv_x];
            int second = uv_row[uv_x + 1];
            int u = nv21 ? second : first;
            int v = nv21 ? first : second;
            converted_frame[dy * output_width + dx] = yuv_to_color(y_row[sx], u, v);
        }
    }
}

static void convert_yuyv(const uint8_t * source, uint32_t stride, bool uyvy)
{
    uint32_t crop_x;
    uint32_t crop_y;
    uint32_t crop_width;
    uint32_t crop_height;

    calculate_source_crop(&crop_x, &crop_y, &crop_width, &crop_height);

    for (uint32_t dy = 0; dy < output_height; dy++) {
        uint32_t sy = crop_y + (uint64_t)dy * crop_height / output_height;
        const uint8_t * row = source + sy * stride;

        for (uint32_t dx = 0; dx < output_width; dx++) {
            uint32_t sx = crop_x + (uint64_t)dx * crop_width / output_width;
            const uint8_t * pair = row + (sx & ~1U) * 2;
            int y = pair[uyvy ? (sx & 1U ? 3 : 1) : (sx & 1U ? 2 : 0)];
            int u = pair[uyvy ? 0 : 1];
            int v = pair[uyvy ? 2 : 3];
            converted_frame[dy * output_width + dx] = yuv_to_color(y, u, v);
        }
    }
}

static void convert_buffer(const mapped_buffer_t * buffer)
{
    const uint8_t * plane0 = buffer->address[0];

    if (capture_format == V4L2_PIX_FMT_NV12 ||
        capture_format == V4L2_PIX_FMT_NV21 ||
        capture_format == V4L2_PIX_FMT_NV12M ||
        capture_format == V4L2_PIX_FMT_NV21M) {
        const uint8_t * uv_plane = buffer->plane_count > 1
                                      ? buffer->address[1]
                                      : plane0 + capture_stride * capture_height;
        bool nv21 = capture_format == V4L2_PIX_FMT_NV21 ||
                    capture_format == V4L2_PIX_FMT_NV21M;
        convert_nv12(plane0, uv_plane, capture_stride, capture_uv_stride, nv21);
    } else if (capture_format == V4L2_PIX_FMT_YUYV ||
               capture_format == V4L2_PIX_FMT_UYVY) {
        convert_yuyv(plane0, capture_stride, capture_format == V4L2_PIX_FMT_UYVY);
    }
}

static void publish_converted_frame(void)
{
    size_t frame_size = (size_t)output_width * output_height * sizeof(lv_color_t);

    pthread_mutex_lock(&frame_mutex);
    memcpy(published_frame, converted_frame, frame_size);
    published_sequence++;
    pthread_mutex_unlock(&frame_mutex);
}

static int queue_buffer(unsigned int index)
{
    struct v4l2_buffer buffer;
    struct v4l2_plane planes[VIDEO_MAX_PLANES];

    memset(&buffer, 0, sizeof(buffer));
    memset(planes, 0, sizeof(planes));
    buffer.type = capture_type;
    buffer.memory = V4L2_MEMORY_MMAP;
    buffer.index = index;
    if (capture_type == V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE) {
        buffer.m.planes = planes;
        buffer.length = mapped_buffers[index].plane_count;
    }

    return xioctl(camera_fd, VIDIOC_QBUF, &buffer);
}

static void * capture_thread_main(void * data)
{
    (void)data;

    while (capture_running) {
        fd_set fds;
        struct timeval timeout;
        struct v4l2_buffer buffer;
        struct v4l2_plane planes[VIDEO_MAX_PLANES];
        int ready;

        FD_ZERO(&fds);
        FD_SET(camera_fd, &fds);
        timeout.tv_sec = 1;
        timeout.tv_usec = 0;
        ready = select(camera_fd + 1, &fds, NULL, NULL, &timeout);
        if (ready == -1) {
            if (errno == EINTR) continue;
            perror("Camera select");
            break;
        }
        if (ready == 0) {
            fprintf(stderr, "Warning: camera frame timeout\n");
            continue;
        }

        memset(&buffer, 0, sizeof(buffer));
        memset(planes, 0, sizeof(planes));
        buffer.type = capture_type;
        buffer.memory = V4L2_MEMORY_MMAP;
        if (capture_type == V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE) {
            buffer.m.planes = planes;
            buffer.length = VIDEO_MAX_PLANES;
        }

        if (xioctl(camera_fd, VIDIOC_DQBUF, &buffer) == -1) {
            if (errno == EAGAIN) continue;
            perror("VIDIOC_DQBUF");
            break;
        }

        if (buffer.index < mapped_buffer_count) {
            convert_buffer(&mapped_buffers[buffer.index]);
            publish_converted_frame();
        }

        if (queue_buffer(buffer.index) == -1) {
            perror("VIDIOC_QBUF");
            break;
        }
    }

    capture_running = false;
    return NULL;
}

static bool device_is_capture_node(const char * path, struct v4l2_capability * capability)
{
    uint32_t caps;
    int fd = open(path, O_RDWR | O_NONBLOCK);

    if (fd < 0) return false;
    if (xioctl(fd, VIDIOC_QUERYCAP, capability) == -1) {
        close(fd);
        return false;
    }

    caps = capability->capabilities & V4L2_CAP_DEVICE_CAPS
               ? capability->device_caps
               : capability->capabilities;
    close(fd);
    return (caps & V4L2_CAP_STREAMING) &&
           (caps & (V4L2_CAP_VIDEO_CAPTURE | V4L2_CAP_VIDEO_CAPTURE_MPLANE));
}

static int open_camera_device(char * selected_path, size_t selected_path_size)
{
    const char * requested_path = getenv("MY_MOVE_CAMERA_VIDEO");
    char fallback_path[32] = "";
    char rkisp_path[32] = "";

    if (requested_path != NULL && requested_path[0] != '\0') {
        snprintf(selected_path, selected_path_size, "%s", requested_path);
        return open(selected_path, O_RDWR | O_NONBLOCK);
    }

    for (int index = 0; index < 64; index++) {
        char path[32];
        struct v4l2_capability capability;

        snprintf(path, sizeof(path), "/dev/video%d", index);
        memset(&capability, 0, sizeof(capability));
        if (!device_is_capture_node(path, &capability)) continue;

        if (fallback_path[0] == '\0') {
            snprintf(fallback_path, sizeof(fallback_path), "%s", path);
        }
        if (strstr((const char *)capability.card, "mainpath") != NULL) {
            snprintf(selected_path, selected_path_size, "%s", path);
            return open(selected_path, O_RDWR | O_NONBLOCK);
        }
        if (rkisp_path[0] == '\0' &&
            (strstr((const char *)capability.card, "rkisp") != NULL ||
             strstr((const char *)capability.card, "rkispp") != NULL)) {
            snprintf(rkisp_path, sizeof(rkisp_path), "%s", path);
        }
    }

    if (rkisp_path[0] != '\0') {
        snprintf(selected_path, selected_path_size, "%s", rkisp_path);
        return open(selected_path, O_RDWR | O_NONBLOCK);
    }

    if (fallback_path[0] != '\0') {
        snprintf(selected_path, selected_path_size, "%s", fallback_path);
        return open(selected_path, O_RDWR | O_NONBLOCK);
    }

    errno = ENODEV;
    return -1;
}

bool camera_capture_find_alternate_device(const char * avoid_path,
                                          char * selected_path,
                                          size_t selected_path_size)
{
    char fallback_path[32] = "";

    if (selected_path == NULL || selected_path_size == 0) {
        return false;
    }

    selected_path[0] = '\0';
    for (int index = 0; index < 64; index++) {
        char path[32];
        struct v4l2_capability capability;
        bool same_as_preview;

        snprintf(path, sizeof(path), "/dev/video%d", index);
        same_as_preview = avoid_path != NULL && strcmp(path, avoid_path) == 0;
        if (same_as_preview) continue;

        memset(&capability, 0, sizeof(capability));
        if (!device_is_capture_node(path, &capability)) continue;

        if (strstr((const char *)capability.card, "selfpath") != NULL ||
            strstr((const char *)capability.card, "rkisp") != NULL ||
            strstr((const char *)capability.card, "rkispp") != NULL) {
            snprintf(selected_path, selected_path_size, "%s", path);
            return true;
        }

        if (fallback_path[0] == '\0') {
            snprintf(fallback_path, sizeof(fallback_path), "%s", path);
        }
    }

    if (fallback_path[0] != '\0') {
        snprintf(selected_path, selected_path_size, "%s", fallback_path);
        return true;
    }

    return false;
}

static int configure_capture_format(void)
{
    struct v4l2_capability capability;
    uint32_t caps;
    const uint32_t formats[] = {
        V4L2_PIX_FMT_NV12,
        V4L2_PIX_FMT_NV12M,
        V4L2_PIX_FMT_NV21,
        V4L2_PIX_FMT_YUYV,
        V4L2_PIX_FMT_UYVY
    };

    memset(&capability, 0, sizeof(capability));
    if (xioctl(camera_fd, VIDIOC_QUERYCAP, &capability) == -1) {
        perror("VIDIOC_QUERYCAP");
        return -1;
    }

    caps = capability.capabilities & V4L2_CAP_DEVICE_CAPS
               ? capability.device_caps
               : capability.capabilities;
    capture_type = caps & V4L2_CAP_VIDEO_CAPTURE_MPLANE
                       ? V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE
                       : V4L2_BUF_TYPE_VIDEO_CAPTURE;

    for (size_t index = 0; index < sizeof(formats) / sizeof(formats[0]); index++) {
        struct v4l2_format format;

        memset(&format, 0, sizeof(format));
        format.type = capture_type;
        if (capture_type == V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE) {
            format.fmt.pix_mp.width = CAPTURE_WIDTH;
            format.fmt.pix_mp.height = CAPTURE_HEIGHT;
            format.fmt.pix_mp.pixelformat = formats[index];
            format.fmt.pix_mp.field = V4L2_FIELD_ANY;
        } else {
            format.fmt.pix.width = CAPTURE_WIDTH;
            format.fmt.pix.height = CAPTURE_HEIGHT;
            format.fmt.pix.pixelformat = formats[index];
            format.fmt.pix.field = V4L2_FIELD_ANY;
        }

        if (xioctl(camera_fd, VIDIOC_S_FMT, &format) == -1) continue;

        if (capture_type == V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE) {
            capture_format = format.fmt.pix_mp.pixelformat;
            capture_width = format.fmt.pix_mp.width;
            capture_height = format.fmt.pix_mp.height;
            capture_stride = format.fmt.pix_mp.plane_fmt[0].bytesperline;
            capture_uv_stride = format.fmt.pix_mp.num_planes > 1
                                    ? format.fmt.pix_mp.plane_fmt[1].bytesperline
                                    : capture_stride;
        } else {
            capture_format = format.fmt.pix.pixelformat;
            capture_width = format.fmt.pix.width;
            capture_height = format.fmt.pix.height;
            capture_stride = format.fmt.pix.bytesperline;
            capture_uv_stride = capture_stride;
        }

        if (capture_format == formats[index] &&
            capture_width == CAPTURE_WIDTH &&
            capture_height == CAPTURE_HEIGHT) {
            if (capture_stride == 0) {
                capture_stride = capture_width *
                    ((capture_format == V4L2_PIX_FMT_YUYV ||
                      capture_format == V4L2_PIX_FMT_UYVY) ? 2 : 1);
            }
            if (capture_uv_stride == 0) {
                capture_uv_stride = capture_stride;
            }
            return 0;
        }
    }

    fprintf(stderr,
            "Camera does not provide %ux%u NV12/NV21/YUYV/UYVY output\n",
            CAPTURE_WIDTH, CAPTURE_HEIGHT);
    return -1;
}

static int map_capture_buffers(void)
{
    struct v4l2_requestbuffers request;

    memset(&request, 0, sizeof(request));
    request.count = CAPTURE_BUFFER_COUNT;
    request.type = capture_type;
    request.memory = V4L2_MEMORY_MMAP;
    if (xioctl(camera_fd, VIDIOC_REQBUFS, &request) == -1 || request.count < 2) {
        perror("VIDIOC_REQBUFS");
        return -1;
    }

    mapped_buffer_count = request.count > CAPTURE_BUFFER_COUNT
                              ? CAPTURE_BUFFER_COUNT
                              : request.count;

    for (unsigned int index = 0; index < mapped_buffer_count; index++) {
        struct v4l2_buffer buffer;
        struct v4l2_plane planes[VIDEO_MAX_PLANES];

        memset(&buffer, 0, sizeof(buffer));
        memset(planes, 0, sizeof(planes));
        buffer.type = capture_type;
        buffer.memory = V4L2_MEMORY_MMAP;
        buffer.index = index;
        if (capture_type == V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE) {
            buffer.m.planes = planes;
            buffer.length = VIDEO_MAX_PLANES;
        }

        if (xioctl(camera_fd, VIDIOC_QUERYBUF, &buffer) == -1) {
            perror("VIDIOC_QUERYBUF");
            return -1;
        }

        mapped_buffers[index].plane_count =
            capture_type == V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE ? buffer.length : 1;
        for (unsigned int plane = 0; plane < mapped_buffers[index].plane_count; plane++) {
            size_t length = capture_type == V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE
                                ? planes[plane].length
                                : buffer.length;
            off_t offset = capture_type == V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE
                               ? planes[plane].m.mem_offset
                               : buffer.m.offset;
            void * address = mmap(NULL, length, PROT_READ | PROT_WRITE,
                                  MAP_SHARED, camera_fd, offset);
            if (address == MAP_FAILED) {
                perror("Camera mmap");
                return -1;
            }
            mapped_buffers[index].address[plane] = address;
            mapped_buffers[index].length[plane] = length;
        }
    }

    for (unsigned int index = 0; index < mapped_buffer_count; index++) {
        if (queue_buffer(index) == -1) {
            perror("VIDIOC_QBUF");
            return -1;
        }
    }

    return 0;
}

int camera_capture_start(uint32_t requested_output_width, uint32_t requested_output_height)
{
    char device_path[64];
    enum v4l2_buf_type type;
    size_t pixel_count;
    uint32_t crop_x;
    uint32_t crop_y;
    uint32_t crop_width;
    uint32_t crop_height;

    if (requested_output_width == 0 || requested_output_height == 0) {
        errno = EINVAL;
        return -1;
    }

    output_width = requested_output_width;
    output_height = requested_output_height;
    pixel_count = (size_t)output_width * output_height;
    converted_frame = calloc(pixel_count, sizeof(lv_color_t));
    published_frame = calloc(pixel_count, sizeof(lv_color_t));
    if (converted_frame == NULL || published_frame == NULL) {
        fprintf(stderr, "Failed to allocate camera preview buffers\n");
        camera_capture_stop();
        return -1;
    }

    camera_fd = open_camera_device(device_path, sizeof(device_path));
    if (camera_fd < 0) {
        perror("Unable to open a V4L2 camera capture node");
        camera_capture_stop();
        return -1;
    }
    snprintf(current_device_path, sizeof(current_device_path), "%s", device_path);
    if (configure_capture_format() != 0 || map_capture_buffers() != 0) {
        camera_capture_stop();
        return -1;
    }

    type = capture_type;
    if (xioctl(camera_fd, VIDIOC_STREAMON, &type) == -1) {
        perror("VIDIOC_STREAMON");
        camera_capture_stop();
        return -1;
    }

    capture_running = true;
    if (pthread_create(&capture_thread, NULL, capture_thread_main, NULL) != 0) {
        perror("Failed to create camera capture thread");
        capture_running = false;
        camera_capture_stop();
        return -1;
    }
    capture_thread_started = true;

    calculate_source_crop(&crop_x, &crop_y, &crop_width, &crop_height);
    printf("Camera preview initialized: %s, capture %ux%u %.4s, "
           "crop (%u,%u) %ux%u -> display %ux%u\n",
           device_path, capture_width, capture_height, (char *)&capture_format,
           crop_x, crop_y, crop_width, crop_height, output_width, output_height);
    return 0;
}

bool camera_capture_copy_latest(lv_color_t * destination, size_t pixel_count)
{
    bool has_new_frame = false;
    size_t expected_pixels = (size_t)output_width * output_height;

    if (published_frame == NULL || pixel_count < expected_pixels) return false;

    pthread_mutex_lock(&frame_mutex);
    if (published_sequence != copied_sequence) {
        memcpy(destination, published_frame, expected_pixels * sizeof(lv_color_t));
        copied_sequence = published_sequence;
        has_new_frame = true;
    }
    pthread_mutex_unlock(&frame_mutex);
    return has_new_frame;
}

const char *camera_capture_get_device_path(void)
{
    return current_device_path[0] == '\0' ? NULL : current_device_path;
}

void camera_capture_stop(void)
{
    capture_running = false;
    if (capture_thread_started) {
        pthread_join(capture_thread, NULL);
        capture_thread_started = false;
    }

    if (camera_fd >= 0) {
        enum v4l2_buf_type type = capture_type;
        xioctl(camera_fd, VIDIOC_STREAMOFF, &type);
    }

    for (unsigned int index = 0; index < mapped_buffer_count; index++) {
        for (unsigned int plane = 0; plane < mapped_buffers[index].plane_count; plane++) {
            if (mapped_buffers[index].address[plane] != NULL &&
                mapped_buffers[index].address[plane] != MAP_FAILED) {
                munmap(mapped_buffers[index].address[plane],
                       mapped_buffers[index].length[plane]);
            }
        }
        memset(&mapped_buffers[index], 0, sizeof(mapped_buffers[index]));
    }
    mapped_buffer_count = 0;

    if (camera_fd >= 0) {
        close(camera_fd);
        camera_fd = -1;
    }

    free(converted_frame);
    free(published_frame);
    converted_frame = NULL;
    published_frame = NULL;
    published_sequence = 0;
    copied_sequence = 0;
    current_device_path[0] = '\0';
}

#else

int camera_capture_start(uint32_t output_width, uint32_t output_height)
{
    (void)output_width;
    (void)output_height;
    return -1;
}

bool camera_capture_copy_latest(lv_color_t * destination, size_t pixel_count)
{
    (void)destination;
    (void)pixel_count;
    return false;
}

const char *camera_capture_get_device_path(void)
{
    return NULL;
}

bool camera_capture_find_alternate_device(const char * avoid_path,
                                          char * selected_path,
                                          size_t selected_path_size)
{
    (void)avoid_path;
    if (selected_path != NULL && selected_path_size > 0) {
        selected_path[0] = '\0';
    }
    return false;
}

void camera_capture_stop(void)
{
}

#endif
