/*
 * xbr_android.h - Android-only pieces of the host layer.
 *
 * [android-port] New.
 */
#ifndef XBR_ANDROID_H
#define XBR_ANDROID_H

#ifdef __cplusplus
extern "C" {
#endif

/* Redirects stdout and stderr into logcat (one record per line, under
 * `tag`) and, if `copy_path` is non-NULL, appends the same lines to that
 * file. Returns 0 on success. */
int xbr_android_log_start(const char *tag, const char *copy_path);

/* Flushes and stops the forwarder (the process may keep running). */
void xbr_android_log_stop(void);

#ifdef __cplusplus
}
#endif

#endif /* XBR_ANDROID_H */
