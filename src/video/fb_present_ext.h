/**
 * An external presenter for the POSIX framebuffer window (fb_present_sdl.c).
 * [android-port]
 *
 * The window shows frames with the SDL renderer by default (GLES on
 * Android). A host can hand it a presenter of its own -- src/host's draws
 * through a Vulkan swapchain on the NV2A renderer's device -- before calling
 * xbox_HostWindowMain(). The window loop, the event pump, the frame handoff
 * from the flip and the title bar stay the window's; the presenter only
 * puts frames on the screen. If it cannot start, or fails later, the window
 * falls back to the SDL renderer.
 */
#ifndef XBOX_FB_PRESENT_EXT_H
#define XBOX_FB_PRESENT_EXT_H

#include <SDL.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct XboxFbPresenter {
    const char *name;           /* for the log, e.g. "vulkan" */
    Uint32 window_flags;        /* added to the window's flags (SDL_WINDOW_VULKAN) */

    /* On the main thread, after the window exists and before the guest
     * starts. 0: the presenter is in use (msg: a description); otherwise
     * msg says why not and the SDL renderer is used. */
    int (*open)(SDL_Window *win, char *msg, size_t msgn);

    /* A w x h frame (bpp 2 = R5G6B5, 4 = X8R8G8B8, `pitch` bytes a row), or
     * with px NULL the last one again (expose, resize). 0: shown; 1: not
     * now (no surface, minimised) -- the window tries again; negative: the
     * presenter is off and has released the window -- the window switches
     * to the SDL renderer. */
    int (*present)(const void *px, uint32_t w, uint32_t h, uint32_t pitch, uint32_t bpp);

    /* Every event the window loop takes, also after a fallback (resizes,
     * application lifecycle). May be NULL. */
    void (*event)(const SDL_Event *ev);

    /* On the main thread, before the window is destroyed. May be NULL. */
    void (*close)(void);
} XboxFbPresenter;

/* Before xbox_HostWindowMain(); NULL restores the SDL renderer. */
void xbox_FramebufferWindowSetPresenter(const XboxFbPresenter *p);

#ifdef __cplusplus
}
#endif

#endif /* XBOX_FB_PRESENT_EXT_H */
