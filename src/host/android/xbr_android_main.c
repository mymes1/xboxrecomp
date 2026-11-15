/*
 * xbr_android_main.c - Android entry point of a recompiled title.
 *
 * [android-port] New. SDL's Java activity (SDLActivity, subclassed by the
 * generated app as XbrGameActivity) loads libSDL2.so and libmain.so and calls
 * SDL_main on its own thread; from there this is the POSIX boot of
 * src/host/xbr_boot.c with Android paths and logging:
 *
 *   argv (from XbrGameActivity.getArguments()):
 *     --game-dir <dir>   extracted disc (default: <external files>/game)
 *     --save-dir <dir>   T:/U:/Z: and the HDD images (default: <files>/saves)
 *     --log <file>       copy of the log (default: <files>/logs/last_run.log)
 *     --env NAME=VALUE   any number of runtime settings (launcher options)
 *
 * stdout and stderr go to logcat (tag "xboxrecomp") and to the log file, so
 * every "[BOOT]" line the boot sequence prints is visible to `adb logcat`
 * and to the launcher's error report.
 *
 * Display: until the Vulkan presenter lands, frames go through the
 * toolkit's SDL presenter (src/video/fb_present_sdl.c, GLES2 renderer under
 * SDL on Android), which shows the surfaces the pushbuffer executor
 * rasterises. The window owns this thread; the guest runs on its own.
 */
#if !defined(__ANDROID__)
#error "xbr_android_main.c is the Android entry point"
#endif

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <SDL.h>
#include <android/log.h>

#include "xbr_host.h"
#include "xbr_android.h"

extern int xbox_HostWindowMain(int (*guest_main)(void));

static XbrBootOptions s_opts;
static char s_game_dir[1024], s_save_dir[1024], s_log_path[1024];

static int guest_main(void)
{
    int rc = xbr_boot_run(&s_opts);
    printf("[BOOT] guest thread finished (rc=%d, last stage: %s)\n", rc, xbr_boot_stage());
    return rc;
}

static void mkdirs(const char *path)
{
    char tmp[1024];
    size_t i, n = strlen(path);
    if (n >= sizeof tmp)
        return;
    memcpy(tmp, path, n + 1);
    for (i = 1; i < n; i++) {
        if (tmp[i] == '/') {
            tmp[i] = 0;
            mkdir(tmp, 0700);
            tmp[i] = '/';
        }
    }
    mkdir(tmp, 0700);
}

static void default_paths(void)
{
    const char *ext = SDL_AndroidGetExternalStoragePath();   /* app-specific, no permission */
    const char *in = SDL_AndroidGetInternalStoragePath();
    const char *base = (ext && *ext) ? ext : (in ? in : ".");

    snprintf(s_game_dir, sizeof s_game_dir, "%s/game", base);
    snprintf(s_save_dir, sizeof s_save_dir, "%s/saves", in && *in ? in : base);
    snprintf(s_log_path, sizeof s_log_path, "%s/logs/last_run.log", base);
}

__attribute__((visibility("default")))
int SDL_main(int argc, char *argv[])
{
    int i;
    char logdir[1024];

    default_paths();
    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--game-dir") && i + 1 < argc)
            snprintf(s_game_dir, sizeof s_game_dir, "%s", argv[++i]);
        else if (!strcmp(argv[i], "--save-dir") && i + 1 < argc)
            snprintf(s_save_dir, sizeof s_save_dir, "%s", argv[++i]);
        else if (!strcmp(argv[i], "--log") && i + 1 < argc)
            snprintf(s_log_path, sizeof s_log_path, "%s", argv[++i]);
        else if (!strcmp(argv[i], "--env") && i + 1 < argc) {
            const char *kv = argv[++i];
            const char *eq = strchr(kv, '=');
            if (eq && eq != kv) {
                char name[128];
                size_t n = (size_t)(eq - kv);
                if (n < sizeof name) {
                    memcpy(name, kv, n);
                    name[n] = 0;
                    setenv(name, eq + 1, 1);
                }
            }
        }
    }

    snprintf(logdir, sizeof logdir, "%s", s_log_path);
    {
        char *slash = strrchr(logdir, '/');
        if (slash) {
            *slash = 0;
            mkdirs(logdir);
        }
    }
    xbr_android_log_start("xboxrecomp", s_log_path);

    /* A relative RECOMP_STATUS_BMP (the scanout snapshot of xbr_stats.c)
     * goes beside the log, where `adb pull` and the launcher can reach it. */
    {
        const char *bmp = getenv("RECOMP_STATUS_BMP");
        if (bmp && *bmp && bmp[0] != '/') {
            char full[1200];
            snprintf(full, sizeof full, "%s/%s", logdir, bmp);
            setenv("RECOMP_STATUS_BMP", full, 1);
        }
    }

    printf("[BOOT] xboxrecomp Android host (arm64: %s, API %d at build)\n",
#if defined(__aarch64__)
           "yes",
#else
           "no",
#endif
           __ANDROID_API__);
    printf("[BOOT] game dir %s\n[BOOT] save dir %s\n[BOOT] log %s\n", s_game_dir,
           s_save_dir, s_log_path);
    mkdirs(s_save_dir);

    /* The Vulkan back end keeps the driver's pipeline cache across runs
     * (src/vk/nv2a_vk.c), in app-private internal storage. --env
     * RECOMP_VK_CACHE=0 turns it off. */
    if (!getenv("RECOMP_VK_CACHE")) {
        const char *in = SDL_AndroidGetInternalStoragePath();
        char dir[1100], file[1200];
        snprintf(dir, sizeof dir, "%s/cache", in && *in ? in : s_save_dir);
        mkdirs(dir);
        snprintf(file, sizeof file, "%s/vk_pipeline_cache.bin", dir);
        setenv("RECOMP_VK_CACHE", file, 1);
    }

    memset(&s_opts, 0, sizeof s_opts);
    xbr_boot_options_from_config(&s_opts);
    s_opts.game_dir = s_game_dir;
    s_opts.save_dir = s_save_dir;

    /* The display is the device screen: the presenter's window (fullscreen
     * under SDL on Android), in landscape. */
    setenv("RECOMP_FB_WINDOW", "1", 0);
    setenv("RECOMP_FULLSCREEN", "1", 0);
    SDL_SetHint(SDL_HINT_ORIENTATIONS, "LandscapeLeft LandscapeRight");
    /* Keep the device's back button for the activity, not a key. */
    SDL_SetHint(SDL_HINT_ANDROID_TRAP_BACK_BUTTON, "0");
    xbr_boot_env_defaults();

    {
        int rc;
        xbr_window_install();       /* the Vulkan presenter, the app lifecycle */
        rc = xbox_HostWindowMain(guest_main);
        printf("[BOOT] SDL_main returning %d\n", rc);
        xbr_android_log_stop();
        return rc;
    }
}
