/*
 * xbr_window.c -- what the host adds to the framebuffer window. [android-port]
 *
 * The window itself is the toolkit's (src/video/fb_present_sdl.c: the SDL
 * window on the main thread, the event loop, the frame handoff from the
 * flip). This installs, through its external-presenter hook
 * (fb_present_ext.h):
 *
 *   - the Vulkan presenter (nv2a_vk_present_*): frames go to a Vulkan
 *     swapchain on the NV2A renderer's device instead of the SDL renderer,
 *     which on Android is a second graphics API (GLES) on the same window.
 *     RECOMP_PRESENT=vulkan|sdl; the default is vulkan on Android, sdl
 *     elsewhere. If Vulkan cannot present to the window the SDL renderer is
 *     used, and the reason is logged ("[BOOT] present: ...").
 *
 *   - the application lifecycle (Android): when the app goes to the
 *     background the presenter lets go of the window's surface (Android
 *     destroys it) and the guest is held at its next flip, so a game in the
 *     background stops drawing rather than running on with no screen; both
 *     come back in the foreground. Threads that never reach a flip keep
 *     running (audio is paused by SDL itself).
 *
 * Log lines: "[BOOT] present: vulkan (<swapchain>)" or "[BOOT] present: sdl
 * (<why>)"; "[HOST] background ..." / "[HOST] foreground ...".
 */
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <SDL.h>

#include "xbr_host.h"
#include "video/fb_present_ext.h"

#ifdef XBR_HAVE_VULKAN
#include <SDL_vulkan.h>
#include <SDL_syswm.h>
#include "nv2a_vk.h"
#endif

/* ---- holding the guest while in the background ------------------------------- */

static pthread_mutex_t s_hold_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t s_hold_cv = PTHREAD_COND_INITIALIZER;
static int s_hold;
static uint64_t s_holds;

void xbr_hold_set(int on)
{
    pthread_mutex_lock(&s_hold_mu);
    s_hold = on;
    if (on)
        s_holds++;
    pthread_cond_broadcast(&s_hold_cv);
    pthread_mutex_unlock(&s_hold_mu);
}

int xbr_hold_active(void)
{
    return __atomic_load_n(&s_hold, __ATOMIC_ACQUIRE);
}

/* At every flip (xbr_stats.c, on the executor's thread). */
void xbr_hold_point(void)
{
    if (!__atomic_load_n(&s_hold, __ATOMIC_ACQUIRE))
        return;
    pthread_mutex_lock(&s_hold_mu);
    if (s_hold) {
        printf("[HOST] guest held at a flip while in the background\n");
        fflush(stdout);
    }
    while (s_hold)
        pthread_cond_wait(&s_hold_cv, &s_hold_mu);
    pthread_mutex_unlock(&s_hold_mu);
}

/* ---- the Vulkan presenter ------------------------------------------------------ */

#ifdef XBR_HAVE_VULKAN
static SDL_Window *s_win;
static int s_vk_on;

/* The window's VkSurfaceKHR, made by SDL for its window system. On Android
 * there is no surface between surfaceDestroyed and the next surfaceChanged:
 * report "not yet" rather than hand SDL a window it has released. */
static int vk_surface(void *ctx, void *instance, uint64_t *out)
{
    VkSurfaceKHR s;
#ifdef __ANDROID__
    SDL_SysWMinfo info;
    SDL_VERSION(&info.version);
    if (!SDL_GetWindowWMInfo(s_win, &info) || !info.info.android.window)
        return -1;
#endif
    (void)ctx;
    memset(&s, 0, sizeof s);
    if (!SDL_Vulkan_CreateSurface(s_win, (VkInstance)instance, &s))
        return -1;
    memset(out, 0, sizeof *out);
    memcpy(out, &s, sizeof s < sizeof *out ? sizeof s : sizeof *out);
    return 0;
}

