#ifndef CAMERA_RECORDER_H
#define CAMERA_RECORDER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

int camera_recorder_start(const char * video_device,
                          const char * path,
                          uint32_t width,
                          uint32_t height,
                          uint32_t fps);
void camera_recorder_stop(void);
bool camera_recorder_is_recording(void);
int camera_recorder_play(const char * path);
int camera_recorder_play_blocking(const char * path);

#endif
