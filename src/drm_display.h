#ifndef DRM_DISPLAY_H
#define DRM_DISPLAY_H

#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint8_t * pixels;
    uint32_t width;
    uint32_t height;
    uint32_t pitch;
    size_t size;
} drm_display_buffer_t;

int drm_display_init(drm_display_buffer_t * buffer);
void drm_display_deinit(void);

#endif
