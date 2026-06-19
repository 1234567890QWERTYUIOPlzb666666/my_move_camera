#include "drm_display.h"

#ifdef RK3566

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include <drm_fourcc.h>
#include <xf86drm.h>
#include <xf86drmMode.h>

static int drm_fd = -1;
static uint32_t connector_id;
static uint32_t crtc_id;
static uint32_t framebuffer_id;
static uint32_t dumb_handle;
static uint8_t * mapped_pixels;
static size_t mapped_size;
static drmModeCrtcPtr previous_crtc;

static int find_crtc(drmModeResPtr resources,
                     drmModeConnectorPtr connector,
                     uint32_t * selected_crtc)
{
    drmModeEncoderPtr encoder = NULL;

    if (connector->encoder_id != 0) {
        encoder = drmModeGetEncoder(drm_fd, connector->encoder_id);
        if (encoder != NULL && encoder->crtc_id != 0) {
            *selected_crtc = encoder->crtc_id;
            drmModeFreeEncoder(encoder);
            return 0;
        }
        drmModeFreeEncoder(encoder);
    }

    for (int encoder_index = 0; encoder_index < connector->count_encoders; encoder_index++) {
        encoder = drmModeGetEncoder(drm_fd, connector->encoders[encoder_index]);
        if (encoder == NULL) continue;

        for (int crtc_index = 0; crtc_index < resources->count_crtcs; crtc_index++) {
            if (encoder->possible_crtcs & (1U << crtc_index)) {
                *selected_crtc = resources->crtcs[crtc_index];
                drmModeFreeEncoder(encoder);
                return 0;
            }
        }
        drmModeFreeEncoder(encoder);
    }

    return -1;
}

static drmModeConnectorPtr find_connector(drmModeResPtr resources,
                                          drmModeModeInfoPtr * selected_mode)
{
    drmModeConnectorPtr fallback = NULL;

    for (int index = 0; index < resources->count_connectors; index++) {
        drmModeConnectorPtr connector =
            drmModeGetConnector(drm_fd, resources->connectors[index]);
        if (connector == NULL || connector->count_modes == 0) {
            drmModeFreeConnector(connector);
            continue;
        }

        if (fallback == NULL) {
            fallback = connector;
        } else if (connector->connection != DRM_MODE_CONNECTED) {
            drmModeFreeConnector(connector);
            continue;
        }

        if (connector->connection == DRM_MODE_CONNECTED) {
            if (fallback != connector) drmModeFreeConnector(fallback);
            fallback = connector;
            break;
        }
    }

    if (fallback == NULL) return NULL;

    *selected_mode = &fallback->modes[0];
    for (int index = 0; index < fallback->count_modes; index++) {
        if (fallback->modes[index].type & DRM_MODE_TYPE_PREFERRED) {
            *selected_mode = &fallback->modes[index];
            break;
        }
    }
    return fallback;
}

