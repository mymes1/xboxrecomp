/*
 * xbr_main_posix.c - desktop POSIX entry point (Linux, macOS) for a title.
 *
 * [android-port] New. The Android build does not use this file: its entry is
 * SDL's SDL_main (src/host/android/xbr_android_main.c). Used by CI to run the
 * same generated code and runtime on Linux x86-64 and Linux arm64 runners.
 *
 *   <exe> [game_dir] [save_dir]
 *
 * game_dir defaults to $RECOMP_GAME_FILES, then ./game; save_dir to
 * $RECOMP_SAVE_DIR, then the toolkit default. RECOMP_FB_WINDOW opens the
 * SDL framebuffer window (xbox_HostWindowMain); headless otherwise.
 */
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "xbr_host.h"

extern int xbox_HostWindowMain(int (*guest_main)(void));

static XbrBootOptions s_opts;

static int guest_main(void)
{
    return xbr_boot_run(&s_opts);
}

int main(int argc, char **argv)
{
    const char *env_game = getenv("RECOMP_GAME_FILES");
    const char *env_save = getenv("RECOMP_SAVE_DIR");
    const char *env_ram = getenv("XBR_TOTAL_RAM_MB");

    memset(&s_opts, 0, sizeof s_opts);
    xbr_boot_options_from_config(&s_opts);
    s_opts.game_dir = argc > 1 ? argv[1] : (env_game && *env_game ? env_game : "game");
    s_opts.save_dir = argc > 2 ? argv[2] : (env_save && *env_save ? env_save : NULL);
    if (env_ram && *env_ram)
        s_opts.total_ram = (size_t)strtoul(env_ram, NULL, 10) * 1024u * 1024u;

    printf("[BOOT] xboxrecomp POSIX host\n");
    xbr_boot_env_defaults();
    /* The SDL window owns the main thread (AppKit on macOS); the guest gets
     * its own. Keep SIGUSR1 for the dump thread a title may install. */
    {
        sigset_t set;
        sigemptyset(&set);
        sigaddset(&set, SIGPIPE);
        pthread_sigmask(SIG_BLOCK, &set, NULL);
    }
    xbr_window_install();           /* RECOMP_PRESENT=vulkan: the Vulkan presenter */
    return xbox_HostWindowMain(guest_main);
}
