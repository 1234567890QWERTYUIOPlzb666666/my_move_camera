#include "camera_recorder.h"

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

static pid_t recorder_pid = -1;

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

static void replace_tokens(char * out,
                           size_t out_size,
                           const char * template,
                           const char * device,
                           const char * path,
                           uint32_t width,
                           uint32_t height,
                           uint32_t fps)
{
    char width_text[16];
    char height_text[16];
    char fps_text[16];

    snprintf(width_text, sizeof(width_text), "%u", width);
    snprintf(height_text, sizeof(height_text), "%u", height);
    snprintf(fps_text, sizeof(fps_text), "%u", fps);

    out[0] = '\0';

    for (const char * p = template; *p != '\0' && strlen(out) + 1 < out_size;) {
        if (strncmp(p, "{device}", 8) == 0) {
            shell_quote_append(out, out_size, device);
            p += 8;
        } else if (strncmp(p, "{path}", 6) == 0) {
            shell_quote_append(out, out_size, path);
            p += 6;
        } else if (strncmp(p, "{width}", 7) == 0) {
            strncat(out, width_text, out_size - strlen(out) - 1);
            p += 7;
        } else if (strncmp(p, "{height}", 8) == 0) {
            strncat(out, height_text, out_size - strlen(out) - 1);
            p += 8;
        } else if (strncmp(p, "{fps}", 5) == 0) {
            strncat(out, fps_text, out_size - strlen(out) - 1);
            p += 5;
        } else {
            size_t used = strlen(out);
            out[used] = *p++;
            out[used + 1] = '\0';
        }
    }
}

static pid_t spawn_shell_command(const char * command)
{
    pid_t pid = fork();
    if (pid < 0) {
        perror("fork");
        return -1;
    }

    if (pid == 0) {
        setpgid(0, 0);
        execlp("sh", "sh", "-c", command, (char *)NULL);
        _exit(127);
    }

    setpgid(pid, pid);
    return pid;
}

static int wait_for_process(pid_t pid)
{
    int status;

    if (pid <= 0) return -1;
    while (waitpid(pid, &status, 0) == -1) {
        if (errno != EINTR) return -1;
    }

    if (WIFEXITED(status)) return WEXITSTATUS(status);
    if (WIFSIGNALED(status)) return 128 + WTERMSIG(status);
    return -1;
}

static bool process_is_running(pid_t pid)
{
    if (pid <= 0) return false;
    if (kill(pid, 0) == 0) return true;
    return errno != ESRCH;
}

bool camera_recorder_is_recording(void)
{
    return process_is_running(recorder_pid);
}

int camera_recorder_start(const char * video_device,
                          const char * path,
                          uint32_t width,
                          uint32_t height,
                          uint32_t fps)
{
    const char * pipeline = getenv("MY_MOVE_CAMERA_RECORD_PIPELINE");
    char command[1024];

    if (camera_recorder_is_recording()) {
        errno = EBUSY;
        return -1;
    }

    if (video_device == NULL || video_device[0] == '\0') {
        video_device = "/dev/video0";
    }
    if (path == NULL || path[0] == '\0') {
        errno = EINVAL;
        return -1;
    }
    if (width == 0) width = 1280;
    if (height == 0) height = 720;
    if (fps == 0) fps = 30;

    if (pipeline == NULL || pipeline[0] == '\0') {
        pipeline =
            "gst-launch-1.0 -e "
            "v4l2src device={device} ! "
            "video/x-raw,format=NV12,width={width},height={height},framerate={fps}/1 ! "
            "mpph264enc ! h264parse ! mp4mux faststart=true ! filesink location={path} "
            "|| gst-launch-1.0 -e "
            "v4l2src device={device} ! "
            "video/x-raw,width={width},height={height},framerate={fps}/1 ! "
            "videoconvert ! "
            "x264enc tune=zerolatency speed-preset=ultrafast bitrate=4096 key-int-max=30 ! "
            "h264parse ! mp4mux faststart=true ! filesink location={path}";
    }

    replace_tokens(command, sizeof(command), pipeline, video_device, path, width, height, fps);
    recorder_pid = spawn_shell_command(command);
    if (recorder_pid < 0) return -1;

    usleep(250000);
    if (waitpid(recorder_pid, NULL, WNOHANG) == recorder_pid) {
        recorder_pid = -1;
        errno = ECHILD;
        return -1;
    }

    printf("Recording with GStreamer: %s\n", command);
    return 0;
}

void camera_recorder_stop(void)
{
    int status;

    if (recorder_pid <= 0) return;

    kill(-recorder_pid, SIGINT);
    for (int i = 0; i < 40; i++) {
        pid_t result = waitpid(recorder_pid, &status, WNOHANG);
        if (result == recorder_pid) {
            recorder_pid = -1;
            return;
        }
        usleep(50000);
    }

    kill(-recorder_pid, SIGTERM);
    waitpid(recorder_pid, &status, 0);
    recorder_pid = -1;
}

int camera_recorder_play(const char * path)
{
    const char * command_template = getenv("MY_MOVE_CAMERA_PLAY_COMMAND");
    char command[1024];

    if (path == NULL || path[0] == '\0') {
        errno = EINVAL;
        return -1;
    }

    if (command_template == NULL || command_template[0] == '\0') {
        command_template = "gst-play-1.0 {path}";
    }

    replace_tokens(command, sizeof(command), command_template, "", path, 0, 0, 0);
    pid_t pid = spawn_shell_command(command);
    if (pid < 0) return -1;

    usleep(250000);
    if (waitpid(pid, NULL, WNOHANG) == pid) {
        errno = ECHILD;
        return -1;
    }

    printf("Playing with GStreamer: %s\n", command);
    return 0;
}

int camera_recorder_play_blocking(const char * path)
{
    const char * command_template = getenv("MY_MOVE_CAMERA_PLAY_COMMAND");
    char command[1024];
    pid_t pid;

    if (path == NULL || path[0] == '\0') {
        errno = EINVAL;
        return -1;
    }

    if (command_template == NULL || command_template[0] == '\0') {
        command_template = "gst-play-1.0 --videosink=kmssink {path}";
    }

    replace_tokens(command, sizeof(command), command_template, "", path, 0, 0, 0);
    pid = spawn_shell_command(command);
    if (pid < 0) return -1;

    printf("Playing with GStreamer: %s\n", command);
    return wait_for_process(pid);
}