int drm_display_init(drm_display_buffer_t * buffer)
{
    const char * device_path = getenv("MY_MOVE_CAMERA_DRM");
    drmModeResPtr resources = NULL;
    drmModeConnectorPtr connector = NULL;
    drmModeModeInfoPtr mode = NULL;
    struct drm_mode_create_dumb create_request;
    struct drm_mode_map_dumb map_request;
    uint32_t handles[4] = {0};
    uint32_t pitches[4] = {0};
    uint32_t offsets[4] = {0};
    int result = -1;

    if (buffer == NULL) return -1;
    memset(buffer, 0, sizeof(*buffer));
    if (device_path == NULL || device_path[0] == '\0') {
        device_path = "/dev/dri/card0";
    }

    drm_fd = open(device_path, O_RDWR | O_CLOEXEC);
    if (drm_fd < 0) {
        perror("Unable to open DRM device");
        goto cleanup;
    }

    resources = drmModeGetResources(drm_fd);
    if (resources == NULL) {
        perror("drmModeGetResources");
        goto cleanup;
    }

    connector = find_connector(resources, &mode);
    if (connector == NULL || find_crtc(resources, connector, &crtc_id) != 0) {
        fprintf(stderr, "Unable to find an active DRM connector and CRTC\n");
        goto cleanup;
    }
    connector_id = connector->connector_id;
    previous_crtc = drmModeGetCrtc(drm_fd, crtc_id);

    memset(&create_request, 0, sizeof(create_request));
    create_request.width = mode->hdisplay;
    create_request.height = mode->vdisplay;
    create_request.bpp = 32;
    if (drmIoctl(drm_fd, DRM_IOCTL_MODE_CREATE_DUMB, &create_request) != 0) {
        perror("DRM_IOCTL_MODE_CREATE_DUMB");
        goto cleanup;
    }
    dumb_handle = create_request.handle;
    mapped_size = create_request.size;

    handles[0] = dumb_handle;
    pitches[0] = create_request.pitch;
    if (drmModeAddFB2(drm_fd, mode->hdisplay, mode->vdisplay,
                      DRM_FORMAT_XRGB8888, handles, pitches, offsets,
                      &framebuffer_id, 0) != 0) {
        perror("drmModeAddFB2");
        goto cleanup;
    }

    memset(&map_request, 0, sizeof(map_request));
    map_request.handle = dumb_handle;
    if (drmIoctl(drm_fd, DRM_IOCTL_MODE_MAP_DUMB, &map_request) != 0) {
        perror("DRM_IOCTL_MODE_MAP_DUMB");
        goto cleanup;
    }

    mapped_pixels = mmap(NULL, mapped_size, PROT_READ | PROT_WRITE,
                         MAP_SHARED, drm_fd, map_request.offset);
    if (mapped_pixels == MAP_FAILED) {
        mapped_pixels = NULL;
        perror("Unable to mmap DRM dumb buffer");
        goto cleanup;
    }
    memset(mapped_pixels, 0, mapped_size);

    if (drmModeSetCrtc(drm_fd, crtc_id, framebuffer_id, 0, 0,
                       &connector_id, 1, mode) != 0) {
        perror("drmModeSetCrtc");
        goto cleanup;
    }

    buffer->pixels = mapped_pixels;
    buffer->width = mode->hdisplay;
    buffer->height = mode->vdisplay;
    buffer->pitch = create_request.pitch;
    buffer->size = mapped_size;
    printf("RK3566 DRM display initialized: %s, %ux%u, pitch %u\n",
           device_path, buffer->width, buffer->height, buffer->pitch);
    result = 0;

cleanup:
    drmModeFreeConnector(connector);
    drmModeFreeResources(resources);
    if (result != 0) drm_display_deinit();
    return result;
}

void drm_display_deinit(void)
{
    if (drm_fd >= 0 && previous_crtc != NULL) {
        drmModeSetCrtc(drm_fd,
                       previous_crtc->crtc_id,
                       previous_crtc->buffer_id,
                       previous_crtc->x,
                       previous_crtc->y,
                       &connector_id,
                       1,
                       &previous_crtc->mode);
    }
    drmModeFreeCrtc(previous_crtc);
    previous_crtc = NULL;

    if (mapped_pixels != NULL) {
        munmap(mapped_pixels, mapped_size);
        mapped_pixels = NULL;
    }
    if (drm_fd >= 0 && framebuffer_id != 0) {
        drmModeRmFB(drm_fd, framebuffer_id);
        framebuffer_id = 0;
    }
    if (drm_fd >= 0 && dumb_handle != 0) {
        struct drm_mode_destroy_dumb destroy_request;
        memset(&destroy_request, 0, sizeof(destroy_request));
        destroy_request.handle = dumb_handle;
        drmIoctl(drm_fd, DRM_IOCTL_MODE_DESTROY_DUMB, &destroy_request);
        dumb_handle = 0;
    }
    if (drm_fd >= 0) {
        close(drm_fd);
        drm_fd = -1;
    }
    mapped_size = 0;
    connector_id = 0;
    crtc_id = 0;
}

#else

int drm_display_init(drm_display_buffer_t * buffer)
{
    (void)buffer;
    return -1;
}

void drm_display_deinit(void)
{
}

#endif
