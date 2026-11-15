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

#ifdef __cplusplus
}
#endif

#endif /* NV2A_VK_H */
