/*
 * nv2a_vk.h -- the NV2A render back end on Vulkan. [android-port]
 *
 * The pushbuffer executor (src/kernel/nv2a_pb_exec.c) decodes what a title
 * sends the NV2A: it runs the vertex programs or the fixed-function
 * transform on the CPU and hands each batch's triangles, texture stages,
 * register-combiner program and render state to a back end
 * (nv2a_backend.h, draw_ex/clear_ex). This back end draws them with Vulkan:
 * the register combiners become one fragment pipeline per combiner program
 * (src/vk/shaders/nv2a.frag), textures are decoded once per change and kept
 * on the GPU, depth and stencil live in GPU depth images, and every colour
 * surface the GPU draws is written back to guest memory at the flip's
 * present_sync -- so the presenter, frame dumps and anything else that reads
 * the framebuffer see exactly what the CPU rasteriser would have written.
 *
 * Core Vulkan 1.0 only: no Adreno- or desktop-specific extension is
 * required. Optional and tested before use: depthClamp (window z outside
 * the depth range clamps, as the CPU rasteriser's compare would accept,
 * instead of clipping), VK_KHR_portability_subset (enabled when a device
 * requires it). The Vulkan loader is opened at run time (dlopen), so a
 * binary with this back end still runs, on the CPU rasteriser, where there
 * is no Vulkan at all.
 *
 * Environment (docs/runtime/environment-variables.md):
 *   RECOMP_VK=0            do not register (CPU rasteriser); read by the host
 *   RECOMP_VK_DEVICE=<n>   physical device index (default: first GPU)
 *   RECOMP_VK_CULL=1       honour the title's cull state (needs
 *                          RECOMP_STRIP_ALT_WINDING=1; see nv2a_vk.c)
 *   RECOMP_VK_STATS=<n>    print [VK] counters every n flips
 *   RECOMP_VK_VALIDATION=1 enable VK_LAYER_KHRONOS_validation if present
 *   RECOMP_VK_LIB=<path>   Vulkan loader to open (default libvulkan.so[.1])
 * Texture filtering follows the CPU rasteriser's RECOMP_TEX_BILINEAR and
 * RECOMP_TEX_ANISO, so both renderers agree.
 */
#ifndef NV2A_VK_H
#define NV2A_VK_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint64_t draws, triangles, clears;
    uint64_t pipelines;             /* created so far */
    uint64_t pipeline_build_us;     /* time spent creating them (driver compile) */
    uint64_t texture_uploads, texture_bytes;
    uint64_t surface_uploads, readbacks, readback_bytes;
    uint64_t submits, render_passes;
    uint64_t pcache_load_bytes;     /* pipeline cache accepted at start (RECOMP_VK_CACHE) */
    uint64_t pcache_saves, pcache_save_bytes;   /* writes this run; size of the last */
} Nv2aVkStats;

/* Bring Vulkan up (first call) and register as the executor's back end.
 * 0 on success. Negative when there is no usable device: nothing is
 * registered, the CPU rasteriser keeps drawing, and nv2a_vk_status() says
 * why. Calling it again after success re-registers. */
int nv2a_vk_register(void);

/* One line: the device, driver and the optional features in use, or why the
 * back end is off ("off: ..."). Never NULL. */
const char *nv2a_vk_status(void);

/* Write every surface the GPU has drawn back to guest memory now (it is
 * otherwise done at each flip). Executor thread only. */
void nv2a_vk_sync(void);

/* Unregister (the CPU rasteriser takes over) and release everything. */
void nv2a_vk_shutdown(void);

void nv2a_vk_get_stats(Nv2aVkStats *out);

/* ---- window presentation (optional) -------------------------------------
 *
 * Shows finished frames in a window through a Vulkan swapchain on the
 * renderer's device, instead of a second graphics API (the SDL renderer is
 * GLES on Android). Frames come from guest memory -- what the flip's
 * present_sync wrote back, or what the CPU rasteriser or the title drew --
 * so every renderer is shown the same way. The window thread calls these;
 * it shares the device and its queue with the executor (locked).
 *
 * Instance extensions: VK_KHR_surface and the platform's (from the window
 * system, e.g. SDL_Vulkan_GetInstanceExtensions), all tested before they
 * are enabled; device: VK_KHR_swapchain, only when the graphics queue can
 * present to the window. Without them the presenter is off and the window
 * uses its own renderer; the NV2A renderer is unaffected either way.
 *
 * Environment: RECOMP_PRESENT_VSYNC=0 (mailbox or immediate instead of
 * FIFO), RECOMP_ASPECT=4:3 (default) | 16:9 | <w>:<h> | stretch | native,
 * RECOMP_PRESENT_FILTER=linear (default) | nearest,
 * RECOMP_PRESENT_CAPTURE=1 (tests: keep a copy of each presented image). */

/* Makes a VkSurfaceKHR for `instance` (a VkInstance); 0 on success. Called
 * on the thread that calls nv2a_vk_present_init / _frame. */
typedef int (*Nv2aVkSurfaceFn)(void *ctx, void *instance, uint64_t *surface);

typedef struct {
    uint64_t presents, skipped;     /* frames shown; frames dropped (no window, timeouts) */
    uint64_t swapchains, suspends;  /* swapchains made; times the window went away */
    uint32_t width, height;         /* the swapchain's size */
    int active;                     /* a swapchain exists now */
} Nv2aVkPresentStats;

/* Before nv2a_vk_register (it brings the device up with the surface
 * extensions). 0: frames can be presented; negative: use another presenter
 * (nv2a_vk_present_status says why). width/height: the window's size in
 * pixels, used where the window system leaves the swapchain size open. */
int nv2a_vk_present_init(const char *const *instance_exts, unsigned n_exts,
                         Nv2aVkSurfaceFn create_surface, void *ctx,
                         unsigned width, unsigned height);

/* The swapchain (size, format, present mode, scaling) or "off: <why>". */
const char *nv2a_vk_present_status(void);

/* Shows a width x height frame (`pitch` bytes a row; bpp 2 = R5G6B5,
 * 4 = X8R8G8B8), or with pixels NULL the last one again. 0: presented;
 * 1: not now (no window, minimised, timed out) -- dropped; negative: the
 * presenter is off for good and has released the window. */
int nv2a_vk_present_frame(const void *pixels, unsigned width, unsigned height,
                          unsigned pitch, unsigned bpp);

/* The window changed size: a new swapchain at the next frame. */
void nv2a_vk_present_resize(unsigned width, unsigned height);

/* The window's surface is going away (Android: the app went to the
 * background) / is back. Frames in between are dropped. */
void nv2a_vk_present_suspend(void);
void nv2a_vk_present_resume(void);

void nv2a_vk_present_get_stats(Nv2aVkPresentStats *out);

/* Tests (RECOMP_PRESENT_CAPTURE=1): the last presented image as RGBA8. */
int nv2a_vk_present_capture(uint8_t *rgba, unsigned max_bytes,
                            unsigned *width, unsigned *height);

/* Releases the swapchain and the surface; the device goes once the
 * renderer has shut down too. */
void nv2a_vk_present_shutdown(void);

#ifdef __cplusplus
}
#endif

#endif /* NV2A_VK_H */