static int vk_open(SDL_Window *win, char *msg, size_t msgn)
{
    const char *names[8];
    unsigned n = 0;
    int w = 0, h = 0;

    s_win = win;
    if (!SDL_Vulkan_GetInstanceExtensions(win, &n, NULL) || n > 8
        || !SDL_Vulkan_GetInstanceExtensions(win, &n, names)) {
        snprintf(msg, msgn, "SDL names no Vulkan surface extensions for the window (%s)",
                 SDL_GetError());
        printf("[BOOT] present: sdl (%s)\n", msg);
        fflush(stdout);
        return -1;
    }
    SDL_Vulkan_GetDrawableSize(win, &w, &h);
    if (nv2a_vk_present_init(names, n, vk_surface, NULL, (unsigned)w, (unsigned)h) != 0) {
        snprintf(msg, msgn, "Vulkan presenter %s", nv2a_vk_present_status());
        printf("[BOOT] present: sdl (%s)\n", msg);
        fflush(stdout);
        return -1;
    }
    s_vk_on = 1;
    snprintf(msg, msgn, "%s", nv2a_vk_present_status());
    printf("[BOOT] present: vulkan (%s)\n", msg);
    fflush(stdout);
    return 0;
}

static int vk_present(const void *px, uint32_t w, uint32_t h, uint32_t pitch, uint32_t bpp)
{
    int r = nv2a_vk_present_frame(px, w, h, pitch, bpp);
    if (r < 0 && s_vk_on) {
        s_vk_on = 0;
        printf("[BOOT] present: sdl (the Vulkan presenter stopped: %s)\n",
               nv2a_vk_present_status());
        fflush(stdout);
    }
    return r;
}

static void vk_close(void)
{
    if (s_vk_on)
        nv2a_vk_present_shutdown();
    s_vk_on = 0;
}
#endif /* XBR_HAVE_VULKAN */

/* ---- events -------------------------------------------------------------------- */

static void on_event(const SDL_Event *ev)
{
    switch (ev->type) {
    case SDL_APP_WILLENTERBACKGROUND:
#ifdef XBR_HAVE_VULKAN
        if (s_vk_on)
            nv2a_vk_present_suspend();
#endif
        xbr_hold_set(1);
        printf("[HOST] background: presenter suspended, guest held at its next flip\n");
        fflush(stdout);
        break;
    case SDL_APP_DIDENTERFOREGROUND:
#ifdef XBR_HAVE_VULKAN
        if (s_vk_on)
            nv2a_vk_present_resume();
#endif
        xbr_hold_set(0);
        printf("[HOST] foreground: presenter resumed, guest released\n");
        fflush(stdout);
        break;
    case SDL_APP_LOWMEMORY:
        printf("[HOST] the system reports low memory\n");
        fflush(stdout);
        break;
#ifdef XBR_HAVE_VULKAN
    case SDL_WINDOWEVENT:
        if (s_vk_on && (ev->window.event == SDL_WINDOWEVENT_SIZE_CHANGED
                        || ev->window.event == SDL_WINDOWEVENT_RESIZED)) {
            int w = 0, h = 0;
            SDL_Vulkan_GetDrawableSize(s_win, &w, &h);
            nv2a_vk_present_resize((unsigned)w, (unsigned)h);
        }
        break;
#endif
    default:
        break;
    }
}

static XboxFbPresenter s_presenter;

void xbr_window_install(void)
{
    const char *e = getenv("RECOMP_PRESENT");
    int want_vk;

#ifdef __ANDROID__
    want_vk = !e || !*e || strcmp(e, "sdl") != 0;
#else
    want_vk = e && !strcmp(e, "vulkan");
#endif
    memset(&s_presenter, 0, sizeof s_presenter);
    s_presenter.name = "vulkan";
    s_presenter.event = on_event;
#ifdef XBR_HAVE_VULKAN
    if (want_vk) {
        s_presenter.window_flags = SDL_WINDOW_VULKAN;
        s_presenter.open = vk_open;
        s_presenter.present = vk_present;
        s_presenter.close = vk_close;
    }
#else
    if (want_vk) {
        printf("[BOOT] present: sdl (built without the Vulkan back end)\n");
        fflush(stdout);
    }
#endif
    xbox_FramebufferWindowSetPresenter(&s_presenter);
}
