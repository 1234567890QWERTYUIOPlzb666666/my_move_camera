#ifndef CAMERA_RECORDER_H
#define CAMERA_RECORDER_H

#include <stdbool.h>
#include <stddef.h>

int camera_recorder_start(const char * video_device, const char * path);
void camera_recorder_stop(void);
bool camera_recorder_is_recording(void);
int camera_recorder_play(const char * path);
int camera_recorder_play_blocking(const char * path);

#endif
