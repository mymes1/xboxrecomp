/*
 * xbr_android_log.c - stdout/stderr to logcat and a log file.
 *
 * [android-port] New. The runtime reports everything with printf/fprintf;
 * an Android app's stdout and stderr go nowhere. A pipe replaces both
 * descriptors and a thread forwards each complete line to
 * __android_log_write and to a file the launcher can show after a crash.
 */
#if !defined(__ANDROID__)
#error "xbr_android_log.c is Android-only"
#endif

#include <android/log.h>
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "xbr_android.h"

static int s_pipe[2] = { -1, -1 };
static pthread_t s_thread;
static int s_started;
static FILE *s_copy;
static char s_tag[32] = "xboxrecomp";

static void emit(const char *line)
{
    int prio = ANDROID_LOG_INFO;
    if (strstr(line, "[CRASH]") || strstr(line, "FAILED") || strstr(line, "FAIL "))
        prio = ANDROID_LOG_ERROR;
    else if (strstr(line, "[UNIMPL]") || strstr(line, "warning"))
        prio = ANDROID_LOG_WARN;
    __android_log_write(prio, s_tag, line);
    if (s_copy) {
        fputs(line, s_copy);
        fputc('\n', s_copy);
        fflush(s_copy);
    }
}

static void *forward(void *arg)
{
    char buf[4096];
    size_t have = 0;
    (void)arg;
    for (;;) {
        ssize_t n = read(s_pipe[0], buf + have, sizeof buf - 1 - have);
        size_t start = 0, i;
        if (n <= 0) {
            if (n < 0 && errno == EINTR)
                continue;
            break;
        }
        have += (size_t)n;
        for (i = 0; i < have; i++) {
            if (buf[i] == '\n') {
                buf[i] = 0;
                emit(buf + start);
                start = i + 1;
            }
        }
        if (start < have) {
            memmove(buf, buf + start, have - start);
            have -= start;
            if (have == sizeof buf - 1) {      /* a line longer than the buffer */
                buf[have] = 0;
                emit(buf);
                have = 0;
            }
        } else {
            have = 0;
        }
    }
    if (have) {
        buf[have] = 0;
        emit(buf);
    }
    return NULL;
}

int xbr_android_log_start(const char *tag, const char *copy_path)
{
    if (s_started)
        return 0;
    if (tag)
        snprintf(s_tag, sizeof s_tag, "%s", tag);
    if (copy_path)
        s_copy = fopen(copy_path, "w");
    if (pipe(s_pipe) != 0)
        return -1;
    setvbuf(stdout, NULL, _IOLBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);
    dup2(s_pipe[1], 1);
    dup2(s_pipe[1], 2);
    if (pthread_create(&s_thread, NULL, forward, NULL) != 0) {
        __android_log_write(ANDROID_LOG_ERROR, s_tag, "log forwarder thread failed");
        return -1;
    }
    pthread_detach(s_thread);
    s_started = 1;
    return 0;
}

void xbr_android_log_stop(void)
{
    fflush(stdout);
    fflush(stderr);
    /* The forwarder drains what is in the pipe; give it a moment. */
    usleep(50 * 1000);
    if (s_copy)
        fflush(s_copy);
}
