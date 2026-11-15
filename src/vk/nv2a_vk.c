/*
 * nv2a_vk.c -- the NV2A render back end on Vulkan. [android-port]
 *
 * See nv2a_vk.h for what it is and docs/technical/pushbuffer-executor.md
 * ("Render Back Ends") for the interface. The executor's CPU rasteriser is
 * the reference: tests/nv2a_vk_backend draws the same pushbuffer with both
 * and compares guest memory. So everything below follows a rule of the CPU
 * path, and says which when it is not obvious:
 *
 * Surfaces. A colour surface is a VkImage (B8G8R8A8) shadowing guest
 * memory. Before its first use in a frame the guest bytes are hashed, and
 * uploaded when they changed (the title's CPU may write a framebuffer: a
 * movie, a software overlay, a clear of its own). What the GPU draws is
 * written back at the flip's present_sync, and on demand when a texture or
 * another surface aliases it. A 16-bit surface is drawn in 8888 too and
 * written back truncated to 5:6:5 exactly as put_pixel truncates; its alpha
 * is never written, so destination alpha reads 1.0 as it does there.
 * Swizzled surfaces are de- and re-swizzled on the way (d3d8_swizzle.h).
 *
 * Depth. The CPU path keeps depth host-side, one float buffer per zeta
 * offset, never in guest memory; so does this, as a D24S8 (or D32S8) image
 * per zeta offset. Depth is the window z over 2^16-1 or 2^24-1, compared as
 * the title produced it, never re-ranged.
 *
 * Pixels. One graphics pipeline per (combiner program, fixed-function
 * state); the program goes in as specialization constants (nv2a.frag). The
 * per-batch data -- combiner constants, fog colour, alpha reference, border
 * colours -- goes in a uniform buffer.
 *
 * Textures. Decoded by the executor's own decoder
 * (nv2a_backend_decode_stage: every format the CPU path samples, palettes,
 * cube faces), uploaded once and re-hashed once a frame. Level 0 only and
 * the CPU path's filter choice (nearest, or the title's tent filter in the
 * combiner path), so the two renderers agree.
 *
 * Submission. One command buffer, recorded as batches arrive and submitted
 * at present_sync or when a readback or a full ring needs it; the thread
 * waits for it. ponytail: no frame overlap -- the executor waits for the GPU
 * once a frame. Double-buffer the rings and fence per frame when profiling
 * on the device says the wait matters.
 *
 * Threading: everything runs on the executor thread, the only caller of a
 * back end, so nothing here locks.
 */
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>

#include <dlfcn.h>
#include <errno.h>
#include <pthread.h>
#include <stdarg.h>
#include <unistd.h>
#include <time.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "nv2a_backend.h"
#include "nv2a_combiner.h"
#include "../d3d/d3d8_swizzle.h"
#include "nv2a_vk.h"
#include "nv2a_vk_spv.h"

ptrdiff_t xbox_GetMemoryOffset(void);      /* xbox_memory_layout.h */

/* ---- the Vulkan entry points we use (core 1.0) ------------------------- */

#define NVK_INSTANCE_FNS(X)                                                    \
    X(DestroyInstance) X(EnumeratePhysicalDevices)                             \
    X(GetPhysicalDeviceProperties) X(GetPhysicalDeviceFeatures)                \
    X(GetPhysicalDeviceQueueFamilyProperties)                                  \
    X(GetPhysicalDeviceMemoryProperties) X(GetPhysicalDeviceFormatProperties)  \
    X(CreateDevice) X(EnumerateDeviceExtensionProperties) X(GetDeviceProcAddr)

#define NVK_DEVICE_FNS(X)                                                      \
    X(DestroyDevice) X(GetDeviceQueue) X(QueueSubmit) X(DeviceWaitIdle)        \
    X(CmdBlitImage) X(CmdClearColorImage) X(CreateSemaphore) X(DestroySemaphore)\
    X(ResetCommandBuffer)                                                      \
    X(AllocateMemory) X(FreeMemory) X(MapMemory) X(UnmapMemory)                \
    X(InvalidateMappedMemoryRanges) X(BindBufferMemory) X(BindImageMemory)     \
    X(GetBufferMemoryRequirements) X(GetImageMemoryRequirements)               \
    X(CreateFence) X(DestroyFence) X(ResetFences) X(WaitForFences)             \
    X(CreateBuffer) X(DestroyBuffer) X(CreateImage) X(DestroyImage)            \
    X(CreateImageView) X(DestroyImageView)                                     \
    X(CreateShaderModule) X(DestroyShaderModule)                               \
    X(CreatePipelineCache) X(DestroyPipelineCache) X(GetPipelineCacheData)     \
    X(CreateGraphicsPipelines) X(DestroyPipeline)                              \
    X(CreatePipelineLayout) X(DestroyPipelineLayout)                           \
    X(CreateSampler) X(DestroySampler)                                         \
    X(CreateDescriptorSetLayout) X(DestroyDescriptorSetLayout)                 \
    X(CreateDescriptorPool) X(DestroyDescriptorPool) X(ResetDescriptorPool)    \
    X(AllocateDescriptorSets) X(UpdateDescriptorSets)                          \
    X(CreateFramebuffer) X(DestroyFramebuffer)                                 \
    X(CreateRenderPass) X(DestroyRenderPass)                                   \
    X(CreateCommandPool) X(DestroyCommandPool) X(ResetCommandPool)             \
    X(AllocateCommandBuffers) X(BeginCommandBuffer) X(EndCommandBuffer)        \
    X(CmdBindPipeline) X(CmdSetViewport) X(CmdSetScissor)                      \
    X(CmdSetBlendConstants) X(CmdSetStencilCompareMask)                        \
    X(CmdSetStencilWriteMask) X(CmdSetStencilReference)                        \
    X(CmdBindDescriptorSets) X(CmdBindVertexBuffers) X(CmdDraw)                \
    X(CmdCopyBufferToImage) X(CmdCopyImageToBuffer) X(CmdClearAttachments)     \
    X(CmdClearDepthStencilImage) X(CmdPipelineBarrier) X(CmdPushConstants)     \
    X(CmdBeginRenderPass) X(CmdEndRenderPass)

/* Window presentation only: VK_KHR_surface (instance) and VK_KHR_swapchain
 * (device), loaded when a window asked for them (nv2a_vk_present_init). */
#define NVK_SURFACE_FNS(X)                                                     \
    X(DestroySurfaceKHR) X(GetPhysicalDeviceSurfaceSupportKHR)                 \
    X(GetPhysicalDeviceSurfaceCapabilitiesKHR)                                 \
    X(GetPhysicalDeviceSurfaceFormatsKHR)                                      \
    X(GetPhysicalDeviceSurfacePresentModesKHR)

#define NVK_SWAPCHAIN_FNS(X)                                                   \
    X(CreateSwapchainKHR) X(DestroySwapchainKHR) X(GetSwapchainImagesKHR)      \
    X(AcquireNextImageKHR) X(QueuePresentKHR)

static struct {
    PFN_vkGetInstanceProcAddr GetInstanceProcAddr;
    PFN_vkCreateInstance CreateInstance;
    PFN_vkEnumerateInstanceExtensionProperties EnumerateInstanceExtensionProperties;
    PFN_vkEnumerateInstanceLayerProperties EnumerateInstanceLayerProperties;
#define NVK_FN(n) PFN_vk##n n;
    NVK_INSTANCE_FNS(NVK_FN)
    NVK_DEVICE_FNS(NVK_FN)
    NVK_SURFACE_FNS(NVK_FN)
    NVK_SWAPCHAIN_FNS(NVK_FN)
#undef NVK_FN
} vk;

/* The one queue is shared by the executor (rendering) and the window thread
 * (presenting): every queue call, and vkDeviceWaitIdle, holds this. */
static pthread_mutex_t s_qlock = PTHREAD_MUTEX_INITIALIZER;

/* ---- state -------------------------------------------------------------- */

#define NVK_MAX_SURF      24
#define NVK_MAX_DEPTH     8
#define NVK_FB_PER_SURF   4
#define NVK_TEX_BUCKETS   1024
#define NVK_MAX_SETS      4096
#define NVK_VERT_RING     (32u << 20)
#define NVK_UBO_RING      (4u << 20)
#define NVK_STAGING       (16u << 20)
#define NVK_TEX_KEEP      600           /* frames a texture survives unused */
#define NVK_MAX_DIM       4096
#define NVK_NSPEC         40

typedef struct {
    VkBuffer buf;
    VkDeviceMemory mem;
    uint8_t *map;
    VkDeviceSize size, used;
    int coherent;
} NvkBuf;

typedef struct {
    VkImage img;
    VkDeviceMemory mem;
    VkImageView view;
    VkImageLayout layout;
    VkImageAspectFlags aspect;
    uint32_t w, h, layers;
} NvkImage;

typedef struct {
    int used;
    uint32_t va, gen;
    NvkImage im;
} NvkDepth;

typedef struct {
    const NvkDepth *d;
    uint32_t gen;
    VkFramebuffer fb;
} NvkFb;

typedef struct {
    int used;
    uint32_t va, pitch, bpp, swizzled;  /* identity; pitch = w * bpp if swizzled */
    uint32_t span;                      /* guest bytes from va */
    NvkImage im;
    int dirty;                          /* drawn since last written back */
    uint32_t dx0, dy0, dx1, dy1;        /* what was drawn */
    uint64_t hash;                      /* guest bytes at the last sync */
    int hash_ok, validated;
    uint64_t last_use;
    NvkFb fbs[NVK_FB_PER_SURF];
    uint32_t fb_next;
} NvkSurf;

typedef struct NvkTex {
    struct NvkTex *next;
    uint32_t offset, width, height, pitch, color, palette, cube, face_stride;
    uint32_t bytes;                     /* guest span of level 0 (all faces) */
    NvkImage im;
    uint64_t hash;
    int fresh, validated, bad;
    uint64_t last_use;                  /* frame */
} NvkTex;

typedef struct {
    uint32_t spec[NVK_NSPEC];
    uint32_t blend[4];                  /* enable, src, dst, op (Vulkan values) */
    uint32_t write_mask;
    uint32_t depth[3];                  /* test, write, compare op */
    uint32_t stencil[5];                /* enable, fail, depth fail, pass, compare */
    uint32_t cull, front;
} NvkPipeKey;

typedef struct {
    int used;
    uint64_t h;
    NvkPipeKey key;
    VkPipeline pipe;
} NvkPipe;

/* The uniform block of nv2a.frag (std140). */
typedef struct {
    float factor0[8][4], factor1[8][4];
    float final_c0[4], final_c1[4];
    uint32_t misc[4];
    float fog_color[4];
    float border[4][4];
} NvkUbo;

typedef struct {
    float vp[4], scale01[4], scale23[4];
} NvkPush;

static struct {
    int ok, registered, init_tried, renderer_tried;
    char status[768];
    char devdesc[400], feat[96];
    void *lib;
    VkInstance inst;
    VkPhysicalDevice phys;
    VkDevice dev;
    VkQueue queue;
    uint32_t qfam;
    VkPhysicalDeviceMemoryProperties mem;
    VkDeviceSize ubo_align;
    VkFormat depth_fmt;
    int depth_clamp;
    VkCommandPool cpool;
    VkCommandBuffer cmd;
    VkFence fence;
    int recording;
    VkRenderPass rp;
    VkDescriptorSetLayout dsl;
    VkPipelineLayout pl;
    VkDescriptorPool dpool;
    VkShaderModule vs, fs;
    VkPipelineCache pcache;
    VkSampler samplers[2][3][3];
    NvkBuf vring, uring, staging, rbuf;
    NvkImage white;
    NvkSurf surf[NVK_MAX_SURF];
    NvkDepth depth[NVK_MAX_DEPTH];
    uint32_t depth_next;
    NvkTex *tex[NVK_TEX_BUCKETS];
    NvkPipe *pipes;
    uint32_t pipe_cap, pipe_n;
    int in_pass;
    NvkSurf *pass_surf;
    NvkDepth *pass_depth;
    uint64_t frame, epoch, clock;
    int cull, bilinear, stats_every;
    char pc_path[1024];             /* RECOMP_VK_CACHE; empty = not kept */
    char pc_state[96];              /* for the status line */
    int pc_dirty;                   /* pipelines built since the last save */
    uint64_t pc_new_frame;          /* frame of the newest one */
    NvkImage *dead_img;
    int dead_img_n, dead_img_cap;
    VkFramebuffer *dead_fb;
    int dead_fb_n, dead_fb_cap;
    Nv2aVkStats st;
} V;

/* ---- small helpers -------------------------------------------------------- */

static void nvk_log(const char *fmt, ...)
{
    va_list ap;
    fputs("[VK] ", stderr);
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    fflush(stderr);
}

static void nvk_fail(const char *what, VkResult r)
{
    snprintf(V.status, sizeof V.status,
             "off: %s failed (VkResult %d); the CPU rasteriser draws from here",
             what, (int)r);
    nvk_log("%s", V.status);
    V.ok = 0;
    if (V.registered) {
        nv2a_backend_register(NULL);
        V.registered = 0;
    }
}

static int ck(VkResult r, const char *what)
{
    if (r == VK_SUCCESS)
        return 1;
    nvk_fail(what, r);
    return 0;
}

static VkResult q_submit(const VkSubmitInfo *si, VkFence fence)
{
    VkResult r;
    pthread_mutex_lock(&s_qlock);
    r = vk.QueueSubmit(V.queue, 1, si, fence);
    pthread_mutex_unlock(&s_qlock);
    return r;
}

static void dev_idle(void)
{
    pthread_mutex_lock(&s_qlock);
    vk.DeviceWaitIdle(V.dev);
    pthread_mutex_unlock(&s_qlock);
}

static uint8_t *guest(uint32_t addr)
{
    return (uint8_t *)xbox_GetMemoryOffset() + addr;
}

static int overlap(uint32_t a, uint32_t an, uint32_t b, uint32_t bn)
{
    return (uint64_t)a < (uint64_t)b + bn && (uint64_t)b < (uint64_t)a + an;
}

static uint64_t now_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000u + (uint64_t)ts.tv_nsec / 1000u;
}

static uint64_t rotl64(uint64_t x, int r)
{
    return (x << r) | (x >> (64 - r));
}

/* Change detection for guest bytes (xxh64-shaped, four lanes). */
static uint64_t nvk_hash(const uint8_t *p, size_t n, uint64_t seed)
{
    const uint64_t P1 = 0x9E3779B185EBCA87ull, P2 = 0xC2B2AE3D27D4EB4Full;
    uint64_t a = seed + P1 + P2, b = seed + P2, c = seed, d = seed - P1, h;

    while (n >= 32) {
        uint64_t w[4];
        memcpy(w, p, 32);
        a = rotl64(a + w[0] * P2, 31) * P1;
        b = rotl64(b + w[1] * P2, 31) * P1;
        c = rotl64(c + w[2] * P2, 31) * P1;
        d = rotl64(d + w[3] * P2, 31) * P1;
        p += 32;
        n -= 32;
    }
    h = rotl64(a, 1) + rotl64(b, 7) + rotl64(c, 12) + rotl64(d, 18) + n;
    while (n >= 8) {
        uint64_t w;
        memcpy(&w, p, 8);
        h = rotl64(h ^ (rotl64(w * P2, 31) * P1), 27) * P1 + P2;
        p += 8;
        n -= 8;
    }
    while (n--) {
        h ^= (uint64_t)(*p++) * P1;
        h = rotl64(h, 11) * P2;
    }
    h ^= h >> 33;
    h *= P2;
    h ^= h >> 29;
    return h;
}

static VkDeviceSize align_up(VkDeviceSize v, VkDeviceSize a)
{
    return (v + a - 1) / a * a;
}

/* ---- memory, buffers, images -------------------------------------------- */

static int mem_type(uint32_t bits, VkMemoryPropertyFlags want)
{
    uint32_t i;
    for (i = 0; i < V.mem.memoryTypeCount; i++)
        if ((bits & (1u << i))
            && (V.mem.memoryTypes[i].propertyFlags & want) == want)
            return (int)i;
    return -1;
}

static int mem_alloc(const VkMemoryRequirements *req,
                     const VkMemoryPropertyFlags *prefs, int nprefs,
                     VkDeviceMemory *out, VkMemoryPropertyFlags *got)
{
    VkMemoryAllocateInfo ai;
    int i, t = -1;

    for (i = 0; i < nprefs && t < 0; i++)
        t = mem_type(req->memoryTypeBits, prefs[i]);
    if (t < 0) {
        nvk_fail("finding a memory type", VK_ERROR_OUT_OF_DEVICE_MEMORY);
        return 0;
    }
    memset(&ai, 0, sizeof ai);
    ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    ai.allocationSize = req->size;
    ai.memoryTypeIndex = (uint32_t)t;
    if (got)
        *got = V.mem.memoryTypes[t].propertyFlags;
    return ck(vk.AllocateMemory(V.dev, &ai, NULL, out), "vkAllocateMemory");
}

static void buf_destroy(NvkBuf *b)
{
    if (b->map)
        vk.UnmapMemory(V.dev, b->mem);
    if (b->buf)
        vk.DestroyBuffer(V.dev, b->buf, NULL);
    if (b->mem)
        vk.FreeMemory(V.dev, b->mem, NULL);
    memset(b, 0, sizeof *b);
}

/* A host-visible buffer, mapped for its lifetime. `readback`: prefer cached
 * memory (the CPU reads it). */
static int buf_create(NvkBuf *b, VkDeviceSize size, VkBufferUsageFlags usage,
                      int readback)
{
    static const VkMemoryPropertyFlags up[] = {
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
    };
    static const VkMemoryPropertyFlags down[] = {
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_CACHED_BIT
            | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_CACHED_BIT,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
    };
    VkBufferCreateInfo ci;
    VkMemoryRequirements req;
    VkMemoryPropertyFlags got = 0;
    void *map = NULL;

    memset(b, 0, sizeof *b);
    memset(&ci, 0, sizeof ci);
    ci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    ci.size = size;
    ci.usage = usage;
    ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (!ck(vk.CreateBuffer(V.dev, &ci, NULL, &b->buf), "vkCreateBuffer"))
        return 0;
    vk.GetBufferMemoryRequirements(V.dev, b->buf, &req);
    if (!mem_alloc(&req, readback ? down : up, readback ? 3 : 1, &b->mem, &got))
        return 0;
    if (!ck(vk.BindBufferMemory(V.dev, b->buf, b->mem, 0), "vkBindBufferMemory"))
        return 0;
    if (!ck(vk.MapMemory(V.dev, b->mem, 0, VK_WHOLE_SIZE, 0, &map), "vkMapMemory"))
        return 0;
    b->map = (uint8_t *)map;
    b->size = size;
    b->coherent = (got & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) != 0;
    return 1;
}

static int ring_take(NvkBuf *b, VkDeviceSize n, VkDeviceSize align, VkDeviceSize *off)
{
    VkDeviceSize o = align_up(b->used, align);
    if (o + n > b->size)
        return 0;
    *off = o;
    b->used = o + n;
    return 1;
}

static void img_destroy_now(NvkImage *im)
{
    if (im->view)
        vk.DestroyImageView(V.dev, im->view, NULL);
    if (im->img)
        vk.DestroyImage(V.dev, im->img, NULL);
    if (im->mem)
        vk.FreeMemory(V.dev, im->mem, NULL);
    memset(im, 0, sizeof *im);
}

/* Destroyed once the GPU is done with whatever was recorded so far. */
static void img_destroy_later(NvkImage *im)
{
    if (!im->img)
        return;
    if (!V.recording) {
        img_destroy_now(im);
        return;
    }
    if (V.dead_img_n == V.dead_img_cap) {
        int cap = V.dead_img_cap ? V.dead_img_cap * 2 : 16;
        NvkImage *n = (NvkImage *)realloc(V.dead_img, (size_t)cap * sizeof *n);
        if (!n) {                           /* out of memory: wait instead */
            dev_idle();
            img_destroy_now(im);
            return;
        }
        V.dead_img = n;
        V.dead_img_cap = cap;
    }
    V.dead_img[V.dead_img_n++] = *im;
    memset(im, 0, sizeof *im);
}

static void fb_destroy_later(VkFramebuffer fb)
{
    if (!fb)
        return;
    if (!V.recording) {
        vk.DestroyFramebuffer(V.dev, fb, NULL);
        return;
    }
    if (V.dead_fb_n == V.dead_fb_cap) {
        int cap = V.dead_fb_cap ? V.dead_fb_cap * 2 : 16;
        VkFramebuffer *n = (VkFramebuffer *)realloc(V.dead_fb, (size_t)cap * sizeof *n);
        if (!n) {
            dev_idle();
            vk.DestroyFramebuffer(V.dev, fb, NULL);
            return;
        }
        V.dead_fb = n;
        V.dead_fb_cap = cap;
    }
    V.dead_fb[V.dead_fb_n++] = fb;
}

static int img_create(NvkImage *im, uint32_t w, uint32_t h, uint32_t layers,
                      VkFormat fmt, VkImageUsageFlags usage,
                      VkImageAspectFlags aspect, VkImageViewType vt)
{
    static const VkMemoryPropertyFlags prefs[] = {
        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0,
    };
    VkImageCreateInfo ci;
    VkImageViewCreateInfo vi;
    VkMemoryRequirements req;

    memset(im, 0, sizeof *im);
    memset(&ci, 0, sizeof ci);
    ci.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    ci.imageType = VK_IMAGE_TYPE_2D;
    ci.format = fmt;
    ci.extent.width = w;
    ci.extent.height = h;
    ci.extent.depth = 1;
    ci.mipLevels = 1;
    ci.arrayLayers = layers;
    ci.samples = VK_SAMPLE_COUNT_1_BIT;
    ci.tiling = VK_IMAGE_TILING_OPTIMAL;
    ci.usage = usage;
    ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (!ck(vk.CreateImage(V.dev, &ci, NULL, &im->img), "vkCreateImage"))
        return 0;
    vk.GetImageMemoryRequirements(V.dev, im->img, &req);
    if (!mem_alloc(&req, prefs, 2, &im->mem, NULL))
        return 0;
    if (!ck(vk.BindImageMemory(V.dev, im->img, im->mem, 0), "vkBindImageMemory"))
        return 0;
    memset(&vi, 0, sizeof vi);
    vi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    vi.image = im->img;
    vi.viewType = vt;
    vi.format = fmt;
    vi.subresourceRange.aspectMask = aspect;
    vi.subresourceRange.levelCount = 1;
    vi.subresourceRange.layerCount = layers;
    if (!ck(vk.CreateImageView(V.dev, &vi, NULL, &im->view), "vkCreateImageView"))
        return 0;
    im->layout = VK_IMAGE_LAYOUT_UNDEFINED;
    im->aspect = aspect;
    im->w = w;
    im->h = h;
    im->layers = layers;
    return 1;
}

/* ---- command recording ---------------------------------------------------- */

static int cmd_begin(void)
{
    VkCommandBufferBeginInfo bi;

    if (V.recording)
        return 1;
    memset(&bi, 0, sizeof bi);
    bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (!ck(vk.BeginCommandBuffer(V.cmd, &bi), "vkBeginCommandBuffer"))
        return 0;
    V.recording = 1;
    return 1;
}

static void pass_end(void)
{
    if (!V.in_pass)
        return;
    vk.CmdEndRenderPass(V.cmd);
    V.in_pass = 0;
    V.pass_surf = NULL;
    V.pass_depth = NULL;
}

/* Submit what is recorded and wait for it. Afterwards the rings and the
 * descriptor pool are empty again (V.epoch counts that). */
static int submit_wait(void)
{
    VkSubmitInfo si;
    int i;

    if (!V.ok)
        return 0;
    if (V.recording) {
        pass_end();
        if (!ck(vk.EndCommandBuffer(V.cmd), "vkEndCommandBuffer"))
            return 0;
        memset(&si, 0, sizeof si);
        si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        si.commandBufferCount = 1;
        si.pCommandBuffers = &V.cmd;
        if (!ck(q_submit(&si, V.fence), "vkQueueSubmit"))
            return 0;
        if (!ck(vk.WaitForFences(V.dev, 1, &V.fence, VK_TRUE, UINT64_MAX),
                "vkWaitForFences"))
            return 0;
        vk.ResetFences(V.dev, 1, &V.fence);
        vk.ResetCommandPool(V.dev, V.cpool, 0);
        V.recording = 0;
        V.st.submits++;
    }
    V.vring.used = V.uring.used = V.staging.used = 0;
    vk.ResetDescriptorPool(V.dev, V.dpool, 0);
    V.epoch++;
    for (i = 0; i < V.dead_img_n; i++)
        img_destroy_now(&V.dead_img[i]);
    V.dead_img_n = 0;
    for (i = 0; i < V.dead_fb_n; i++)
        vk.DestroyFramebuffer(V.dev, V.dead_fb[i], NULL);
    V.dead_fb_n = 0;
    return 1;
}

static void layout_masks(VkImageLayout l, VkPipelineStageFlags *stage,
                         VkAccessFlags *access)
{
    switch (l) {
    case VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL:
        *stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
        *access = VK_ACCESS_TRANSFER_WRITE_BIT;
        break;
    case VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL:
        *stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
        *access = VK_ACCESS_TRANSFER_READ_BIT;
        break;
    case VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL:
        *stage = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
        *access = VK_ACCESS_SHADER_READ_BIT;
        break;
    case VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL:
        *stage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        *access = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT
                | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        break;
    case VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL:
        *stage = VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT
               | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
        *access = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT
                | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
        break;
    default:                                /* UNDEFINED */
        *stage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
        *access = 0;
        break;
    }
}

/* Outside a render pass only. */
static void img_barrier(NvkImage *im, VkImageLayout to)
{
    VkImageMemoryBarrier b;
    VkPipelineStageFlags ss, ds;

    if (im->layout == to)
        return;
    memset(&b, 0, sizeof b);
    b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    layout_masks(im->layout, &ss, &b.srcAccessMask);
    layout_masks(to, &ds, &b.dstAccessMask);
    b.oldLayout = im->layout;
    b.newLayout = to;
    b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = im->img;
    b.subresourceRange.aspectMask = im->aspect;
    b.subresourceRange.levelCount = 1;
    b.subresourceRange.layerCount = im->layers;
    vk.CmdPipelineBarrier(V.cmd, ss, ds, 0, 0, NULL, 0, NULL, 1, &b);
    im->layout = to;
}

/* Upload space, outside a render pass. May submit (and so end the pass). */
static uint8_t *staging_alloc(VkDeviceSize n, VkDeviceSize *off)
{
    if (ring_take(&V.staging, n, 16, off))
        return V.staging.map + *off;
    if (!submit_wait())
        return NULL;
    if (n > V.staging.size) {
        buf_destroy(&V.staging);
        if (!buf_create(&V.staging, align_up(n, 1u << 20),
                        VK_BUFFER_USAGE_TRANSFER_SRC_BIT, 0))
            return NULL;
    }
    if (!ring_take(&V.staging, n, 16, off))
        return NULL;
    return V.staging.map + *off;
}

/* ---- textures ------------------------------------------------------------- */

static NvkTex **tex_bucket(uint32_t offset, uint32_t color)
{
    return &V.tex[((offset >> 7) ^ (offset >> 19) ^ (color * 0x9Eu))
                  & (NVK_TEX_BUCKETS - 1)];
}

static void tex_invalidate_range(uint32_t a, uint32_t n)
{
    int i;
    NvkTex *t;
    for (i = 0; i < NVK_TEX_BUCKETS; i++)
        for (t = V.tex[i]; t; t = t->next)
            if (overlap(t->offset, t->bytes, a, n))
                t->validated = 0;
}

static int surf_readback(NvkSurf **list, int n);

static int tex_upload(NvkTex *t, const Nv2aStage *st)
{
    uint32_t layers = t->cube ? 6 : 1, f;
    VkDeviceSize face = (VkDeviceSize)t->width * t->height * 4, off;
    VkBufferImageCopy c;
    uint8_t *dst;

    pass_end();
    dst = staging_alloc(face * layers, &off);
    if (!dst)
        return 0;
    for (f = 0; f < layers; f++)
        if (!nv2a_backend_decode_stage(st, f, (uint32_t *)(dst + f * face))) {
            t->bad = 1;                     /* a format the decoder lacks */
            return 1;
        }
    if (!cmd_begin())
        return 0;
    img_barrier(&t->im, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    memset(&c, 0, sizeof c);
    c.bufferOffset = off;
    c.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    c.imageSubresource.layerCount = layers;
    c.imageExtent.width = t->width;
    c.imageExtent.height = t->height;
    c.imageExtent.depth = 1;
    vk.CmdCopyBufferToImage(V.cmd, V.staging.buf, t->im.img,
                            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &c);
    img_barrier(&t->im, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    V.st.texture_uploads++;
    V.st.texture_bytes += face * layers;
    return 1;
}

/* The image for a stage, decoded and current; NULL reads white (an
 * unusable stage, as rc_texel reads one). [tva, tva+tspan) is the surface
 * this batch draws into: a texture there is re-checked every batch. */
static NvkTex *tex_get(const Nv2aStage *st, uint32_t tva, uint32_t tspan)
{
    NvkTex **head, *t;
    uint32_t bytes;

    if (!st->valid || !st->tex.width || !st->tex.height
        || st->tex.width > NVK_MAX_DIM || st->tex.height > NVK_MAX_DIM)
        return NULL;
    bytes = nv2a_backend_stage_bytes(st);
    if (!bytes)
        return NULL;
    head = tex_bucket(st->tex.offset, st->tex.color);
    for (t = *head; t; t = t->next)
        if (t->offset == st->tex.offset && t->color == st->tex.color
            && t->width == st->tex.width && t->height == st->tex.height
            && t->pitch == st->tex.pitch && t->palette == st->palette
            && t->cube == st->cube && t->face_stride == st->face_stride)
            break;
    if (!t) {
        t = (NvkTex *)calloc(1, sizeof *t);
        if (!t)
            return NULL;
        t->offset = st->tex.offset;
        t->color = st->tex.color;
        t->width = st->tex.width;
        t->height = st->tex.height;
        t->pitch = st->tex.pitch;
        t->palette = st->palette;
        t->cube = st->cube;
        t->face_stride = st->face_stride;
        t->bytes = bytes;
        if (!img_create(&t->im, t->width, t->height, st->cube ? 6 : 1,
                        VK_FORMAT_B8G8R8A8_UNORM,
                        VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                        VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_VIEW_TYPE_2D_ARRAY)) {
            img_destroy_now(&t->im);
            free(t);
            return NULL;
        }
        t->fresh = 1;
        t->next = *head;
        *head = t;
    }
    t->last_use = V.frame;
    if (!t->validated) {
        NvkSurf *list[NVK_MAX_SURF];
        int i, n = 0;
        uint64_t h;

        /* Render to texture: what the GPU drew there goes to memory first. */
        for (i = 0; i < NVK_MAX_SURF; i++)
            if (V.surf[i].used && V.surf[i].dirty
                && overlap(V.surf[i].va, V.surf[i].span, t->offset, t->bytes))
                list[n++] = &V.surf[i];
        if (n && !surf_readback(list, n))
            return NULL;
        h = nvk_hash(guest(t->offset), t->bytes, 0);
        if (t->palette)
            h = nvk_hash(guest(t->palette), 256 * 4, h);
        if (t->fresh || h != t->hash) {
            t->bad = 0;
            if (!tex_upload(t, st))
                return NULL;
            t->hash = h;
            t->fresh = 0;
        }
        t->validated = !overlap(tva, tspan, t->offset, t->bytes);
    }
    return t->bad ? NULL : t;
}

static void tex_free_all(void)
{
    int i;
    for (i = 0; i < NVK_TEX_BUCKETS; i++) {
        while (V.tex[i]) {
            NvkTex *t = V.tex[i];
            V.tex[i] = t->next;
            img_destroy_now(&t->im);
            free(t);
        }
    }
}

/* At a frame boundary, with the GPU idle: textures unused for a while go. */
static void tex_frame(void)
{
    int i;
    for (i = 0; i < NVK_TEX_BUCKETS; i++) {
        NvkTex **pp = &V.tex[i];
        while (*pp) {
            NvkTex *t = *pp;
            t->validated = 0;
            if (t->last_use + NVK_TEX_KEEP < V.frame) {
                *pp = t->next;
                img_destroy_now(&t->im);
                free(t);
                continue;
            }
            pp = &t->next;
        }
    }
}

static VkSampler sampler_get(int linear, uint32_t au, uint32_t av)
{
    int iu = au == 1 ? 0 : au == 2 ? 1 : 2, iv = av == 1 ? 0 : av == 2 ? 1 : 2;
    static const VkSamplerAddressMode modes[3] = {
        VK_SAMPLER_ADDRESS_MODE_REPEAT, VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT,
        VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
    };
    VkSamplerCreateInfo ci;
    VkSampler *s = &V.samplers[linear ? 1 : 0][iu][iv];

    if (*s)
        return *s;
    memset(&ci, 0, sizeof ci);
    ci.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    ci.magFilter = ci.minFilter = linear ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
    ci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    ci.addressModeU = modes[iu];
    ci.addressModeV = modes[iv];
    ci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    ci.maxLod = 0.0f;
    ci.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE;
    if (!ck(vk.CreateSampler(V.dev, &ci, NULL, s), "vkCreateSampler"))
        return VK_NULL_HANDLE;
    return *s;
}

/* ---- colour surfaces ------------------------------------------------------ */

/* Guest pixel (x, y) of a surface, as put_pixel addresses it. */
static uint8_t *surf_px(const NvkSurf *s, uint8_t *base, uint32_t mx, uint32_t my,
                        uint32_t x, uint32_t y)
{
    if (s->swizzled)
        return base + (size_t)(swizzle_deposit(x, mx) | swizzle_deposit(y, my)) * s->bpp;
    return base + (size_t)y * s->pitch + (size_t)x * s->bpp;
}

static int surf_upload(NvkSurf *s)
{
    VkDeviceSize n = (VkDeviceSize)s->im.w * s->im.h * 4, off;
    VkBufferImageCopy c;
    uint8_t *dst, *g = guest(s->va);
    uint32_t x, y, mx = 0, my = 0;

    pass_end();
    dst = staging_alloc(n, &off);
    if (!dst || !cmd_begin())
        return 0;
    if (s->swizzled)
        xbox_swizzle_masks(s->im.w, s->im.h, &mx, &my);
    if (!s->swizzled && s->bpp == 4) {
        for (y = 0; y < s->im.h; y++)
            memcpy(dst + (size_t)y * s->im.w * 4, g + (size_t)y * s->pitch,
                   (size_t)s->im.w * 4);
    } else {
        uint32_t *o = (uint32_t *)dst;
        for (y = 0; y < s->im.h; y++)
            for (x = 0; x < s->im.w; x++) {
                const uint8_t *p = surf_px(s, g, mx, my, x, y);
                uint32_t v;
                if (s->bpp == 4) {
                    memcpy(&v, p, 4);
                } else {                    /* 5:6:5 widened as put_pixel reads it */
                    uint16_t t;
                    memcpy(&t, p, 2);
                    v = 0xFF000000u | ((uint32_t)(t & 0xF800u) << 8)
                      | ((uint32_t)(t & 0x07E0u) << 5) | ((uint32_t)(t & 0x001Fu) << 3);
                }
                o[(size_t)y * s->im.w + x] = v;
            }
    }
    img_barrier(&s->im, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    memset(&c, 0, sizeof c);
    c.bufferOffset = off;
    c.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    c.imageSubresource.layerCount = 1;
    c.imageExtent.width = s->im.w;
    c.imageExtent.height = s->im.h;
    c.imageExtent.depth = 1;
    vk.CmdCopyBufferToImage(V.cmd, V.staging.buf, s->im.img,
                            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &c);
    img_barrier(&s->im, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    V.st.surface_uploads++;
    return 1;
}

/* Write what the GPU drew in these surfaces back to guest memory. */
static int surf_readback(NvkSurf **list, int n)
{
    VkDeviceSize total = 0, off[NVK_MAX_SURF];
    int i;

    if (n <= 0)
        return 1;
    for (i = 0; i < n; i++) {
        off[i] = total;
        total += align_up((VkDeviceSize)(list[i]->dx1 - list[i]->dx0)
                          * (list[i]->dy1 - list[i]->dy0) * 4, 16);
    }
    pass_end();
    if (total > V.rbuf.size) {
        if (!submit_wait())                 /* nothing may still use it */
            return 0;
        buf_destroy(&V.rbuf);
        if (!buf_create(&V.rbuf, align_up(total, 1u << 20),
                        VK_BUFFER_USAGE_TRANSFER_DST_BIT, 1))
            return 0;
    }
    if (!cmd_begin())
        return 0;
    for (i = 0; i < n; i++) {
        NvkSurf *s = list[i];
        VkBufferImageCopy c;
        img_barrier(&s->im, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        memset(&c, 0, sizeof c);
        c.bufferOffset = off[i];
        c.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        c.imageSubresource.layerCount = 1;
        c.imageOffset.x = (int32_t)s->dx0;
        c.imageOffset.y = (int32_t)s->dy0;
        c.imageExtent.width = s->dx1 - s->dx0;
        c.imageExtent.height = s->dy1 - s->dy0;
        c.imageExtent.depth = 1;
        vk.CmdCopyImageToBuffer(V.cmd, s->im.img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                V.rbuf.buf, 1, &c);
        img_barrier(&s->im, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    }
    if (!submit_wait())
        return 0;
    if (!V.rbuf.coherent) {
        VkMappedMemoryRange r;
        memset(&r, 0, sizeof r);
        r.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
        r.memory = V.rbuf.mem;
        r.size = VK_WHOLE_SIZE;
        vk.InvalidateMappedMemoryRanges(V.dev, 1, &r);
    }
    for (i = 0; i < n; i++) {
        NvkSurf *s = list[i];
        const uint32_t *src = (const uint32_t *)(V.rbuf.map + off[i]);
        uint32_t w = s->dx1 - s->dx0, x, y, mx = 0, my = 0, j;
        uint8_t *g = guest(s->va);

        if (s->swizzled)
            xbox_swizzle_masks(s->im.w, s->im.h, &mx, &my);
        for (y = s->dy0; y < s->dy1; y++) {
            const uint32_t *row = src + (size_t)(y - s->dy0) * w;
            if (!s->swizzled && s->bpp == 4) {
                memcpy(g + (size_t)y * s->pitch + (size_t)s->dx0 * 4, row, (size_t)w * 4);
                continue;
            }
            for (x = s->dx0; x < s->dx1; x++) {
                uint32_t v = row[x - s->dx0];
                uint8_t *p = surf_px(s, g, mx, my, x, y);
                if (s->bpp == 4) {
                    memcpy(p, &v, 4);
                } else {                    /* put_pixel's truncation */
                    uint16_t t = (uint16_t)(((v >> 8) & 0xF800u) | ((v >> 5) & 0x07E0u)
                                          | ((v >> 3) & 0x001Fu));
                    memcpy(p, &t, 2);
                }
            }
        }
        V.st.readbacks++;
        V.st.readback_bytes += (uint64_t)w * (s->dy1 - s->dy0) * s->bpp;
        s->dirty = 0;
        s->dx0 = s->dy0 = s->dx1 = s->dy1 = 0;
        s->hash = nvk_hash(g, s->span, 0);
        s->hash_ok = 1;
        /* Whatever else shadows these bytes must look again. */
        for (j = 0; j < NVK_MAX_SURF; j++)
            if (&V.surf[j] != s && V.surf[j].used
                && overlap(V.surf[j].va, V.surf[j].span, s->va, s->span))
                V.surf[j].validated = 0;
        tex_invalidate_range(s->va, s->span);
    }
    return 1;
}

static void surf_drop_fbs(NvkSurf *s)
{
    int i;
    for (i = 0; i < NVK_FB_PER_SURF; i++) {
        fb_destroy_later(s->fbs[i].fb);
        memset(&s->fbs[i], 0, sizeof s->fbs[i]);
    }
}

static void surf_release(NvkSurf *s)
{
    if (V.pass_surf == s)
        pass_end();
    surf_drop_fbs(s);
    img_destroy_later(&s->im);
    memset(s, 0, sizeof *s);
}

static int surf_alloc_image(NvkSurf *s, uint32_t w, uint32_t h)
{
    if (!img_create(&s->im, w, h, 1, VK_FORMAT_B8G8R8A8_UNORM,
                    VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT
                        | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                    VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_VIEW_TYPE_2D))
        return 0;
    s->span = s->swizzled ? w * h * s->bpp : s->pitch * h;
    s->hash_ok = 0;
    s->validated = 0;
    return 1;
}

/* The surface object for this target, its image at least `need_h` rows
 * (linear) -- created, grown or reused. NULL: nothing to draw into. */
static NvkSurf *surf_get(const Nv2aSurfaceEx *sx, uint32_t need_h)
{
    uint32_t bpp = sx->base.bytes_per_pixel, va = sx->base.color_va;
    uint32_t pitch, w, h;
    NvkSurf *s = NULL, *victim = NULL;
    int i;

    if (sx->swizzled) {
        w = sx->swz_width;
        h = sx->swz_height;
        pitch = w * bpp;
    } else {
        pitch = sx->base.pitch;
        w = pitch / bpp;
        h = need_h;
    }
    if (!w || !h || w > NVK_MAX_DIM || h > NVK_MAX_DIM)
        return NULL;
    for (i = 0; i < NVK_MAX_SURF; i++) {
        NvkSurf *c = &V.surf[i];
        if (c->used && c->va == va && c->bpp == bpp && c->pitch == pitch
            && c->swizzled == sx->swizzled && (!c->swizzled || c->im.h == h)) {
            s = c;
            break;
        }
    }
    if (s && s->im.h < h) {                 /* grown: keep what it holds */
        NvkSurf keep;
        if (s->dirty && !surf_readback(&s, 1))
            return NULL;
        keep = *s;
        surf_release(s);
        s->used = 1;
        s->va = keep.va;
        s->pitch = keep.pitch;
        s->bpp = keep.bpp;
        s->swizzled = keep.swizzled;
        if (!surf_alloc_image(s, w, h))
            return NULL;
    }
    if (!s) {
        for (i = 0; i < NVK_MAX_SURF; i++) {
            NvkSurf *c = &V.surf[i];
            if (!c->used) {
                victim = c;
                break;
            }
            if (c != V.pass_surf && (!victim || c->last_use < victim->last_use))
                victim = c;
        }
        if (!victim)
            return NULL;
        if (victim->used) {
            if (victim->dirty && !surf_readback(&victim, 1))
                return NULL;
            surf_release(victim);
        }
        s = victim;
        s->used = 1;
        s->va = va;
        s->pitch = pitch;
        s->bpp = bpp;
        s->swizzled = sx->swizzled;
        if (!surf_alloc_image(s, w, h)) {
            s->used = 0;
            return NULL;
        }
    }
    s->last_use = ++V.clock;
    return s;
}

/* Make `s` current against guest memory before it is drawn into. */
static int surf_prepare(NvkSurf *s)
{
    if (V.pass_surf != s) {
        NvkSurf *list[NVK_MAX_SURF];
        int i, n = 0;
        for (i = 0; i < NVK_MAX_SURF; i++) {
            NvkSurf *t = &V.surf[i];
            if (t != s && t->used && t->dirty && overlap(t->va, t->span, s->va, s->span))
                list[n++] = t;
        }
        if (n) {
            if (!surf_readback(list, n))
                return 0;
            s->validated = 0;
        }
        tex_invalidate_range(s->va, s->span);
    }
    if (!s->validated) {
        uint64_t h = nvk_hash(guest(s->va), s->span, 0);
        if (!s->hash_ok || h != s->hash) {
            if (s->dirty) {
                /* Cannot happen by construction (a surface is written back
                 * before anything else aliasing it is drawn); if it does,
                 * the GPU's newer pixels win. */
                static int once;
                if (!once++)
                    nvk_log("surface 0x%08X changed in memory while drawn on the GPU;"
                            " keeping the GPU's pixels", s->va);
            } else if (!surf_upload(s)) {
                return 0;
            }
            s->hash = h;
            s->hash_ok = 1;
        }
        s->validated = 1;
    }
    return 1;
}

static int surf_sync_all(void)
{
    NvkSurf *list[NVK_MAX_SURF];
    int i, n = 0;
    for (i = 0; i < NVK_MAX_SURF; i++)
        if (V.surf[i].used && V.surf[i].dirty)
            list[n++] = &V.surf[i];
    return surf_readback(list, n);
}

static void surf_mark(NvkSurf *s, const VkRect2D *r)
{
    uint32_t x0 = (uint32_t)r->offset.x, y0 = (uint32_t)r->offset.y;
    uint32_t x1 = x0 + r->extent.width, y1 = y0 + r->extent.height;
    if (!s->dirty) {
        s->dx0 = x0; s->dy0 = y0; s->dx1 = x1; s->dy1 = y1;
        s->dirty = 1;
        return;
    }
    if (x0 < s->dx0) s->dx0 = x0;
    if (y0 < s->dy0) s->dy0 = y0;
    if (x1 > s->dx1) s->dx1 = x1;
    if (y1 > s->dy1) s->dy1 = y1;
}

/* ---- depth ---------------------------------------------------------------- */

static int depth_init(NvkDepth *d, uint32_t w, uint32_t h)
{
    VkClearDepthStencilValue cv;
    VkImageSubresourceRange r;

    if (!img_create(&d->im, w, h, 1, V.depth_fmt,
                    VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT
                        | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                    VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT,
                    VK_IMAGE_VIEW_TYPE_2D))
        return 0;
    d->gen++;
    pass_end();
    if (!cmd_begin())
        return 0;
    /* Far, as the CPU path's zbuf_current fills a new buffer. */
    img_barrier(&d->im, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    cv.depth = 1.0f;
    cv.stencil = 0;
    memset(&r, 0, sizeof r);
    r.aspectMask = d->im.aspect;
    r.levelCount = 1;
    r.layerCount = 1;
    vk.CmdClearDepthStencilImage(V.cmd, d->im.img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                 &cv, 1, &r);
    img_barrier(&d->im, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);
    return 1;
}

static NvkDepth *depth_get(uint32_t zva, uint32_t w, uint32_t h)
{
    NvkDepth *d = NULL;
    int i;

    for (i = 0; i < NVK_MAX_DEPTH; i++)
        if (V.depth[i].used && V.depth[i].va == zva) {
            d = &V.depth[i];
            break;
        }
    if (d && (d->im.w < w || d->im.h < h)) {
        uint32_t nw = d->im.w > w ? d->im.w : w, nh = d->im.h > h ? d->im.h : h;
        if (V.pass_depth == d)
            pass_end();
        img_destroy_later(&d->im);
        if (!depth_init(d, nw, nh))
            return NULL;
    }
    if (!d) {
        for (i = 0; i < NVK_MAX_DEPTH; i++)
            if (!V.depth[i].used) {
                d = &V.depth[i];
                break;
            }
        if (!d) {
            d = &V.depth[V.depth_next++ % NVK_MAX_DEPTH];
            if (d == V.pass_depth)
                d = &V.depth[V.depth_next++ % NVK_MAX_DEPTH];
            if (V.pass_depth == d)
                pass_end();
            img_destroy_later(&d->im);
        }
        d->used = 1;
        d->va = zva;
        if (!depth_init(d, w, h))
            return NULL;
    }
    return d;
}

/* ---- render passes -------------------------------------------------------- */

static VkFramebuffer fb_get(NvkSurf *s, NvkDepth *d)
{
    VkFramebufferCreateInfo ci;
    VkImageView att[2];
    NvkFb *f;
    int i;

    for (i = 0; i < NVK_FB_PER_SURF; i++)
        if (s->fbs[i].fb && s->fbs[i].d == d && s->fbs[i].gen == d->gen)
            return s->fbs[i].fb;
    f = &s->fbs[s->fb_next++ % NVK_FB_PER_SURF];
    fb_destroy_later(f->fb);
    memset(f, 0, sizeof *f);
    att[0] = s->im.view;
    att[1] = d->im.view;
    memset(&ci, 0, sizeof ci);
    ci.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
    ci.renderPass = V.rp;
    ci.attachmentCount = 2;
    ci.pAttachments = att;
    ci.width = s->im.w;
    ci.height = s->im.h;
    ci.layers = 1;
    if (!ck(vk.CreateFramebuffer(V.dev, &ci, NULL, &f->fb), "vkCreateFramebuffer"))
        return VK_NULL_HANDLE;
    f->d = d;
    f->gen = d->gen;
    return f->fb;
}

static int pass_begin(NvkSurf *s, NvkDepth *d)
{
    VkRenderPassBeginInfo bi;
    VkFramebuffer fb;

    if (V.in_pass && V.pass_surf == s && V.pass_depth == d)
        return 1;
    pass_end();
    fb = fb_get(s, d);
    if (!fb || !cmd_begin())
        return 0;
    img_barrier(&s->im, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    img_barrier(&d->im, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);
    memset(&bi, 0, sizeof bi);
    bi.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    bi.renderPass = V.rp;
    bi.framebuffer = fb;
    bi.renderArea.extent.width = s->im.w;
    bi.renderArea.extent.height = s->im.h;
    vk.CmdBeginRenderPass(V.cmd, &bi, VK_SUBPASS_CONTENTS_INLINE);
    V.in_pass = 1;
    V.pass_surf = s;
    V.pass_depth = d;
    V.st.render_passes++;
    return 1;
}

/* ---- NV2A state to Vulkan --------------------------------------------------- */

static uint32_t vk_compare(uint32_t gl)
{
    /* GL's NEVER..ALWAYS (0x200..0x207) are Vulkan's 0..7 in the same order;
     * anything else passes, as depth_pass treats it. */
    return gl >= 0x200 && gl <= 0x207 ? gl - 0x200 : VK_COMPARE_OP_ALWAYS;
}

static uint32_t vk_blend_factor(uint32_t f)
{
    switch (f) {
    case 0x0000: return VK_BLEND_FACTOR_ZERO;
    case 0x0001: return VK_BLEND_FACTOR_ONE;
    case 0x0300: return VK_BLEND_FACTOR_SRC_COLOR;
    case 0x0301: return VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
    case 0x0302: return VK_BLEND_FACTOR_SRC_ALPHA;
    case 0x0303: return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    case 0x0304: return VK_BLEND_FACTOR_DST_ALPHA;
    case 0x0305: return VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
    case 0x0306: return VK_BLEND_FACTOR_DST_COLOR;
    case 0x0307: return VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR;
    case 0x0308: return VK_BLEND_FACTOR_SRC_ALPHA_SATURATE;
    case 0x8001: return VK_BLEND_FACTOR_CONSTANT_COLOR;
    case 0x8002: return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR;
    case 0x8003: return VK_BLEND_FACTOR_CONSTANT_ALPHA;
    case 0x8004: return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_ALPHA;
    default:     return f ? VK_BLEND_FACTOR_ONE : VK_BLEND_FACTOR_ZERO;  /* blend_factor */
    }
}

static uint32_t vk_blend_op(uint32_t eq)
{
    switch (eq) {
    case 0x800A: return VK_BLEND_OP_SUBTRACT;
    case 0x800B: return VK_BLEND_OP_REVERSE_SUBTRACT;
    case 0x8007: return VK_BLEND_OP_MIN;
    case 0x8008: return VK_BLEND_OP_MAX;
    default:     return VK_BLEND_OP_ADD;
    }
}

static uint32_t vk_stencil_op(uint32_t op)
{
    switch (op) {
    case 0x0000: return VK_STENCIL_OP_ZERO;
    case 0x1E01: return VK_STENCIL_OP_REPLACE;
    case 0x1E02: return VK_STENCIL_OP_INCREMENT_AND_CLAMP;
    case 0x1E03: return VK_STENCIL_OP_DECREMENT_AND_CLAMP;
    case 0x150A: return VK_STENCIL_OP_INVERT;
    case 0x8507: return VK_STENCIL_OP_INCREMENT_AND_WRAP;
    case 0x8508: return VK_STENCIL_OP_DECREMENT_AND_WRAP;
    default:     return VK_STENCIL_OP_KEEP;   /* 0x1E00 KEEP */
    }
}

static VkPipeline pipe_create(const NvkPipeKey *k)
{
    VkSpecializationMapEntry me[NVK_NSPEC];
    VkSpecializationInfo si;
    VkPipelineShaderStageCreateInfo ss[2];
    VkVertexInputBindingDescription vb;
    VkVertexInputAttributeDescription va[8];
    VkPipelineVertexInputStateCreateInfo vi;
    VkPipelineInputAssemblyStateCreateInfo ia;
    VkPipelineViewportStateCreateInfo vp;
    VkPipelineRasterizationStateCreateInfo rs;
    VkPipelineMultisampleStateCreateInfo ms;
    VkPipelineDepthStencilStateCreateInfo ds;
    VkPipelineColorBlendAttachmentState ba;
    VkPipelineColorBlendStateCreateInfo cb;
    static const VkDynamicState dyn[] = {
        VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR,
        VK_DYNAMIC_STATE_BLEND_CONSTANTS, VK_DYNAMIC_STATE_STENCIL_COMPARE_MASK,
        VK_DYNAMIC_STATE_STENCIL_WRITE_MASK, VK_DYNAMIC_STATE_STENCIL_REFERENCE,
    };
    VkPipelineDynamicStateCreateInfo dy;
    VkGraphicsPipelineCreateInfo ci;
    VkPipeline p = VK_NULL_HANDLE;
    uint64_t t0 = now_us(), dt;
    int i;

    for (i = 0; i < NVK_NSPEC; i++) {
        me[i].constantID = (uint32_t)i;
        me[i].offset = (uint32_t)(i * 4);
        me[i].size = 4;
    }
    si.mapEntryCount = NVK_NSPEC;
    si.pMapEntries = me;
    si.dataSize = sizeof k->spec;
    si.pData = k->spec;

    memset(ss, 0, sizeof ss);
    ss[0].sType = ss[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    ss[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    ss[0].module = V.vs;
    ss[0].pName = "main";
    ss[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    ss[1].module = V.fs;
    ss[1].pName = "main";
    ss[1].pSpecializationInfo = &si;

    vb.binding = 0;
    vb.stride = sizeof(Nv2aVertexEx);
    vb.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
    for (i = 0; i < 8; i++) {
        va[i].location = (uint32_t)i;
        va[i].binding = 0;
        va[i].format = VK_FORMAT_R32G32B32A32_SFLOAT;
    }
    va[0].offset = offsetof(Nv2aVertexEx, pos);
    va[1].offset = offsetof(Nv2aVertexEx, d0);
    va[2].offset = offsetof(Nv2aVertexEx, d1);
    va[3].offset = offsetof(Nv2aVertexEx, fog);
    va[3].format = VK_FORMAT_R32_SFLOAT;
    for (i = 0; i < 4; i++)
        va[4 + i].offset = (uint32_t)(offsetof(Nv2aVertexEx, tex) + (size_t)i * 16);

    memset(&vi, 0, sizeof vi);
    vi.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vi.vertexBindingDescriptionCount = 1;
    vi.pVertexBindingDescriptions = &vb;
    vi.vertexAttributeDescriptionCount = 8;
    vi.pVertexAttributeDescriptions = va;

    memset(&ia, 0, sizeof ia);
    ia.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    memset(&vp, 0, sizeof vp);
    vp.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    vp.viewportCount = 1;
    vp.scissorCount = 1;

    memset(&rs, 0, sizeof rs);
    rs.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rs.depthClampEnable = V.depth_clamp ? VK_TRUE : VK_FALSE;
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode = k->cull;
    rs.frontFace = (VkFrontFace)k->front;
    rs.lineWidth = 1.0f;

    memset(&ms, 0, sizeof ms);
    ms.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    memset(&ds, 0, sizeof ds);
    ds.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    ds.depthTestEnable = k->depth[0];
    ds.depthWriteEnable = k->depth[1];
    ds.depthCompareOp = (VkCompareOp)k->depth[2];
    ds.stencilTestEnable = k->stencil[0];
    ds.front.failOp = (VkStencilOp)k->stencil[1];
    ds.front.depthFailOp = (VkStencilOp)k->stencil[2];
    ds.front.passOp = (VkStencilOp)k->stencil[3];
    ds.front.compareOp = (VkCompareOp)k->stencil[4];
    ds.front.compareMask = ds.front.writeMask = 0xFF;      /* dynamic */
    ds.back = ds.front;
    ds.maxDepthBounds = 1.0f;

    memset(&ba, 0, sizeof ba);
    ba.blendEnable = k->blend[0];
    ba.srcColorBlendFactor = ba.srcAlphaBlendFactor = (VkBlendFactor)k->blend[1];
    ba.dstColorBlendFactor = ba.dstAlphaBlendFactor = (VkBlendFactor)k->blend[2];
    ba.colorBlendOp = ba.alphaBlendOp = (VkBlendOp)k->blend[3];
    ba.colorWriteMask = k->write_mask;

    memset(&cb, 0, sizeof cb);
    cb.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    cb.attachmentCount = 1;
    cb.pAttachments = &ba;

    memset(&dy, 0, sizeof dy);
    dy.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dy.dynamicStateCount = (uint32_t)(sizeof dyn / sizeof dyn[0]);
    dy.pDynamicStates = dyn;

    memset(&ci, 0, sizeof ci);
    ci.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    ci.stageCount = 2;
    ci.pStages = ss;
    ci.pVertexInputState = &vi;
    ci.pInputAssemblyState = &ia;
    ci.pViewportState = &vp;
    ci.pRasterizationState = &rs;
    ci.pMultisampleState = &ms;
    ci.pDepthStencilState = &ds;
    ci.pColorBlendState = &cb;
    ci.pDynamicState = &dy;
    ci.layout = V.pl;
    ci.renderPass = V.rp;
    ci.basePipelineIndex = -1;
    if (!ck(vk.CreateGraphicsPipelines(V.dev, V.pcache, 1, &ci, NULL, &p),
            "vkCreateGraphicsPipelines"))
        return VK_NULL_HANDLE;
    dt = now_us() - t0;
    V.st.pipelines++;
    V.st.pipeline_build_us += dt;
    V.pc_dirty = 1;
    V.pc_new_frame = V.frame;
    /* A pipeline is built the first time a combiner program is drawn with,
     * mid-frame: the stutter shader compilation costs on a phone GPU. */
    if (V.stats_every > 0)
        nvk_log("pipeline %llu: %.1f ms (combiner stages %u, stage program 0x%05X)",
                (unsigned long long)V.st.pipelines, (double)dt / 1000.0,
                k->spec[36] ? (k->spec[34] & 0xFF) : 0, k->spec[35]);
    return p;
}

static VkPipeline pipe_get(const NvkPipeKey *k)
{
    uint64_t h = nvk_hash((const uint8_t *)k, sizeof *k, 0x4E56324Bu);
    uint32_t i;

    if (V.pipe_n + 1 > V.pipe_cap / 2) {    /* grow and rehash */
        uint32_t cap = V.pipe_cap ? V.pipe_cap * 2 : 256, j;
        NvkPipe *n = (NvkPipe *)calloc(cap, sizeof *n);
        if (!n)
            return VK_NULL_HANDLE;
        for (j = 0; j < V.pipe_cap; j++)
            if (V.pipes[j].used) {
                uint32_t m = (uint32_t)V.pipes[j].h & (cap - 1);
                while (n[m].used)
                    m = (m + 1) & (cap - 1);
                n[m] = V.pipes[j];
            }
        free(V.pipes);
        V.pipes = n;
        V.pipe_cap = cap;
    }
    for (i = (uint32_t)h & (V.pipe_cap - 1); V.pipes[i].used; i = (i + 1) & (V.pipe_cap - 1))
        if (V.pipes[i].h == h && !memcmp(&V.pipes[i].key, k, sizeof *k))
            return V.pipes[i].pipe;
    V.pipes[i].pipe = pipe_create(k);
    if (!V.pipes[i].pipe)
        return VK_NULL_HANDLE;
    V.pipes[i].used = 1;
    V.pipes[i].h = h;
    V.pipes[i].key = *k;
    V.pipe_n++;
    return V.pipes[i].pipe;
}

/* The filter the CPU path would use for a stage (rc_texel; xf_rows' fixed
 * path ignores the title's filter). */
static int stage_linear(const Nv2aStage *st, int use_rc)
{
    uint32_t mag = (st->tex.filter >> 24) & 0xF, min = (st->tex.filter >> 16) & 0xFF;
    if (V.bilinear)
        return 1;
    if (!use_rc)
        return 0;
    return mag == 2 || min == 2 || min == 4 || min == 6;
}

static void unpack_argb(uint32_t c, float o[4])
{
    nv2a_rc_unpack(c, o);                   /* 0xAARRGGBB -> r g b a */
}

/* ---- the back end ----------------------------------------------------------- */

static uint32_t surface_rows(const Nv2aSurfaceEx *sx)
{
    return sx->swizzled ? sx->swz_height : sx->clip_y + sx->base.height;
}

static uint32_t surface_span(const Nv2aSurfaceEx *sx)
{
    return sx->swizzled ? sx->swz_width * sx->swz_height * sx->base.bytes_per_pixel
                        : sx->base.pitch * surface_rows(sx);
}

/* The rectangle the CPU path writes: the clip, inside the image (and the
 * swizzled surface's size). 0 if empty. */
static int clip_rect(const Nv2aSurfaceEx *sx, const NvkSurf *s, const uint32_t *r,
                     VkRect2D *out)
{
    uint32_t x0, y0, x1, y1;
    if (r) {
        x0 = r[0]; y0 = r[1]; x1 = r[2]; y1 = r[3];
    } else {
        x0 = sx->clip_x; y0 = sx->clip_y;
        x1 = sx->clip_x + sx->base.width; y1 = sx->clip_y + sx->base.height;
    }
    if (x1 > s->im.w) x1 = s->im.w;
    if (y1 > s->im.h) y1 = s->im.h;
    if (x0 >= x1 || y0 >= y1)
        return 0;
    out->offset.x = (int32_t)x0;
    out->offset.y = (int32_t)y0;
    out->extent.width = x1 - x0;
    out->extent.height = y1 - y0;
    return 1;
}

static float zmax_of(uint32_t zeta_format)
{
    return zeta_format == 1 ? 65535.0f : 16777215.0f;
}

static void vk_draw_ex(const Nv2aSurfaceEx *sx, const Nv2aBatchEx *b)
{
    const Nv2aRenderState *rs = b->state;
    uint32_t bpp = sx->base.bytes_per_pixel, tva, tspan, done = 0;
    int use_rc = (int)b->use_rc, textured = (int)b->textured, st;
    int linear[4] = {0, 0, 0, 0};
    NvkTex *tx[4] = {NULL, NULL, NULL, NULL};
    NvkSurf *s;
    NvkDepth *d;
    VkRect2D sc;
    NvkPipeKey k;
    VkPipeline pipe;
    NvkUbo u;
    NvkPush pc;
    VkDescriptorSet set = VK_NULL_HANDLE;
    uint64_t set_epoch = ~(uint64_t)0;
    VkDeviceSize uoff = 0;

    if (!V.ok || !b->count || (bpp != 2 && bpp != 4) || !sx->base.color_va
        || !sx->base.pitch)
        return;
    /* Colour writes off and no alpha test: the CPU path does not shade at
     * all (xf_rows' `shade`), so neither may a texture stage kill a pixel. */
    if (!(rs->color_mask & 0x01010101u) && !rs->alpha_test_enable)
        use_rc = textured = 0;
    tva = sx->base.color_va;
    tspan = surface_span(sx);

    /* 1. Textures (may upload, and read back a surface drawn into one). */
    for (st = 0; st < 4; st++) {
        uint32_t mode = (b->combiner.stage_program >> (st * 5)) & 0x1F;
        int need = use_rc ? (mode >= 1 && mode <= 3) : (st == 0 && textured);
        if (!need)
            continue;
        tx[st] = tex_get(&b->stages[st], tva, tspan);
        if (!V.ok)
            return;
        linear[st] = stage_linear(&b->stages[st], use_rc);
    }
    if (!use_rc && textured && !tx[0])
        textured = 0;                       /* xf_rows keeps the colour */

    /* 2. The surface and its depth. */
    s = surf_get(sx, surface_rows(sx));
    if (!s || !surf_prepare(s))
        return;
    d = depth_get(rs->zeta_va, s->im.w, s->im.h);
    if (!d || !clip_rect(sx, s, NULL, &sc))
        return;

    /* 3. The pipeline. */
    memset(&k, 0, sizeof k);
    if (use_rc) {
        const Nv2aCombiner *rc = &b->combiner;
        uint32_t n = rc->control & 0xFF, i, cube = 0;
        if (n > 8)
            n = 8;
        for (i = 0; i < n; i++) {
            k.spec[i] = rc->color_icw[i];
            k.spec[8 + i] = rc->alpha_icw[i];
            k.spec[16 + i] = rc->color_ocw[i];
            k.spec[24 + i] = rc->alpha_ocw[i];
        }
        k.spec[32] = rc->final0;
        k.spec[33] = rc->final1;
        k.spec[34] = rc->control;
        k.spec[35] = rc->stage_program;
        k.spec[36] = 1;
        for (i = 0; i < 4; i++)
            if (tx[i] && b->stages[i].cube)
                cube |= 1u << i;
        k.spec[38] = cube;
    } else {
        k.spec[37] = (uint32_t)textured;
    }
    k.spec[39] = rs->alpha_test_enable && !b->screen_space ? rs->alpha_func : 0x207;
    if (rs->blend_enable && !(rs->blend_src == 1 && rs->blend_dst == 0)) {
        k.blend[0] = 1;
        k.blend[1] = vk_blend_factor(rs->blend_src);
        k.blend[2] = vk_blend_factor(rs->blend_dst);
        k.blend[3] = vk_blend_op(rs->blend_eq);
    }
    if (rs->color_mask & 0x00010000u) k.write_mask |= VK_COLOR_COMPONENT_R_BIT;
    if (rs->color_mask & 0x00000100u) k.write_mask |= VK_COLOR_COMPONENT_G_BIT;
    if (rs->color_mask & 0x00000001u) k.write_mask |= VK_COLOR_COMPONENT_B_BIT;
    if ((rs->color_mask & 0x01000000u) && bpp == 4)
        k.write_mask |= VK_COLOR_COMPONENT_A_BIT;
    if (b->screen_space) {
        k.depth[2] = VK_COMPARE_OP_ALWAYS;  /* raster_triangle: no depth */
    } else {
        k.depth[0] = rs->depth_test_enable || rs->depth_write;
        k.depth[1] = rs->depth_write ? 1 : 0;
        k.depth[2] = rs->depth_test_enable ? vk_compare(rs->depth_func)
                                           : VK_COMPARE_OP_ALWAYS;
        if (rs->stencil_enable) {
            k.stencil[0] = 1;
            k.stencil[1] = vk_stencil_op(rs->stencil_fail);
            k.stencil[2] = vk_stencil_op(rs->stencil_zfail);
            k.stencil[3] = vk_stencil_op(rs->stencil_zpass);
            k.stencil[4] = vk_compare(rs->stencil_func);
        }
    }
    k.cull = VK_CULL_MODE_NONE;
    k.front = VK_FRONT_FACE_CLOCKWISE;
    if (V.cull && rs->cull_enable && !b->screen_space) {
        /* ponytail: the orientation convention is not verified against
         * hardware; RECOMP_VK_CULL is a per-title opt-in for that reason. */
        k.cull = rs->cull_face == 0x0404 ? VK_CULL_MODE_FRONT_BIT
               : rs->cull_face == 0x0408 ? VK_CULL_MODE_FRONT_AND_BACK
               : VK_CULL_MODE_BACK_BIT;
        k.front = rs->front_face == 0x0901 ? VK_FRONT_FACE_COUNTER_CLOCKWISE
                                           : VK_FRONT_FACE_CLOCKWISE;
    }
    pipe = pipe_get(&k);
    if (!pipe)
        return;

    /* 4. Per-batch data. */
    memset(&u, 0, sizeof u);
    for (st = 0; st < 8; st++) {
        unpack_argb(b->combiner.factor0[st], u.factor0[st]);
        unpack_argb(b->combiner.factor1[st], u.factor1[st]);
    }
    unpack_argb(b->combiner.final_c0, u.final_c0);
    unpack_argb(b->combiner.final_c1, u.final_c1);
    u.misc[0] = rs->alpha_ref & 0xFF;
    for (st = 0; st < 4; st++) {
        const Nv2aStage *g = &b->stages[st];
        if (!g->valid || !g->border_color)
            continue;
        if (g->tex.addr_u == 4) u.misc[1] |= 1u << (st * 2);
        if (g->tex.addr_v == 4) u.misc[1] |= 2u << (st * 2);
        unpack_argb(g->border, u.border[st]);
    }
    u.misc[2] = b->clip_plane_mode;
    u.fog_color[0] = (float)(b->fog_color & 0xFF) / 255.0f;   /* R in bits 0-7 */
    u.fog_color[1] = (float)((b->fog_color >> 8) & 0xFF) / 255.0f;
    u.fog_color[2] = (float)((b->fog_color >> 16) & 0xFF) / 255.0f;
    u.fog_color[3] = (float)(b->fog_color >> 24) / 255.0f;

    memset(&pc, 0, sizeof pc);
    pc.vp[0] = 2.0f / (float)s->im.w;
    pc.vp[1] = 2.0f / (float)s->im.h;
    pc.vp[2] = 1.0f / zmax_of(sx->zeta_format);
    for (st = 0; st < 4; st++) {
        float *o = st < 2 ? &pc.scale01[st * 2] : &pc.scale23[(st - 2) * 2];
        const Nv2aStage *g = &b->stages[st];
        o[0] = o[1] = 1.0f;
        if (g->valid && !g->normalized && g->tex.width && g->tex.height) {
            o[0] = 1.0f / (float)g->tex.width;
            o[1] = 1.0f / (float)g->tex.height;
        }
    }

    /* 5. Record, in as many pieces as the vertex ring takes. */
    while (done < b->count) {
        VkDeviceSize voff, room;
        uint32_t n;

        if (set_epoch != V.epoch) {
            VkDescriptorSetAllocateInfo ai;
            VkDescriptorBufferInfo bi;
            VkDescriptorImageInfo ii[4];
            VkWriteDescriptorSet w[5];
            int j;

            memset(&ai, 0, sizeof ai);
            ai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
            ai.descriptorPool = V.dpool;
            ai.descriptorSetCount = 1;
            ai.pSetLayouts = &V.dsl;
            if (!ring_take(&V.uring, sizeof u, V.ubo_align, &uoff)
                || vk.AllocateDescriptorSets(V.dev, &ai, &set) != VK_SUCCESS) {
                if (!submit_wait())
                    return;
                if (!ring_take(&V.uring, sizeof u, V.ubo_align, &uoff)
                    || !ck(vk.AllocateDescriptorSets(V.dev, &ai, &set),
                           "vkAllocateDescriptorSets"))
                    return;
            }
            memcpy(V.uring.map + uoff, &u, sizeof u);
            bi.buffer = V.uring.buf;
            bi.offset = 0;
            bi.range = sizeof u;
            memset(w, 0, sizeof w);
            w[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            w[0].dstSet = set;
            w[0].dstBinding = 0;
            w[0].descriptorCount = 1;
            w[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
            w[0].pBufferInfo = &bi;
            for (j = 0; j < 4; j++) {
                const Nv2aStage *g = &b->stages[j];
                ii[j].sampler = sampler_get(linear[j], g->tex.addr_u, g->tex.addr_v);
                ii[j].imageView = tx[j] ? tx[j]->im.view : V.white.view;
                ii[j].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
                if (!ii[j].sampler)
                    return;
                w[1 + j].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
                w[1 + j].dstSet = set;
                w[1 + j].dstBinding = (uint32_t)(1 + j);
                w[1 + j].descriptorCount = 1;
                w[1 + j].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
                w[1 + j].pImageInfo = &ii[j];
            }
            vk.UpdateDescriptorSets(V.dev, 5, w, 0, NULL);
            set_epoch = V.epoch;
        }
        voff = align_up(V.vring.used, 16);
        room = voff < V.vring.size ? (V.vring.size - voff) / sizeof(Nv2aVertexEx) : 0;
        n = b->count - done;
        if (room < n)
            n = (uint32_t)(room - room % 3);
        if (!n) {
            if (!submit_wait())
                return;
            continue;                       /* new epoch: a new set */
        }
        ring_take(&V.vring, (VkDeviceSize)n * sizeof(Nv2aVertexEx), 16, &voff);
        memcpy(V.vring.map + voff, b->vertices + done, (size_t)n * sizeof(Nv2aVertexEx));
        if (!pass_begin(s, d))
            return;
        {
            VkViewport vpt;
            float bc[4];
            uint32_t dyn_off = (uint32_t)uoff;
            vpt.x = vpt.y = 0.0f;
            vpt.width = (float)s->im.w;
            vpt.height = (float)s->im.h;
            vpt.minDepth = 0.0f;
            vpt.maxDepth = 1.0f;
            unpack_argb(rs->blend_color, bc);
            vk.CmdBindPipeline(V.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
            vk.CmdSetViewport(V.cmd, 0, 1, &vpt);
            vk.CmdSetScissor(V.cmd, 0, 1, &sc);
            vk.CmdSetBlendConstants(V.cmd, bc);
            vk.CmdSetStencilCompareMask(V.cmd, VK_STENCIL_FACE_FRONT_AND_BACK,
                                        rs->stencil_func_mask & 0xFF);
            vk.CmdSetStencilWriteMask(V.cmd, VK_STENCIL_FACE_FRONT_AND_BACK,
                                      rs->stencil_write_mask & 0xFF);
            vk.CmdSetStencilReference(V.cmd, VK_STENCIL_FACE_FRONT_AND_BACK,
                                      rs->stencil_ref & 0xFF);
            vk.CmdPushConstants(V.cmd, V.pl,
                                VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                                0, sizeof pc, &pc);
            vk.CmdBindDescriptorSets(V.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, V.pl, 0, 1,
                                     &set, 1, &dyn_off);
            vk.CmdBindVertexBuffers(V.cmd, 0, 1, &V.vring.buf, &voff);
            vk.CmdDraw(V.cmd, n, 1, 0, 0);
        }
        done += n;
    }
    if (k.write_mask)
        surf_mark(s, &sc);
    V.st.draws++;
    V.st.triangles += b->count / 3;
}

static void vk_clear_ex(const Nv2aSurfaceEx *sx, const Nv2aRenderState *rs,
                        uint32_t flags, uint32_t argb, uint32_t zstencil,
                        const uint32_t rect[4])
{
    uint32_t bpp = sx->base.bytes_per_pixel;
    VkClearAttachment ca;
    VkClearRect cr;
    NvkSurf *s;
    NvkDepth *d;
    VkRect2D r;

    if (!V.ok || (bpp != 2 && bpp != 4) || !sx->base.color_va || !sx->base.pitch)
        return;
    s = surf_get(sx, surface_rows(sx));
    if (!s || !surf_prepare(s))
        return;
    d = depth_get(rs->zeta_va, s->im.w, s->im.h);
    if (!d || !pass_begin(s, d))
        return;
    memset(&cr, 0, sizeof cr);
    cr.layerCount = 1;
    if ((flags & 0xF0) && clip_rect(sx, s, rect, &r)) {
        /* Any colour bit clears the whole pixel, as the CPU path does. */
        float c[4];
        unpack_argb(argb, c);
        memset(&ca, 0, sizeof ca);
        ca.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        ca.colorAttachment = 0;
        ca.clearValue.color.float32[0] = c[0];
        ca.clearValue.color.float32[1] = c[1];
        ca.clearValue.color.float32[2] = c[2];
        ca.clearValue.color.float32[3] = bpp == 4 ? c[3] : 1.0f;
        cr.rect = r;
        vk.CmdClearAttachments(V.cmd, 1, &ca, 1, &cr);
        surf_mark(s, &r);
    }
    if (flags & 0x03) {
        /* zbuf_clear clears the whole buffer, whatever the rectangle. */
        uint32_t zf = sx->zeta_format;
        memset(&ca, 0, sizeof ca);
        if (flags & 0x01)
            ca.aspectMask |= VK_IMAGE_ASPECT_DEPTH_BIT;
        if (flags & 0x02)
            ca.aspectMask |= VK_IMAGE_ASPECT_STENCIL_BIT;
        ca.clearValue.depthStencil.depth =
            (float)(zf == 1 ? (zstencil & 0xFFFF) : (zstencil >> 8)) / zmax_of(zf);
        ca.clearValue.depthStencil.stencil = zstencil & 0xFF;
        cr.rect.offset.x = cr.rect.offset.y = 0;
        cr.rect.extent.width = s->im.w;
        cr.rect.extent.height = s->im.h;
        vk.CmdClearAttachments(V.cmd, 1, &ca, 1, &cr);
    }
    V.st.clears++;
}

/* ---- pipeline cache on disk ---------------------------------------------- */

/* Pipelines are built the first time a combiner program is drawn with, and
 * a phone driver compiles each one in tens of milliseconds: a stutter every
 * time the game shows something new, again on every start. With
 * RECOMP_VK_CACHE=<file> (set by the Android host) the driver's pipeline
 * cache is kept across runs.
 *
 * File: "XBRVKPC1", u32 payload length, u32 0, u64 FNV-1a of the payload,
 * u64 FNV-1a of this build's SPIR-V; then the vkGetPipelineCacheData bytes.
 * The payload reaches the driver only when the file is whole and its Vulkan
 * header names this device, driver build and shaders: some mobile drivers
 * misbehave on damaged or foreign cache data instead of ignoring it. Saved
 * through a temporary file and a rename, so a crash mid-write leaves the
 * old file. */

#define NVK_PC_MAGIC "XBRVKPC1"
#define NVK_PC_HDR 32u
#define NVK_PC_MAX (64u << 20)
#define NVK_PC_SAVE_FRAMES 300u      /* save once no pipeline was new for this long */

static uint64_t fnv1a64(uint64_t h, const void *data, size_t n)
{
    const uint8_t *p = (const uint8_t *)data;
    while (n--) {
        h ^= *p++;
        h *= 1099511628211ull;
    }
    return h;
}

static uint64_t pc_shader_hash(void)
{
    uint64_t h = fnv1a64(1469598103934665603ull, nv2a_vk_vert_spv, sizeof nv2a_vk_vert_spv);
    return fnv1a64(h, nv2a_vk_frag_spv, sizeof nv2a_vk_frag_spv);
}

/* The file's payload when it may be given to this device, else NULL with
 * V.pc_state saying why. */
static void *pc_load(size_t *len)
{
    VkPhysicalDeviceProperties p;
    uint8_t hdr[NVK_PC_HDR], *buf;
    uint32_t n, vhl, vver, vven, vdev;
    uint64_t h, sh;
    FILE *f;

    *len = 0;
    f = fopen(V.pc_path, "rb");
    if (!f) {
        snprintf(V.pc_state, sizeof V.pc_state, "new");
        return NULL;
    }
    if (fread(hdr, 1, sizeof hdr, f) != sizeof hdr || memcmp(hdr, NVK_PC_MAGIC, 8) != 0) {
        fclose(f);
        snprintf(V.pc_state, sizeof V.pc_state, "ignored (not a pipeline cache file)");
        return NULL;
    }
    memcpy(&n, hdr + 8, 4);
    memcpy(&h, hdr + 16, 8);
    memcpy(&sh, hdr + 24, 8);
    if (n < 16 + VK_UUID_SIZE || n > NVK_PC_MAX) {
        fclose(f);
        snprintf(V.pc_state, sizeof V.pc_state, "ignored (bad length %u)", n);
        return NULL;
    }
    buf = (uint8_t *)malloc(n);
    if (!buf || fread(buf, 1, n, f) != n || fgetc(f) != EOF) {
        fclose(f);
        free(buf);
        snprintf(V.pc_state, sizeof V.pc_state, "ignored (truncated or wrong length)");
        return NULL;
    }
    fclose(f);
    if (fnv1a64(1469598103934665603ull, buf, n) != h) {
        free(buf);
        snprintf(V.pc_state, sizeof V.pc_state, "ignored (checksum)");
        return NULL;
    }
    if (sh != pc_shader_hash()) {
        free(buf);
        snprintf(V.pc_state, sizeof V.pc_state, "new (the shaders changed)");
        return NULL;
    }
    /* VkPipelineCacheHeaderVersionOne */
    vk.GetPhysicalDeviceProperties(V.phys, &p);
    memcpy(&vhl, buf, 4);
    memcpy(&vver, buf + 4, 4);
    memcpy(&vven, buf + 8, 4);
    memcpy(&vdev, buf + 12, 4);
    if (vhl < 16 + VK_UUID_SIZE || vhl > n || vver != VK_PIPELINE_CACHE_HEADER_VERSION_ONE
        || vven != p.vendorID || vdev != p.deviceID
        || memcmp(buf + 16, p.pipelineCacheUUID, VK_UUID_SIZE) != 0) {
        free(buf);
        snprintf(V.pc_state, sizeof V.pc_state, "new (another device or driver build)");
        return NULL;
    }
    *len = n;
    return buf;
}

static void pc_save(const char *why)
{
    char tmp[1100];
    uint8_t hdr[NVK_PC_HDR];
    size_t n = 0;
    uint32_t n32;
    uint64_t h;
    void *buf;
    FILE *f;
    int ok;

    V.pc_dirty = 0;
    if (!V.pc_path[0] || !V.pcache)
        return;
    if (vk.GetPipelineCacheData(V.dev, V.pcache, &n, NULL) != VK_SUCCESS || !n || n > NVK_PC_MAX)
        return;
    buf = malloc(n);
    if (!buf)
        return;
    if (vk.GetPipelineCacheData(V.dev, V.pcache, &n, buf) != VK_SUCCESS) {
        free(buf);                          /* grew in between: next time */
        V.pc_dirty = 1;
        return;
    }
    n32 = (uint32_t)n;
    h = fnv1a64(1469598103934665603ull, buf, n);
    memset(hdr, 0, sizeof hdr);
    memcpy(hdr, NVK_PC_MAGIC, 8);
    memcpy(hdr + 8, &n32, 4);
    memcpy(hdr + 16, &h, 8);
    h = pc_shader_hash();
    memcpy(hdr + 24, &h, 8);
    snprintf(tmp, sizeof tmp, "%s.tmp", V.pc_path);
    f = fopen(tmp, "wb");
    if (!f) {
        nvk_log("pipeline cache: cannot write %s (%s); not kept this run", tmp, strerror(errno));
        V.pc_path[0] = 0;
        free(buf);
        return;
    }
    ok = fwrite(hdr, 1, sizeof hdr, f) == sizeof hdr && fwrite(buf, 1, n, f) == n
         && fflush(f) == 0 && fsync(fileno(f)) == 0;
    ok = (fclose(f) == 0) && ok;
    free(buf);
    if (!ok || rename(tmp, V.pc_path) != 0) {
        nvk_log("pipeline cache: writing %s failed (%s)", V.pc_path, strerror(errno));
        remove(tmp);
        return;
    }
    V.st.pcache_saves++;
    V.st.pcache_save_bytes = n;
    if (V.stats_every > 0)
        nvk_log("pipeline cache: %zu bytes saved (%s)", n, why);
}

static void vk_print_stats(void)
{
    nvk_log("frame %llu: draws %llu tris %llu clears %llu pipelines %llu tex %llu (%llu KB)"
            " surf-up %llu readbacks %llu (%llu KB) submits %llu passes %llu",
            (unsigned long long)V.frame, (unsigned long long)V.st.draws,
            (unsigned long long)V.st.triangles, (unsigned long long)V.st.clears,
            (unsigned long long)V.st.pipelines, (unsigned long long)V.st.texture_uploads,
            (unsigned long long)(V.st.texture_bytes >> 10),
            (unsigned long long)V.st.surface_uploads, (unsigned long long)V.st.readbacks,
            (unsigned long long)(V.st.readback_bytes >> 10),
            (unsigned long long)V.st.submits, (unsigned long long)V.st.render_passes);
}

/* The flip's present_sync: every drawn surface into guest memory before the
 * window snapshot and the frame dump. */
static void vk_present_sync(void)
{
    int i;

    if (!V.ok)
        return;
    if (!surf_sync_all() || !submit_wait())
        return;
    V.frame++;
    for (i = 0; i < NVK_MAX_SURF; i++)
        V.surf[i].validated = 0;
    tex_frame();
    if (V.stats_every > 0 && V.frame % (uint64_t)V.stats_every == 0)
        vk_print_stats();
    /* After a burst of new pipelines (a new area, a menu) has settled, so the
     * write is one short hitch rather than one per pipeline. */
    if (V.pc_dirty && V.frame - V.pc_new_frame >= NVK_PC_SAVE_FRAMES)
        pc_save("idle");
}

static void vk_flip(void)
{
}

static const Nv2aBackend s_vk_backend = {
    NULL,           /* clear: clear_ex instead */
    NULL,           /* draw: draw_ex instead */
    vk_flip,
    vk_draw_ex,
    vk_clear_ex,
};

/* ---- bring-up ----------------------------------------------------------------- */

static const char *dev_type(VkPhysicalDeviceType t)
{
    switch (t) {
    case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU: return "integrated GPU";
    case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU:   return "discrete GPU";
    case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU:    return "virtual GPU";
    case VK_PHYSICAL_DEVICE_TYPE_CPU:            return "CPU";
    default:                                     return "other";
    }
}

static int fmt_has(VkPhysicalDevice pd, VkFormat f, VkFormatFeatureFlags need)
{
    VkFormatProperties p;
    vk.GetPhysicalDeviceFormatProperties(pd, f, &p);
    return (p.optimalTilingFeatures & need) == need;
}

static int has_ext(const VkExtensionProperties *e, uint32_t n, const char *name)
{
    uint32_t i;
    for (i = 0; i < n; i++)
        if (!strcmp(e[i].extensionName, name))
            return 1;
    return 0;
}

/* ==== window presentation ================================================= */

/* [android-port] Frames reach the screen through a Vulkan swapchain on the
 * device the renderer uses, rather than through the SDL renderer (GLES on
 * Android). The window thread hands over each finished frame from guest
 * memory -- where the flip's present_sync has just written it back -- and
 * it is uploaded, scaled into the window with a blit (4:3 letterboxed by
 * default) and presented. Guest memory stays the one source of truth, so
 * frames the CPU rasteriser or the title itself drew show the same way.
 *
 * Threads: the window thread presents, the executor renders. They share the
 * device and the one queue (s_qlock); the presenter has its own command
 * pool, fences, semaphores and error state, so a presenter failure never
 * switches the renderer off. */

#define NVK_PRESENT_MAX_IMAGES 16

typedef struct {
    VkBuffer buf;
    VkDeviceMemory mem;
    void *map;
    VkDeviceSize size;
} NvkPBuf;

typedef struct {
    VkImage img;
    VkDeviceMemory mem;
    uint32_t w, h;
    VkFormat fmt;
} NvkPImg;

static struct {
    /* the request, made before the device exists */
    int wanted;
    const char *exts[8];
    uint32_t next;
    Nv2aVkSurfaceFn create_surface;
    void *ctx;
    uint32_t want_w, want_h;
    int have_surface_ext, have_swapchain_ext, can_present;
    /* state */
    int ok;                         /* frames can be presented (now or later) */
    int suspended, need_swapchain;
    VkSurfaceKHR surface;
    VkSwapchainKHR swapchain;
    VkFormat format;
    VkExtent2D extent;
    VkImageUsageFlags usage;
    VkCompositeAlphaFlagBitsKHR alpha;
    VkPresentModeKHR mode;
    VkSurfaceTransformFlagBitsKHR transform;
    uint32_t nimg;
    VkImage img[NVK_PRESENT_MAX_IMAGES];
    VkSemaphore done[NVK_PRESENT_MAX_IMAGES];
    VkCommandPool pool;
    VkCommandBuffer cmd[2];
    VkFence fence[2];
    int pending[2];
    VkSemaphore acq[2];
    NvkPBuf staging[2];
    NvkPImg src[2];
    NvkPBuf cap;                    /* tests: a copy of the last presented image */
    int capture, cap_valid, cap_slot;
    uint32_t cap_w, cap_h;
    uint32_t frame;
    int vsync, linear;
    double aspect;                  /* display aspect; 0 = stretch, <0 = the frame's */
    uint64_t presents, skipped, swapchains, suspends;
    char status[384];
} P;

static int p_off(const char *fmt, ...)
{
    va_list ap;
    int n = snprintf(P.status, sizeof P.status, "off: ");
    va_start(ap, fmt);
    vsnprintf(P.status + n, sizeof P.status - (size_t)n, fmt, ap);
    va_end(ap);
    P.ok = 0;
    nvk_log("presenter %s", P.status);
    return -1;
}

static void p_read_env(void)
{
    const char *e = getenv("RECOMP_PRESENT_VSYNC");
    P.vsync = !e || strcmp(e, "0") != 0;
    e = getenv("RECOMP_PRESENT_FILTER");
    P.linear = !(e && !strcmp(e, "nearest"));
    P.aspect = 4.0 / 3.0;
    e = getenv("RECOMP_ASPECT");
    if (e && *e) {
        unsigned a = 0, b = 0;
        if (!strcmp(e, "stretch"))
            P.aspect = 0;
        else if (!strcmp(e, "native") || !strcmp(e, "source"))
            P.aspect = -1;
        else if (sscanf(e, "%u:%u", &a, &b) == 2 && a && b)
            P.aspect = (double)a / (double)b;
    }
}

static const char *p_fmt_name(VkFormat f)
{
    switch (f) {
    case VK_FORMAT_B8G8R8A8_UNORM: return "B8G8R8A8_UNORM";
    case VK_FORMAT_R8G8B8A8_UNORM: return "R8G8B8A8_UNORM";
    case VK_FORMAT_A8B8G8R8_UNORM_PACK32: return "A8B8G8R8_UNORM";
    case VK_FORMAT_B8G8R8A8_SRGB: return "B8G8R8A8_SRGB";
    case VK_FORMAT_R8G8B8A8_SRGB: return "R8G8B8A8_SRGB";
    case VK_FORMAT_A8B8G8R8_SRGB_PACK32: return "A8B8G8R8_SRGB";
    case VK_FORMAT_A2B10G10R10_UNORM_PACK32: return "A2B10G10R10_UNORM";
    case VK_FORMAT_R5G6B5_UNORM_PACK16: return "R5G6B5_UNORM";
    default: return "another format";
    }
}

static void p_buf_free(NvkPBuf *b)
{
    if (b->map)
        vk.UnmapMemory(V.dev, b->mem);
    if (b->buf)
        vk.DestroyBuffer(V.dev, b->buf, NULL);
    if (b->mem)
        vk.FreeMemory(V.dev, b->mem, NULL);
    memset(b, 0, sizeof *b);
}

/* Host-visible and coherent; `readback` prefers cached memory. */
static int p_buf(NvkPBuf *b, VkDeviceSize size, VkBufferUsageFlags usage, int readback)
{
    VkBufferCreateInfo ci;
    VkMemoryRequirements req;
    VkMemoryAllocateInfo ai;
    int t = -1;

    memset(b, 0, sizeof *b);
    memset(&ci, 0, sizeof ci);
    ci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    ci.size = size;
    ci.usage = usage;
    ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (vk.CreateBuffer(V.dev, &ci, NULL, &b->buf) != VK_SUCCESS)
        return 0;
    vk.GetBufferMemoryRequirements(V.dev, b->buf, &req);
    if (readback)
        t = mem_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT
                                             | VK_MEMORY_PROPERTY_HOST_CACHED_BIT
                                             | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (t < 0)
        t = mem_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT
                                             | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    memset(&ai, 0, sizeof ai);
    ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = (uint32_t)t;
    if (t < 0 || vk.AllocateMemory(V.dev, &ai, NULL, &b->mem) != VK_SUCCESS
        || vk.BindBufferMemory(V.dev, b->buf, b->mem, 0) != VK_SUCCESS
        || vk.MapMemory(V.dev, b->mem, 0, VK_WHOLE_SIZE, 0, &b->map) != VK_SUCCESS) {
        p_buf_free(b);
        return 0;
    }
    b->size = size;
    return 1;
}

static void p_img_free(NvkPImg *im)
{
    if (im->img)
        vk.DestroyImage(V.dev, im->img, NULL);
    if (im->mem)
        vk.FreeMemory(V.dev, im->mem, NULL);
    memset(im, 0, sizeof *im);
}

static int p_img(NvkPImg *im, uint32_t w, uint32_t h, VkFormat fmt)
{
    VkImageCreateInfo ci;
    VkMemoryRequirements req;
    VkMemoryAllocateInfo ai;
    int t;

    memset(im, 0, sizeof *im);
    memset(&ci, 0, sizeof ci);
    ci.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    ci.imageType = VK_IMAGE_TYPE_2D;
    ci.format = fmt;
    ci.extent.width = w;
    ci.extent.height = h;
    ci.extent.depth = 1;
    ci.mipLevels = 1;
    ci.arrayLayers = 1;
    ci.samples = VK_SAMPLE_COUNT_1_BIT;
    ci.tiling = VK_IMAGE_TILING_OPTIMAL;
    ci.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (vk.CreateImage(V.dev, &ci, NULL, &im->img) != VK_SUCCESS)
        return 0;
    vk.GetImageMemoryRequirements(V.dev, im->img, &req);
    t = mem_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (t < 0)
        t = mem_type(req.memoryTypeBits, 0);
    memset(&ai, 0, sizeof ai);
    ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = (uint32_t)t;
    if (t < 0 || vk.AllocateMemory(V.dev, &ai, NULL, &im->mem) != VK_SUCCESS
        || vk.BindImageMemory(V.dev, im->img, im->mem, 0) != VK_SUCCESS) {
        p_img_free(im);
        return 0;
    }
    im->w = w;
    im->h = h;
    im->fmt = fmt;
    return 1;
}

static void p_wait_slots(void)
{
    int i;
    for (i = 0; i < 2; i++)
        if (P.pending[i]) {
            vk.WaitForFences(V.dev, 1, &P.fence[i], VK_TRUE, UINT64_MAX);
            vk.ResetFences(V.dev, 1, &P.fence[i]);
            P.pending[i] = 0;
        }
}

static void p_swapchain_destroy(void)
{
    uint32_t i;

    if (!P.swapchain)
        return;
    /* Every blit and present that used its images is done. */
    dev_idle();
    p_wait_slots();
    for (i = 0; i < P.nimg; i++)
        if (P.done[i]) {
            vk.DestroySemaphore(V.dev, P.done[i], NULL);
            P.done[i] = VK_NULL_HANDLE;
        }
    vk.DestroySwapchainKHR(V.dev, P.swapchain, NULL);
    P.swapchain = VK_NULL_HANDLE;
    P.nimg = 0;
    P.cap_valid = 0;
}

static void p_surface_destroy(void)
{
    p_swapchain_destroy();
    if (P.surface && V.inst && vk.DestroySurfaceKHR)
        vk.DestroySurfaceKHR(V.inst, P.surface, NULL);
    P.surface = VK_NULL_HANDLE;
}

/* The window system's surface for the window, through the host's callback.
 * Before a device is chosen, pick_device checks it can be presented to.
 * `quiet`: no window yet is not a failure (Android, just after a resume). */
static int p_surface_create_q(int quiet)
{
    uint64_t s = 0;
    VkBool32 sup = VK_FALSE;

    if (P.create_surface(P.ctx, (void *)V.inst, &s) != 0 || !s)
        return quiet ? -1 : p_off("the window system made no Vulkan surface");
    memcpy(&P.surface, &s, sizeof P.surface);
    if (V.dev) {
        if (vk.GetPhysicalDeviceSurfaceSupportKHR(V.phys, V.qfam, P.surface, &sup) != VK_SUCCESS
            || !sup)
            return p_off("the graphics queue cannot present to the window");
    }
    return 0;
}

static int p_surface_create(void)
{
    return p_surface_create_q(0);
}

static int p_format_ok(VkFormat f)
{
    VkFormatProperties fp;
    vk.GetPhysicalDeviceFormatProperties(V.phys, f, &fp);
    return (fp.optimalTilingFeatures & VK_FORMAT_FEATURE_BLIT_DST_BIT) != 0;
}

/* 1 made, 0 not now (no surface, a zero-sized window: try again later),
 * negative: the presenter is off. */
static int p_swapchain_create(void)
{
    static const VkFormat pref[] = {
        VK_FORMAT_B8G8R8A8_UNORM, VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_A8B8G8R8_UNORM_PACK32,
        VK_FORMAT_B8G8R8A8_SRGB, VK_FORMAT_R8G8B8A8_SRGB, VK_FORMAT_A8B8G8R8_SRGB_PACK32,
    };
    VkSurfaceCapabilitiesKHR caps;
    VkSurfaceFormatKHR fmts[64];
    VkPresentModeKHR modes[16];
    VkSwapchainCreateInfoKHR ci;
    VkSemaphoreCreateInfo si;
    uint32_t nf = 64, nm = 16, i, k, want;
    VkResult r;
    int found = 0;

    if (!P.surface)
        return 0;
    r = vk.GetPhysicalDeviceSurfaceCapabilitiesKHR(V.phys, P.surface, &caps);
    if (r == VK_ERROR_SURFACE_LOST_KHR) {
        p_surface_destroy();
        return 0;
    }
    if (r != VK_SUCCESS)
        return p_off("vkGetPhysicalDeviceSurfaceCapabilitiesKHR failed (VkResult %d)", (int)r);
    if (caps.currentExtent.width != 0xFFFFFFFFu) {
        P.extent = caps.currentExtent;
    } else {
        P.extent.width = P.want_w;
        P.extent.height = P.want_h;
        if (P.extent.width < caps.minImageExtent.width) P.extent.width = caps.minImageExtent.width;
        if (P.extent.height < caps.minImageExtent.height) P.extent.height = caps.minImageExtent.height;
        if (P.extent.width > caps.maxImageExtent.width) P.extent.width = caps.maxImageExtent.width;
        if (P.extent.height > caps.maxImageExtent.height) P.extent.height = caps.maxImageExtent.height;
    }
    if (!P.extent.width || !P.extent.height) {
        P.need_swapchain = 1;               /* minimised: later */
        return 0;
    }
    if (!(caps.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_DST_BIT))
        return p_off("the window's images cannot be written by transfers");
    P.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    if (P.capture && (caps.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_SRC_BIT))
        P.usage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;

    r = vk.GetPhysicalDeviceSurfaceFormatsKHR(V.phys, P.surface, &nf, fmts);
    if ((r != VK_SUCCESS && r != VK_INCOMPLETE) || !nf)
        return p_off("the window offers no formats (VkResult %d)", (int)r);
    if (nf == 1 && fmts[0].format == VK_FORMAT_UNDEFINED) {
        P.format = VK_FORMAT_B8G8R8A8_UNORM;
        found = 1;
    }
    /* UNORM first: the frames are already display-encoded, and a blit into
     * an sRGB image would encode them a second time. */
    for (k = 0; k < sizeof pref / sizeof pref[0] && !found; k++)
        for (i = 0; i < nf && !found; i++)
            if (fmts[i].format == pref[k] && fmts[i].colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR
                && p_format_ok(pref[k])) {
                P.format = pref[k];
                found = 1;
            }
    if (!found)
        return p_off("none of the window's %u formats is an 8-bit RGBA one a blit can write", nf);

    P.alpha = (caps.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR)
                  ? VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR
            : (caps.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR)
                  ? VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR
                  : (VkCompositeAlphaFlagBitsKHR)(caps.supportedCompositeAlpha
                                                  & (~caps.supportedCompositeAlpha + 1));
    /* A half turn is pre-rotated (the blit reverses both axes, p_present);
     * a quarter turn cannot be done by a blit, so the compositor rotates
     * those frames: correct, if a little slower. */
    if (caps.currentTransform == VK_SURFACE_TRANSFORM_ROTATE_180_BIT_KHR
        && (caps.supportedTransforms & VK_SURFACE_TRANSFORM_ROTATE_180_BIT_KHR))
        P.transform = VK_SURFACE_TRANSFORM_ROTATE_180_BIT_KHR;
    else if (caps.supportedTransforms & VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR)
        P.transform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
    else
        return p_off("the window does not accept unrotated frames (transforms 0x%X)",
                     (unsigned)caps.supportedTransforms);
    P.mode = VK_PRESENT_MODE_FIFO_KHR;      /* always there: vsync */
    if (!P.vsync) {
        r = vk.GetPhysicalDeviceSurfacePresentModesKHR(V.phys, P.surface, &nm, modes);
        if (r == VK_SUCCESS || r == VK_INCOMPLETE) {
            for (i = 0; i < nm; i++)
                if (modes[i] == VK_PRESENT_MODE_MAILBOX_KHR)
                    P.mode = modes[i];
            if (P.mode == VK_PRESENT_MODE_FIFO_KHR)
                for (i = 0; i < nm; i++)
                    if (modes[i] == VK_PRESENT_MODE_IMMEDIATE_KHR)
                        P.mode = modes[i];
        }
    }
    want = caps.minImageCount + 1;
    if (want < 2)
        want = 2;
    if (caps.maxImageCount && want > caps.maxImageCount)
        want = caps.maxImageCount;
    if (want > NVK_PRESENT_MAX_IMAGES)
        want = NVK_PRESENT_MAX_IMAGES;

    memset(&ci, 0, sizeof ci);
    ci.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
    ci.surface = P.surface;
    ci.minImageCount = want;
    ci.imageFormat = P.format;
    ci.imageColorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
    ci.imageExtent = P.extent;
    ci.imageArrayLayers = 1;
    ci.imageUsage = P.usage;
    ci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ci.preTransform = P.transform;
    ci.compositeAlpha = P.alpha;
    ci.presentMode = P.mode;
    ci.clipped = VK_TRUE;
    r = vk.CreateSwapchainKHR(V.dev, &ci, NULL, &P.swapchain);
    if (r == VK_ERROR_SURFACE_LOST_KHR || r == VK_ERROR_NATIVE_WINDOW_IN_USE_KHR) {
        P.swapchain = VK_NULL_HANDLE;
        if (r == VK_ERROR_SURFACE_LOST_KHR)
            p_surface_destroy();
        else
            P.need_swapchain = 1;
        return 0;
    }
    if (r != VK_SUCCESS) {
        P.swapchain = VK_NULL_HANDLE;
        return p_off("vkCreateSwapchainKHR failed (VkResult %d)", (int)r);
    }
    P.nimg = NVK_PRESENT_MAX_IMAGES;
    r = vk.GetSwapchainImagesKHR(V.dev, P.swapchain, &P.nimg, P.img);
    if (r != VK_SUCCESS) {
        vk.DestroySwapchainKHR(V.dev, P.swapchain, NULL);
        P.swapchain = VK_NULL_HANDLE;
        P.nimg = 0;
        return p_off("the swapchain has more than %d images (VkResult %d)",
                     NVK_PRESENT_MAX_IMAGES, (int)r);
    }
    memset(&si, 0, sizeof si);
    si.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
    for (i = 0; i < P.nimg; i++)
        if (vk.CreateSemaphore(V.dev, &si, NULL, &P.done[i]) != VK_SUCCESS) {
            p_swapchain_destroy();
            return p_off("vkCreateSemaphore failed");
        }
    P.need_swapchain = 0;
    P.swapchains++;
    snprintf(P.status, sizeof P.status,
             "swapchain %ux%u %s, %u images, %s, %s %s%s%s",
             P.extent.width, P.extent.height, p_fmt_name(P.format), P.nimg,
             P.mode == VK_PRESENT_MODE_FIFO_KHR ? "FIFO (vsync)"
             : P.mode == VK_PRESENT_MODE_MAILBOX_KHR ? "mailbox" : "immediate",
             P.aspect == 0 ? "stretched" : P.aspect < 0 ? "frame aspect" : "letterboxed",
             P.linear ? "linear" : "nearest",
             P.transform == VK_SURFACE_TRANSFORM_ROTATE_180_BIT_KHR ? ", pre-rotated a half turn"
             : caps.currentTransform != VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR ? ", rotated by the compositor" : "",
             P.alpha == VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR ? "" : ", alpha forced opaque");
    return 1;
}

/* The presenter's own objects, once the device is up. Never fails the
 * renderer: on any problem the presenter is off and the window falls back. */
static void p_setup(void)
{
    VkCommandPoolCreateInfo cp;
    VkCommandBufferAllocateInfo ca;
    VkFenceCreateInfo fi;
    VkSemaphoreCreateInfo si;
    int i;

    if (!P.surface) {
        if (!P.status[0])
            p_off("no surface");
        return;
    }
    if (!P.can_present) {
        p_off("the chosen device's graphics queue cannot present to the window");
        return;
    }
    if (!P.have_swapchain_ext) {
        p_off("the device has no VK_KHR_swapchain");
        return;
    }
    memset(&cp, 0, sizeof cp);
    cp.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    cp.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    cp.queueFamilyIndex = V.qfam;
    memset(&ca, 0, sizeof ca);
    ca.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    ca.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ca.commandBufferCount = 2;
    memset(&fi, 0, sizeof fi);
    fi.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    memset(&si, 0, sizeof si);
    si.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
    if (vk.CreateCommandPool(V.dev, &cp, NULL, &P.pool) != VK_SUCCESS) {
        p_off("vkCreateCommandPool failed");
        return;
    }
    ca.commandPool = P.pool;
    if (vk.AllocateCommandBuffers(V.dev, &ca, P.cmd) != VK_SUCCESS) {
        p_off("vkAllocateCommandBuffers failed");
        return;
    }
    for (i = 0; i < 2; i++)
        if (vk.CreateFence(V.dev, &fi, NULL, &P.fence[i]) != VK_SUCCESS
            || vk.CreateSemaphore(V.dev, &si, NULL, &P.acq[i]) != VK_SUCCESS) {
            p_off("vkCreateFence/vkCreateSemaphore failed");
            return;
        }
    P.ok = 1;
    if (p_swapchain_create() < 0)
        return;
    if (!P.swapchain)
        snprintf(P.status, sizeof P.status, "waiting for the window (no swapchain yet)");
}

/* Every presenter object, and the surface. The renderer stays up. */
static void p_teardown(void)
{
    int i;

    if (V.dev) {
        if (P.pool || P.swapchain)
            dev_idle();
        p_wait_slots();
        p_swapchain_destroy();
        for (i = 0; i < 2; i++) {
            if (P.fence[i]) vk.DestroyFence(V.dev, P.fence[i], NULL);
            if (P.acq[i]) vk.DestroySemaphore(V.dev, P.acq[i], NULL);
            p_buf_free(&P.staging[i]);
            p_img_free(&P.src[i]);
        }
        p_buf_free(&P.cap);
        if (P.pool)
            vk.DestroyCommandPool(V.dev, P.pool, NULL);
    }
    p_surface_destroy();
    memset(&P, 0, sizeof P);
}

static void p_barrier(VkCommandBuffer cb, VkImage img, VkImageLayout from, VkImageLayout to,
                      VkAccessFlags sa, VkAccessFlags da,
                      VkPipelineStageFlags ss, VkPipelineStageFlags ds)
{
    VkImageMemoryBarrier b;
    memset(&b, 0, sizeof b);
    b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    b.srcAccessMask = sa;
    b.dstAccessMask = da;
    b.oldLayout = from;
    b.newLayout = to;
    b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = img;
    b.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    b.subresourceRange.levelCount = 1;
    b.subresourceRange.layerCount = 1;
    vk.CmdPipelineBarrier(cb, ss, ds, 0, 0, NULL, 0, NULL, 1, &b);
}

/* Where a w x h frame goes in the window: centred at the display aspect. */
static void p_dst_rect(uint32_t w, uint32_t h, VkOffset3D o[2])
{
    double ew = P.extent.width, eh = P.extent.height, a = P.aspect, dw, dh;
    int x0, y0;

    memset(o, 0, 2 * sizeof *o);
    o[1].z = 1;
    if (a < 0)
        a = (double)w / (double)h;
    if (a == 0) {
        o[1].x = (int32_t)P.extent.width;
        o[1].y = (int32_t)P.extent.height;
        return;
    }
    if (ew / eh > a) {
        dh = eh;
        dw = eh * a;
    } else {
        dw = ew;
        dh = ew / a;
    }
    x0 = (int)((ew - dw) / 2.0 + 0.5);
    y0 = (int)((eh - dh) / 2.0 + 0.5);
    o[0].x = x0;
    o[0].y = y0;
    o[1].x = x0 + (int)(dw + 0.5);
    o[1].y = y0 + (int)(dh + 0.5);
    if (o[1].x > (int32_t)P.extent.width) o[1].x = (int32_t)P.extent.width;
    if (o[1].y > (int32_t)P.extent.height) o[1].y = (int32_t)P.extent.height;
}

static int p_fail(const char *what, VkResult r)
{
    if (r == VK_ERROR_DEVICE_LOST)
        return p_off("%s: the device was lost", what);
    return p_off("%s failed (VkResult %d)", what, (int)r);
}

/* One frame: `px` (w x h, `pitch` bytes a row, bpp 2 = R5G6B5, 4 = X8R8G8B8)
 * or, with px NULL, the last frame again. Records and submits on this
 * thread's slot; waits only for the slot used two frames ago, so the CPU
 * prepares a frame while the GPU shows the previous one. */
static int p_present(const void *px, uint32_t w, uint32_t h, uint32_t pitch, uint32_t bpp)
{
    VkCommandBufferBeginInfo bi;
    VkSubmitInfo si;
    VkPresentInfoKHR pi;
    VkImageBlit blit;
    VkClearColorValue black;
    VkImageSubresourceRange range;
    VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
    VkCommandBuffer cb;
    NvkPImg *src;
    VkResult r;
    uint32_t img, slot, y;
    VkFormat sf;
    int reuse = !px;

    if (!P.ok)
        return -1;
    if (P.suspended)
        return 1;
    if (!P.surface) {
        if (p_surface_create_q(1) < 0) {
            if (!P.ok)
                return -1;                  /* the window cannot be presented to */
            P.skipped++;
            return 1;                       /* no window yet: the next frame tries again */
        }
        P.need_swapchain = 1;
    }
    if (!P.swapchain || P.need_swapchain) {
        int c;
        p_swapchain_destroy();
        c = p_swapchain_create();
        if (c < 0)
            return -1;
        if (c == 0)
            return 1;
    }
    if (reuse) {
        if (P.cap_slot < 0 || !P.src[P.cap_slot].img)
            return 1;                       /* nothing shown yet */
        slot = (uint32_t)P.cap_slot;
        w = P.src[slot].w;
        h = P.src[slot].h;
    } else {
        if (!w || !h || w > NVK_MAX_DIM || h > NVK_MAX_DIM || (bpp != 2 && bpp != 4)
            || pitch < w * bpp)
            return 1;
        slot = P.cap_slot < 0 ? 0 : (uint32_t)(P.cap_slot ^ 1);
    }
    if (P.pending[slot]) {
        r = vk.WaitForFences(V.dev, 1, &P.fence[slot], VK_TRUE, UINT64_MAX);
        if (r != VK_SUCCESS)
            return p_fail("vkWaitForFences", r);
        vk.ResetFences(V.dev, 1, &P.fence[slot]);
        P.pending[slot] = 0;
    }
    src = &P.src[slot];
    if (!reuse) {
        VkDeviceSize need = (VkDeviceSize)w * h * bpp;
        sf = bpp == 4 ? VK_FORMAT_B8G8R8A8_UNORM : VK_FORMAT_R5G6B5_UNORM_PACK16;
        if (bpp == 2) {
            VkFormatProperties fp;
            vk.GetPhysicalDeviceFormatProperties(V.phys, sf, &fp);
            if (!(fp.optimalTilingFeatures & VK_FORMAT_FEATURE_BLIT_SRC_BIT)) {
                sf = VK_FORMAT_B8G8R8A8_UNORM;     /* widened on the CPU below */
                need = (VkDeviceSize)w * h * 4;
            }
        }
        if (P.staging[slot].size < need) {
            p_buf_free(&P.staging[slot]);
            if (!p_buf(&P.staging[slot], need, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, 0))
                return p_off("no memory for a %ux%u frame", w, h);
        }
        if (!src->img || src->w != w || src->h != h || src->fmt != sf) {
            p_img_free(src);
            if (!p_img(src, w, h, sf))
                return p_off("cannot make a %ux%u frame image", w, h);
        }
        /* Rows into the staging buffer; X8R8G8B8 gets an opaque alpha so the
         * window's alpha mode never matters. */
        for (y = 0; y < h; y++) {
            const uint8_t *s = (const uint8_t *)px + (size_t)y * pitch;
            if (sf == VK_FORMAT_B8G8R8A8_UNORM && bpp == 4) {
                uint32_t *d = (uint32_t *)P.staging[slot].map + (size_t)y * w, x;
                memcpy(d, s, (size_t)w * 4);
                for (x = 0; x < w; x++)
                    d[x] |= 0xFF000000u;
            } else if (sf == VK_FORMAT_B8G8R8A8_UNORM) {
                uint32_t *d = (uint32_t *)P.staging[slot].map + (size_t)y * w, x;
                for (x = 0; x < w; x++) {
                    uint32_t v = (uint32_t)s[2 * x] | ((uint32_t)s[2 * x + 1] << 8);
                    uint32_t r5 = (v >> 11) & 31, g6 = (v >> 5) & 63, b5 = v & 31;
                    d[x] = 0xFF000000u | (((r5 << 3) | (r5 >> 2)) << 16)
                         | (((g6 << 2) | (g6 >> 4)) << 8) | ((b5 << 3) | (b5 >> 2));
                }
            } else {
                memcpy((uint8_t *)P.staging[slot].map + (size_t)y * w * 2, s, (size_t)w * 2);
            }
        }
    }

    r = vk.AcquireNextImageKHR(V.dev, P.swapchain, 100000000ull /* 100 ms */, P.acq[slot],
                               VK_NULL_HANDLE, &img);
    if (r == VK_TIMEOUT || r == VK_NOT_READY) {
        P.skipped++;
        return 1;
    }
    if (r == VK_ERROR_OUT_OF_DATE_KHR) {
        P.need_swapchain = 1;
        P.skipped++;
        return 1;
    }
    if (r == VK_ERROR_SURFACE_LOST_KHR) {
        p_surface_destroy();
        P.skipped++;
        return 1;
    }
    if (r != VK_SUCCESS && r != VK_SUBOPTIMAL_KHR)
        return p_fail("vkAcquireNextImageKHR", r);

    cb = P.cmd[slot];
    vk.ResetCommandBuffer(cb, 0);
    memset(&bi, 0, sizeof bi);
    bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if ((r = vk.BeginCommandBuffer(cb, &bi)) != VK_SUCCESS)
        return p_fail("vkBeginCommandBuffer", r);
    if (!reuse) {
        VkBufferImageCopy c;
        p_barrier(cb, src->img, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                  0, VK_ACCESS_TRANSFER_WRITE_BIT,
                  VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
        memset(&c, 0, sizeof c);
        c.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        c.imageSubresource.layerCount = 1;
        c.imageExtent.width = w;
        c.imageExtent.height = h;
        c.imageExtent.depth = 1;
        vk.CmdCopyBufferToImage(cb, P.staging[slot].buf, src->img,
                                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &c);
        p_barrier(cb, src->img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                  VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                  VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT,
                  VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    }
    /* The window image: black bars, then the frame scaled into place. */
    p_barrier(cb, P.img[img], VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
              0, VK_ACCESS_TRANSFER_WRITE_BIT,
              VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    memset(&black, 0, sizeof black);
    black.float32[3] = 1.0f;
    memset(&range, 0, sizeof range);
    range.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    range.levelCount = 1;
    range.layerCount = 1;
    vk.CmdClearColorImage(cb, P.img[img], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &black, 1, &range);
    p_barrier(cb, P.img[img], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
              VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
              VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
              VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    memset(&blit, 0, sizeof blit);
    blit.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    blit.srcSubresource.layerCount = 1;
    blit.srcOffsets[1].x = (int32_t)w;
    blit.srcOffsets[1].y = (int32_t)h;
    blit.srcOffsets[1].z = 1;
    blit.dstSubresource = blit.srcSubresource;
    p_dst_rect(w, h, blit.dstOffsets);
    if (P.transform == VK_SURFACE_TRANSFORM_ROTATE_180_BIT_KHR) {
        /* Pre-rotated by half a turn: a blit with both axes reversed. */
        int32_t t;
        t = blit.dstOffsets[0].x;
        blit.dstOffsets[0].x = (int32_t)P.extent.width - blit.dstOffsets[1].x;
        blit.dstOffsets[1].x = (int32_t)P.extent.width - t;
        t = blit.dstOffsets[0].y;
        blit.dstOffsets[0].y = (int32_t)P.extent.height - blit.dstOffsets[1].y;
        blit.dstOffsets[1].y = (int32_t)P.extent.height - t;
        t = blit.dstOffsets[0].x; blit.dstOffsets[0].x = blit.dstOffsets[1].x; blit.dstOffsets[1].x = t;
        t = blit.dstOffsets[0].y; blit.dstOffsets[0].y = blit.dstOffsets[1].y; blit.dstOffsets[1].y = t;
    }
    vk.CmdBlitImage(cb, src->img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                    P.img[img], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit,
                    P.linear ? VK_FILTER_LINEAR : VK_FILTER_NEAREST);
    if (P.capture && (P.usage & VK_IMAGE_USAGE_TRANSFER_SRC_BIT)) {
        VkBufferImageCopy c;
        VkDeviceSize need = (VkDeviceSize)P.extent.width * P.extent.height * 4;
        if (P.cap.size < need) {
            p_buf_free(&P.cap);
            if (!p_buf(&P.cap, need, VK_BUFFER_USAGE_TRANSFER_DST_BIT, 1))
                P.capture = 0;
        }
        if (P.capture) {
            p_barrier(cb, P.img[img], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                      VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                      VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT,
                      VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
            memset(&c, 0, sizeof c);
            c.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            c.imageSubresource.layerCount = 1;
            c.imageExtent.width = P.extent.width;
            c.imageExtent.height = P.extent.height;
            c.imageExtent.depth = 1;
            vk.CmdCopyImageToBuffer(cb, P.img[img], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                    P.cap.buf, 1, &c);
            p_barrier(cb, P.img[img], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                      VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                      VK_ACCESS_TRANSFER_READ_BIT, 0,
                      VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
            P.cap_w = P.extent.width;
            P.cap_h = P.extent.height;
        }
    }
    if (!(P.capture && (P.usage & VK_IMAGE_USAGE_TRANSFER_SRC_BIT)))
        p_barrier(cb, P.img[img], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                  VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                  VK_ACCESS_TRANSFER_WRITE_BIT, 0,
                  VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
    if ((r = vk.EndCommandBuffer(cb)) != VK_SUCCESS)
        return p_fail("vkEndCommandBuffer", r);

    memset(&si, 0, sizeof si);
    si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.waitSemaphoreCount = 1;
    si.pWaitSemaphores = &P.acq[slot];
    si.pWaitDstStageMask = &wait_stage;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cb;
    si.signalSemaphoreCount = 1;
    si.pSignalSemaphores = &P.done[img];
    memset(&pi, 0, sizeof pi);
    pi.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    pi.waitSemaphoreCount = 1;
    pi.pWaitSemaphores = &P.done[img];
    pi.swapchainCount = 1;
    pi.pSwapchains = &P.swapchain;
    pi.pImageIndices = &img;
    pthread_mutex_lock(&s_qlock);
    r = vk.QueueSubmit(V.queue, 1, &si, P.fence[slot]);
    if (r == VK_SUCCESS) {
        VkResult pr = vk.QueuePresentKHR(V.queue, &pi);
        pthread_mutex_unlock(&s_qlock);
        P.pending[slot] = 1;
        P.cap_slot = (int)slot;
        P.cap_valid = P.capture && (P.usage & VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
        P.presents++;
        P.frame++;
        if (pr == VK_ERROR_OUT_OF_DATE_KHR) {
            P.need_swapchain = 1;
        } else if (pr == VK_SUBOPTIMAL_KHR) {
            /* Android reports a rotated display as suboptimal on every
             * present when the frames are not pre-rotated (the compositor
             * rotates them): rebuild only when the size changed. */
            VkSurfaceCapabilitiesKHR caps;
            if (vk.GetPhysicalDeviceSurfaceCapabilitiesKHR(V.phys, P.surface, &caps) == VK_SUCCESS
                && caps.currentExtent.width != 0xFFFFFFFFu
                && (caps.currentExtent.width != P.extent.width
                    || caps.currentExtent.height != P.extent.height))
                P.need_swapchain = 1;
        } else if (pr == VK_ERROR_SURFACE_LOST_KHR) {
            p_surface_destroy();
        } else if (pr != VK_SUCCESS) {
            return p_fail("vkQueuePresentKHR", pr);
        }
        return 0;
    }
    pthread_mutex_unlock(&s_qlock);
    return p_fail("vkQueueSubmit (present)", r);
}

static int off(const char *fmt, ...)
{
    va_list ap;
    int n = snprintf(V.status, sizeof V.status, "off: ");
    va_start(ap, fmt);
    vsnprintf(V.status + n, sizeof V.status - (size_t)n, fmt, ap);
    va_end(ap);
    return -1;
}

static int load_loader(void)
{
    const char *names[4];
    int i, n = 0;

    names[n++] = getenv("RECOMP_VK_LIB");
#ifdef __ANDROID__
    names[n++] = "libvulkan.so";
    names[n++] = "libvulkan.so.1";
#else
    names[n++] = "libvulkan.so.1";
    names[n++] = "libvulkan.so";
#endif
    for (i = 0; i < n && !V.lib; i++)
        if (names[i] && *names[i])
            V.lib = dlopen(names[i], RTLD_NOW | RTLD_LOCAL);
    if (!V.lib)
        return off("no Vulkan loader (%s)", dlerror());
    vk.GetInstanceProcAddr = (PFN_vkGetInstanceProcAddr)dlsym(V.lib, "vkGetInstanceProcAddr");
    if (!vk.GetInstanceProcAddr)
        return off("the Vulkan loader has no vkGetInstanceProcAddr");
#define NVK_GLOBAL(n) vk.n = (PFN_vk##n)vk.GetInstanceProcAddr(VK_NULL_HANDLE, "vk" #n);
    NVK_GLOBAL(CreateInstance)
    NVK_GLOBAL(EnumerateInstanceExtensionProperties)
    NVK_GLOBAL(EnumerateInstanceLayerProperties)
#undef NVK_GLOBAL
    if (!vk.CreateInstance || !vk.EnumerateInstanceExtensionProperties)
        return off("the Vulkan loader lacks vkCreateInstance");
    return 0;
}

static int create_instance(void)
{
    VkApplicationInfo app;
    VkInstanceCreateInfo ci;
    const char *exts[2 + 8], *layers[1];
    uint32_t next = 0, nlay = 0, n = 0, i;
    VkExtensionProperties *e = NULL;
    VkResult r;
    const char *val = getenv("RECOMP_VK_VALIDATION");
    int surface_exts = 0;

    memset(&app, 0, sizeof app);
    app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app.pApplicationName = "xboxrecomp";
    app.pEngineName = "xboxrecomp-nv2a";
    app.apiVersion = VK_API_VERSION_1_0;
    memset(&ci, 0, sizeof ci);
    ci.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    ci.pApplicationInfo = &app;
    if (vk.EnumerateInstanceExtensionProperties(NULL, &n, NULL) == VK_SUCCESS && n) {
        e = (VkExtensionProperties *)calloc(n, sizeof *e);
        if (e && vk.EnumerateInstanceExtensionProperties(NULL, &n, e) == VK_SUCCESS) {
            if (has_ext(e, n, "VK_KHR_portability_enumeration")) {
                exts[next++] = "VK_KHR_portability_enumeration";
                ci.flags |= 0x00000001;   /* VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR */
            }
            /* The window's surface extensions (VK_KHR_surface and the
             * platform's, e.g. VK_KHR_android_surface): all or none, each
             * tested first, so a missing one costs the presenter only. */
            if (P.wanted) {
                const char *missing = NULL;
                for (i = 0; i < P.next && !missing; i++)
                    if (!has_ext(e, n, P.exts[i]))
                        missing = P.exts[i];
                if (missing) {
                    p_off("the Vulkan instance has no %s", missing);
                } else {
                    for (i = 0; i < P.next; i++)
                        exts[next++] = P.exts[i];
                    surface_exts = 1;
                }
            }
        }
        free(e);
    } else if (P.wanted) {
        p_off("the Vulkan instance reports no extensions");
    }
    if (val && strcmp(val, "0") && vk.EnumerateInstanceLayerProperties) {
        uint32_t ln = 0, i;
        VkLayerProperties *lp;
        vk.EnumerateInstanceLayerProperties(&ln, NULL);
        lp = (VkLayerProperties *)calloc(ln ? ln : 1, sizeof *lp);
        if (lp && vk.EnumerateInstanceLayerProperties(&ln, lp) == VK_SUCCESS)
            for (i = 0; i < ln; i++)
                if (!strcmp(lp[i].layerName, "VK_LAYER_KHRONOS_validation"))
                    layers[nlay++] = "VK_LAYER_KHRONOS_validation";
        free(lp);
        if (!nlay)
            nvk_log("RECOMP_VK_VALIDATION: VK_LAYER_KHRONOS_validation is not installed");
    }
    ci.enabledExtensionCount = next;
    ci.ppEnabledExtensionNames = next ? exts : NULL;
    ci.enabledLayerCount = nlay;
    ci.ppEnabledLayerNames = nlay ? layers : NULL;
    r = vk.CreateInstance(&ci, NULL, &V.inst);
    if (r != VK_SUCCESS)
        return off("vkCreateInstance failed (VkResult %d)", (int)r);
#define NVK_IFN(n) vk.n = (PFN_vk##n)vk.GetInstanceProcAddr(V.inst, "vk" #n); \
    if (!vk.n) return off("vk" #n " missing");
    NVK_INSTANCE_FNS(NVK_IFN)
#undef NVK_IFN
    if (surface_exts) {
        int all = 1;
#define NVK_SFN(n) vk.n = (PFN_vk##n)vk.GetInstanceProcAddr(V.inst, "vk" #n); \
        if (!vk.n) all = 0;
        NVK_SURFACE_FNS(NVK_SFN)
#undef NVK_SFN
        if (all)
            P.have_surface_ext = 1;
        else
            p_off("the Vulkan loader lacks the VK_KHR_surface entry points");
    }
    return 0;
}

static int queue_family(VkPhysicalDevice pd, uint32_t *out)
{
    VkQueueFamilyProperties q[16];
    uint32_t n = 16, i;
    vk.GetPhysicalDeviceQueueFamilyProperties(pd, &n, q);
    for (i = 0; i < n; i++)
        if (q[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) {
            *out = i;
            return 1;
        }
    return 0;
}

static int device_usable(VkPhysicalDevice pd, uint32_t *qf)
{
    return queue_family(pd, qf)
        && fmt_has(pd, VK_FORMAT_B8G8R8A8_UNORM,
                   VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT
                       | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT
                       | VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT
                       | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT)
        && (fmt_has(pd, VK_FORMAT_D24_UNORM_S8_UINT,
                    VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT)
            || fmt_has(pd, VK_FORMAT_D32_SFLOAT_S8_UINT,
                       VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT));
}

/* Whether `pd`'s graphics queue family can present to the window. */
static int can_present(VkPhysicalDevice pd)
{
    uint32_t qf;
    VkBool32 sup = VK_FALSE;
    if (!P.surface || !queue_family(pd, &qf))
        return 0;
    return vk.GetPhysicalDeviceSurfaceSupportKHR(pd, qf, P.surface, &sup) == VK_SUCCESS && sup;
}

static int pick_device(void)
{
    VkPhysicalDevice pds[16];
    uint32_t n = 16, i, qf = 0;
    const char *want = getenv("RECOMP_VK_DEVICE");
    int best = -1, best_score = -1;
    VkResult r = vk.EnumeratePhysicalDevices(V.inst, &n, pds);

    if ((r != VK_SUCCESS && r != VK_INCOMPLETE) || !n)
        return off("no Vulkan device (VkResult %d, %u devices)", (int)r, n);
    if (want && *want) {
        uint32_t k = (uint32_t)atoi(want);
        if (k >= n)
            return off("RECOMP_VK_DEVICE=%u but there are %u devices", k, n);
        if (!device_usable(pds[k], &qf))
            return off("device %u lacks a graphics queue or the colour/depth formats", k);
        best = (int)k;
    } else {
        for (i = 0; i < n; i++) {
            VkPhysicalDeviceProperties p;
            int score;
            if (!device_usable(pds[i], &qf))
                continue;
            vk.GetPhysicalDeviceProperties(pds[i], &p);
            score = p.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU
                 || p.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU ? 3
                  : p.deviceType == VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU ? 2 : 1;
            /* A window to show frames in: one GPU does both, and frames
             * never cross devices. */
            if (can_present(pds[i]))
                score += 4;
            if (score > best_score) {
                best = (int)i;
                best_score = score;
            }
        }
        if (best < 0)
            return off("none of %u Vulkan devices has a graphics queue and the"
                       " B8G8R8A8 colour and D24S8/D32S8 depth formats", n);
    }
    V.phys = pds[best];
    queue_family(V.phys, &V.qfam);
    P.can_present = can_present(V.phys);
    return 0;
}

static int create_device(char *feat, size_t featn)
{
    VkDeviceQueueCreateInfo q;
    VkDeviceCreateInfo ci;
    VkPhysicalDeviceFeatures have, want;
    VkExtensionProperties *e = NULL;
    const char *exts[2];
    uint32_t n = 0, next = 0;
    float prio = 1.0f;
    VkResult r;
    int portability = 0, swapchain = 0;

    vk.GetPhysicalDeviceFeatures(V.phys, &have);
    memset(&want, 0, sizeof want);
    if (have.depthClamp)
        want.depthClamp = VK_TRUE;
    V.depth_clamp = have.depthClamp ? 1 : 0;
    if (vk.EnumerateDeviceExtensionProperties(V.phys, NULL, &n, NULL) == VK_SUCCESS && n) {
        e = (VkExtensionProperties *)calloc(n, sizeof *e);
        if (e && vk.EnumerateDeviceExtensionProperties(V.phys, NULL, &n, e) == VK_SUCCESS) {
            if (has_ext(e, n, "VK_KHR_portability_subset")) {
                exts[next++] = "VK_KHR_portability_subset";   /* required when offered */
                portability = 1;
            }
            /* Only with a window that this device's queue can present to. */
            if (P.surface && P.can_present && has_ext(e, n, "VK_KHR_swapchain")) {
                exts[next++] = "VK_KHR_swapchain";
                swapchain = 1;
            }
        }
        free(e);
    }
    memset(&q, 0, sizeof q);
    q.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    q.queueFamilyIndex = V.qfam;
    q.queueCount = 1;
    q.pQueuePriorities = &prio;
    memset(&ci, 0, sizeof ci);
    ci.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    ci.queueCreateInfoCount = 1;
    ci.pQueueCreateInfos = &q;
    ci.enabledExtensionCount = next;
    ci.ppEnabledExtensionNames = next ? exts : NULL;
    ci.pEnabledFeatures = &want;
    r = vk.CreateDevice(V.phys, &ci, NULL, &V.dev);
    if (r != VK_SUCCESS)
        return off("vkCreateDevice failed (VkResult %d)", (int)r);
#define NVK_DFN(n) vk.n = (PFN_vk##n)vk.GetDeviceProcAddr(V.dev, "vk" #n); \
    if (!vk.n) return off("vk" #n " missing");
    NVK_DEVICE_FNS(NVK_DFN)
#undef NVK_DFN
    if (swapchain) {
        int all = 1;
#define NVK_SCFN(n) vk.n = (PFN_vk##n)vk.GetDeviceProcAddr(V.dev, "vk" #n); \
        if (!vk.n) all = 0;
        NVK_SWAPCHAIN_FNS(NVK_SCFN)
#undef NVK_SCFN
        P.have_swapchain_ext = all;
    }
    vk.GetDeviceQueue(V.dev, V.qfam, 0, &V.queue);
    snprintf(feat, featn, "depthClamp %s%s", V.depth_clamp ? "yes" : "no",
             portability ? ", portability subset" : "");
    return 0;
}

static int create_objects(void)
{
    VkCommandPoolCreateInfo cp;
    VkCommandBufferAllocateInfo ca;
    VkFenceCreateInfo fi;
    VkAttachmentDescription att[2];
    VkAttachmentReference cref, dref;
    VkSubpassDescription sub;
    VkSubpassDependency dep[2];
    VkRenderPassCreateInfo rp;
    VkDescriptorSetLayoutBinding bind[5];
    VkDescriptorSetLayoutCreateInfo dl;
    VkPushConstantRange pr;
    VkPipelineLayoutCreateInfo pl;
    VkDescriptorPoolSize ps[2];
    VkDescriptorPoolCreateInfo dp;
    VkShaderModuleCreateInfo sm;
    VkPipelineCacheCreateInfo pc;
    int i;

    memset(&cp, 0, sizeof cp);
    cp.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    cp.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
    cp.queueFamilyIndex = V.qfam;
    if (!ck(vk.CreateCommandPool(V.dev, &cp, NULL, &V.cpool), "vkCreateCommandPool"))
        return 0;
    memset(&ca, 0, sizeof ca);
    ca.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    ca.commandPool = V.cpool;
    ca.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ca.commandBufferCount = 1;
    if (!ck(vk.AllocateCommandBuffers(V.dev, &ca, &V.cmd), "vkAllocateCommandBuffers"))
        return 0;
    memset(&fi, 0, sizeof fi);
    fi.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    if (!ck(vk.CreateFence(V.dev, &fi, NULL, &V.fence), "vkCreateFence"))
        return 0;

    /* One render pass for every target: B8G8R8A8 colour and the depth
     * format, contents kept on both sides (clears are explicit). */
    memset(att, 0, sizeof att);
    att[0].format = VK_FORMAT_B8G8R8A8_UNORM;
    att[0].samples = VK_SAMPLE_COUNT_1_BIT;
    att[0].loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    att[0].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    att[0].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    att[0].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    att[0].initialLayout = att[0].finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    att[1].format = V.depth_fmt;
    att[1].samples = VK_SAMPLE_COUNT_1_BIT;
    att[1].loadOp = att[1].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    att[1].storeOp = att[1].stencilStoreOp = VK_ATTACHMENT_STORE_OP_STORE;
    att[1].initialLayout = att[1].finalLayout
        = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    cref.attachment = 0;
    cref.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    dref.attachment = 1;
    dref.layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    memset(&sub, 0, sizeof sub);
    sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    sub.colorAttachmentCount = 1;
    sub.pColorAttachments = &cref;
    sub.pDepthStencilAttachment = &dref;
    memset(dep, 0, sizeof dep);
    dep[0].srcSubpass = VK_SUBPASS_EXTERNAL;
    dep[0].dstSubpass = 0;
    dep[0].srcStageMask = VK_PIPELINE_STAGE_TRANSFER_BIT
                        | VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT
                        | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
    dep[0].srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT
                         | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT
                         | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    dep[0].dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT
                        | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT
                        | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT
                        | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    dep[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT
                         | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT
                         | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT
                         | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT
                         | VK_ACCESS_SHADER_READ_BIT;
    dep[1].srcSubpass = 0;
    dep[1].dstSubpass = VK_SUBPASS_EXTERNAL;
    dep[1].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT
                        | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
    dep[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT
                         | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    dep[1].dstStageMask = VK_PIPELINE_STAGE_TRANSFER_BIT
                        | VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT
                        | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
    dep[1].dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT
                         | VK_ACCESS_COLOR_ATTACHMENT_READ_BIT
                         | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT
                         | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT
                         | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    memset(&rp, 0, sizeof rp);
    rp.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    rp.attachmentCount = 2;
    rp.pAttachments = att;
    rp.subpassCount = 1;
    rp.pSubpasses = &sub;
    rp.dependencyCount = 2;
    rp.pDependencies = dep;
    if (!ck(vk.CreateRenderPass(V.dev, &rp, NULL, &V.rp), "vkCreateRenderPass"))
        return 0;

    memset(bind, 0, sizeof bind);
    bind[0].binding = 0;
    bind[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
    bind[0].descriptorCount = 1;
    bind[0].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    for (i = 1; i < 5; i++) {
        bind[i].binding = (uint32_t)i;
        bind[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        bind[i].descriptorCount = 1;
        bind[i].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    }
    memset(&dl, 0, sizeof dl);
    dl.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    dl.bindingCount = 5;
    dl.pBindings = bind;
    if (!ck(vk.CreateDescriptorSetLayout(V.dev, &dl, NULL, &V.dsl),
            "vkCreateDescriptorSetLayout"))
        return 0;
    pr.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    pr.offset = 0;
    pr.size = sizeof(NvkPush);
    memset(&pl, 0, sizeof pl);
    pl.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pl.setLayoutCount = 1;
    pl.pSetLayouts = &V.dsl;
    pl.pushConstantRangeCount = 1;
    pl.pPushConstantRanges = &pr;
    if (!ck(vk.CreatePipelineLayout(V.dev, &pl, NULL, &V.pl), "vkCreatePipelineLayout"))
        return 0;
    ps[0].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
    ps[0].descriptorCount = NVK_MAX_SETS;
    ps[1].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    ps[1].descriptorCount = NVK_MAX_SETS * 4;
    memset(&dp, 0, sizeof dp);
    dp.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    dp.maxSets = NVK_MAX_SETS;
    dp.poolSizeCount = 2;
    dp.pPoolSizes = ps;
    if (!ck(vk.CreateDescriptorPool(V.dev, &dp, NULL, &V.dpool), "vkCreateDescriptorPool"))
        return 0;

    memset(&sm, 0, sizeof sm);
    sm.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    sm.codeSize = sizeof nv2a_vk_vert_spv;
    sm.pCode = nv2a_vk_vert_spv;
    if (!ck(vk.CreateShaderModule(V.dev, &sm, NULL, &V.vs), "vkCreateShaderModule"))
        return 0;
    sm.codeSize = sizeof nv2a_vk_frag_spv;
    sm.pCode = nv2a_vk_frag_spv;
    if (!ck(vk.CreateShaderModule(V.dev, &sm, NULL, &V.fs), "vkCreateShaderModule"))
        return 0;
    memset(&pc, 0, sizeof pc);
    pc.sType = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO;
    if (V.pc_path[0]) {
        size_t n;
        void *data = pc_load(&n);
        if (data) {
            pc.initialDataSize = n;
            pc.pInitialData = data;
            if (vk.CreatePipelineCache(V.dev, &pc, NULL, &V.pcache) == VK_SUCCESS) {
                V.st.pcache_load_bytes = n;
                if (n >= 10240)
                    snprintf(V.pc_state, sizeof V.pc_state, "%zu KB loaded", n >> 10);
                else
                    snprintf(V.pc_state, sizeof V.pc_state, "%zu bytes loaded", n);
            } else {
                V.pcache = VK_NULL_HANDLE;
                snprintf(V.pc_state, sizeof V.pc_state, "new (the driver refused the file)");
            }
            free(data);
            pc.initialDataSize = 0;
            pc.pInitialData = NULL;
        }
    } else {
        snprintf(V.pc_state, sizeof V.pc_state, "off");
    }
    if (!V.pcache
        && !ck(vk.CreatePipelineCache(V.dev, &pc, NULL, &V.pcache), "vkCreatePipelineCache"))
        return 0;

    if (!buf_create(&V.vring, NVK_VERT_RING, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, 0)
        || !buf_create(&V.uring, NVK_UBO_RING, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, 0)
        || !buf_create(&V.staging, NVK_STAGING, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, 0)
        || !buf_create(&V.rbuf, 8u << 20, VK_BUFFER_USAGE_TRANSFER_DST_BIT, 1))
        return 0;

    /* What an unusable stage reads: white (rc_texel). */
    {
        VkDeviceSize o;
        uint8_t *p;
        VkBufferImageCopy c;
        if (!img_create(&V.white, 1, 1, 1, VK_FORMAT_B8G8R8A8_UNORM,
                        VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                        VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_VIEW_TYPE_2D_ARRAY))
            return 0;
        p = staging_alloc(4, &o);
        if (!p || !cmd_begin())
            return 0;
        memset(p, 0xFF, 4);
        img_barrier(&V.white, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        memset(&c, 0, sizeof c);
        c.bufferOffset = o;
        c.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        c.imageSubresource.layerCount = 1;
        c.imageExtent.width = c.imageExtent.height = c.imageExtent.depth = 1;
        vk.CmdCopyBufferToImage(V.cmd, V.staging.buf, V.white.img,
                                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &c);
        img_barrier(&V.white, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        if (!submit_wait())
            return 0;
    }
    return 1;
}

static void read_env(void)
{
    const char *e;
    e = getenv("RECOMP_VK_CULL");
    V.cull = e && strcmp(e, "0") != 0;
    e = getenv("RECOMP_VK_CACHE");
    if (e && *e && strcmp(e, "0") != 0)
        snprintf(V.pc_path, sizeof V.pc_path, "%s", e);
    e = getenv("RECOMP_VK_STATS");
    V.stats_every = e ? atoi(e) : 0;
    if (e && V.stats_every <= 0 && strcmp(e, "0") != 0)
        V.stats_every = 60;
    /* tex_filter_config in nv2a_pb_exec.c, so both renderers agree. */
    e = getenv("RECOMP_TEX_BILINEAR");
    V.bilinear = e && strcmp(e, "0") != 0;
    e = getenv("RECOMP_TEX_ANISO");
    if (e && atoi(e) >= 8)
        V.bilinear = 1;
    if (V.cull) {
        e = getenv("RECOMP_STRIP_ALT_WINDING");
        if (!e || !strcmp(e, "0"))
            nvk_log("RECOMP_VK_CULL without RECOMP_STRIP_ALT_WINDING: every second"
                    " triangle of a strip winds the other way and may be culled");
    }
}

/* The instance, the window's surface (when a presenter asked for one) and
 * the device: what the renderer and the presenter share. Idempotent. */
static int nvk_device_up(void)
{
    VkPhysicalDeviceProperties p;

    if (V.dev)
        return 0;
    if (V.inst || V.lib) {
        /* A failed attempt left the instance or the loader: start over. */
        p_surface_destroy();
        if (V.inst && vk.DestroyInstance)
            vk.DestroyInstance(V.inst, NULL);
        if (V.lib)
            dlclose(V.lib);
    }
    memset(&V, 0, sizeof V);
    V.init_tried = 1;
    read_env();
    if (load_loader() || create_instance())
        return -1;
    if (P.wanted && P.have_surface_ext && !P.surface)
        p_surface_create();                 /* failure: the presenter is off */
    if (pick_device())
        return -1;
    vk.GetPhysicalDeviceProperties(V.phys, &p);
    vk.GetPhysicalDeviceMemoryProperties(V.phys, &V.mem);
    V.ubo_align = p.limits.minUniformBufferOffsetAlignment;
    if (V.ubo_align < 16)
        V.ubo_align = 16;
    V.depth_fmt = fmt_has(V.phys, VK_FORMAT_D24_UNORM_S8_UINT,
                          VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT)
                ? VK_FORMAT_D24_UNORM_S8_UINT : VK_FORMAT_D32_SFLOAT_S8_UINT;
    if (create_device(V.feat, sizeof V.feat))
        return -1;
    snprintf(V.devdesc, sizeof V.devdesc, "Vulkan %u.%u.%u on %s (%s, driver 0x%08X, vendor 0x%04X)",
             VK_VERSION_MAJOR(p.apiVersion), VK_VERSION_MINOR(p.apiVersion),
             VK_VERSION_PATCH(p.apiVersion), p.deviceName, dev_type(p.deviceType),
             p.driverVersion, p.vendorID);
    return 0;
}

/* The renderer's objects on top of the device. */
static int nvk_renderer_up(void)
{
    V.renderer_tried = 1;
    V.ok = 1;
    if (!create_objects()) {
        V.ok = 0;
        return -1;                          /* nvk_fail wrote the status */
    }
    snprintf(V.status, sizeof V.status, "%s, depth %s, %s, pipeline cache %s", V.devdesc,
             V.depth_fmt == VK_FORMAT_D24_UNORM_S8_UINT ? "D24S8" : "D32S8", V.feat, V.pc_state);
    nvk_log("%s", V.status);
    return 0;
}

/* The device and the presenter are gone together, by whichever of the
 * renderer and the presenter shuts down last (s_life). */
static pthread_mutex_t s_life = PTHREAD_MUTEX_INITIALIZER;
static int s_renderer_down = 1, s_presenter_down = 1;

static void nvk_device_down(void)
{
    if (V.dev) {
        dev_idle();
        vk.DestroyDevice(V.dev, NULL);
    }
    p_surface_destroy();
    if (V.inst && vk.DestroyInstance)
        vk.DestroyInstance(V.inst, NULL);
    if (V.lib)
        dlclose(V.lib);
    V.dev = VK_NULL_HANDLE;
    V.inst = VK_NULL_HANDLE;
    V.lib = NULL;
}

/* ---- API ---------------------------------------------------------------------- */

int nv2a_vk_register(void)
{
    if (!V.ok) {
        int rc;
        if (V.renderer_tried && V.dev)
            return -1;                      /* failed mid-run: stay on the CPU */
        pthread_mutex_lock(&s_life);
        rc = nvk_device_up();
        if (rc == 0) {
            rc = nvk_renderer_up();
            if (rc == 0)
                s_renderer_down = 0;
        }
        pthread_mutex_unlock(&s_life);
        if (rc != 0) {
            nvk_log("%s", V.status);
            return -1;
        }
    }
    nv2a_backend_register_with_present_sync(&s_vk_backend, vk_present_sync);
    V.registered = 1;
    return 0;
}

const char *nv2a_vk_status(void)
{
    return V.status[0] ? V.status : "off: not initialised";
}

void nv2a_vk_sync(void)
{
    if (V.ok && surf_sync_all())
        submit_wait();
}

void nv2a_vk_get_stats(Nv2aVkStats *out)
{
    *out = V.st;
}

void nv2a_vk_shutdown(void)
{
    int i, a, b, c;

    if (V.registered) {
        nv2a_backend_register(NULL);
        V.registered = 0;
    }
    if (V.dev) {
        if (V.ok) {
            nv2a_vk_sync();
            if (V.pc_dirty)
                pc_save("shutdown");
        }
        dev_idle();
        V.recording = 0;
        for (i = 0; i < V.dead_img_n; i++)
            img_destroy_now(&V.dead_img[i]);
        for (i = 0; i < V.dead_fb_n; i++)
            vk.DestroyFramebuffer(V.dev, V.dead_fb[i], NULL);
        for (i = 0; i < NVK_MAX_SURF; i++)
            if (V.surf[i].used) {
                for (a = 0; a < NVK_FB_PER_SURF; a++)
                    if (V.surf[i].fbs[a].fb)
                        vk.DestroyFramebuffer(V.dev, V.surf[i].fbs[a].fb, NULL);
                img_destroy_now(&V.surf[i].im);
            }
        for (i = 0; i < NVK_MAX_DEPTH; i++)
            img_destroy_now(&V.depth[i].im);
        tex_free_all();
        img_destroy_now(&V.white);
        for (a = 0; a < 2; a++)
            for (b = 0; b < 3; b++)
                for (c = 0; c < 3; c++)
                    if (V.samplers[a][b][c])
                        vk.DestroySampler(V.dev, V.samplers[a][b][c], NULL);
        for (i = 0; i < (int)V.pipe_cap; i++)
            if (V.pipes[i].used)
                vk.DestroyPipeline(V.dev, V.pipes[i].pipe, NULL);
        buf_destroy(&V.vring);
        buf_destroy(&V.uring);
        buf_destroy(&V.staging);
        buf_destroy(&V.rbuf);
        if (V.pcache) vk.DestroyPipelineCache(V.dev, V.pcache, NULL);
        if (V.vs) vk.DestroyShaderModule(V.dev, V.vs, NULL);
        if (V.fs) vk.DestroyShaderModule(V.dev, V.fs, NULL);
        if (V.dpool) vk.DestroyDescriptorPool(V.dev, V.dpool, NULL);
        if (V.pl) vk.DestroyPipelineLayout(V.dev, V.pl, NULL);
        if (V.dsl) vk.DestroyDescriptorSetLayout(V.dev, V.dsl, NULL);
        if (V.rp) vk.DestroyRenderPass(V.dev, V.rp, NULL);
        if (V.fence) vk.DestroyFence(V.dev, V.fence, NULL);
        if (V.cpool) vk.DestroyCommandPool(V.dev, V.cpool, NULL);
    }
    free(V.pipes);
    free(V.dead_img);
    free(V.dead_fb);
    pthread_mutex_lock(&s_life);
    s_renderer_down = 1;
    if (s_presenter_down) {
        nvk_device_down();
        memset(&V, 0, sizeof V);
    } else {
        /* The window still presents on this device: only the renderer's
         * objects go (create_objects can bring them back). */
        VkInstance inst = V.inst;
        VkPhysicalDevice phys = V.phys;
        VkDevice dev = V.dev;
        VkQueue queue = V.queue;
        uint32_t qfam = V.qfam;
        VkPhysicalDeviceMemoryProperties mem = V.mem;
        void *lib = V.lib;
        char devdesc[sizeof V.devdesc], feat[sizeof V.feat];
        VkDeviceSize ubo_align = V.ubo_align;
        VkFormat depth_fmt = V.depth_fmt;
        int depth_clamp = V.depth_clamp;
        memcpy(devdesc, V.devdesc, sizeof devdesc);
        memcpy(feat, V.feat, sizeof feat);
        memset(&V, 0, sizeof V);
        V.inst = inst; V.phys = phys; V.dev = dev; V.queue = queue; V.qfam = qfam;
        V.mem = mem; V.lib = lib; V.ubo_align = ubo_align; V.depth_fmt = depth_fmt;
        V.depth_clamp = depth_clamp; V.init_tried = 1;
        memcpy(V.devdesc, devdesc, sizeof devdesc);
        memcpy(V.feat, feat, sizeof feat);
        read_env();
    }
    pthread_mutex_unlock(&s_life);
    snprintf(V.status, sizeof V.status, "off: shut down");
}

/* ---- window presentation API (nv2a_vk.h) ------------------------------------ */

int nv2a_vk_present_init(const char *const *instance_exts, unsigned n_exts,
                         Nv2aVkSurfaceFn create_surface, void *ctx,
                         unsigned width, unsigned height)
{
    unsigned i;
    const char *e;
    char keep[sizeof P.status];

    if (P.ok)
        return 0;
    memset(&P, 0, sizeof P);
    P.cap_slot = -1;
    p_read_env();
    e = getenv("RECOMP_PRESENT_CAPTURE");
    P.capture = e && strcmp(e, "0") != 0;
    if (!create_surface || n_exts > sizeof P.exts / sizeof P.exts[0])
        return p_off("bad arguments");
    for (i = 0; i < n_exts; i++)
        P.exts[i] = instance_exts[i];
    P.next = n_exts;
    P.create_surface = create_surface;
    P.ctx = ctx;
    P.want_w = width ? width : 640;
    P.want_h = height ? height : 480;
    pthread_mutex_lock(&s_life);
    if (V.dev) {
        pthread_mutex_unlock(&s_life);
        return p_off("the Vulkan device is already up, without the window's extensions"
                     " (nv2a_vk_present_init must come first)");
    }
    P.wanted = 1;
    if (nvk_device_up() != 0) {
        if (!P.status[0] || P.ok)
            p_off("%s", V.status[0] ? V.status : "no Vulkan device");
        else if (strncmp(V.status, "off: ", 5) == 0) {
            /* Both reasons: the device's, then the presenter's. */
            snprintf(keep, sizeof keep, "%s", P.status);
            snprintf(P.status, sizeof P.status, "%.180s (%.180s)", keep, V.status + 5);
        }
        P.wanted = 0;
        P.ok = 0;
        pthread_mutex_unlock(&s_life);
        return -1;
    }
    s_presenter_down = 0;
    p_setup();
    if (!P.ok) {
        snprintf(keep, sizeof keep, "%s", P.status);
        p_teardown();                       /* the device stays, for the renderer */
        memcpy(P.status, keep, sizeof keep);
        s_presenter_down = 1;
        pthread_mutex_unlock(&s_life);
        return -1;
    }
    pthread_mutex_unlock(&s_life);
    nvk_log("presenter on %s: %s", V.devdesc, P.status);
    return 0;
}

const char *nv2a_vk_present_status(void)
{
    return P.status[0] ? P.status : "off: not requested";
}

int nv2a_vk_present_frame(const void *pixels, unsigned width, unsigned height,
                          unsigned pitch, unsigned bpp)
{
    int r;
    if (!P.ok)
        return -1;
    r = p_present(pixels, width, height, pitch, bpp);
    if (r < 0) {
        /* Off for good: release the window now, so whatever presents next
         * can connect to it. */
        char keep[sizeof P.status];
        snprintf(keep, sizeof keep, "%s", P.status);
        pthread_mutex_lock(&s_life);
        p_teardown();
        s_presenter_down = 1;
        pthread_mutex_unlock(&s_life);
        memcpy(P.status, keep, sizeof keep);
    }
    return r;
}

void nv2a_vk_present_resize(unsigned width, unsigned height)
{
    if (!P.ok)
        return;
    if (width && height) {
        P.want_w = width;
        P.want_h = height;
    }
    P.need_swapchain = 1;
}

void nv2a_vk_present_suspend(void)
{
    if (!P.ok || P.suspended)
        return;
    p_surface_destroy();
    P.suspended = 1;
    P.suspends++;
}

void nv2a_vk_present_resume(void)
{
    if (!P.ok)
        return;
    P.suspended = 0;
    P.need_swapchain = 1;                  /* the surface comes back on the next frame */
}

void nv2a_vk_present_get_stats(Nv2aVkPresentStats *out)
{
    memset(out, 0, sizeof *out);
    out->presents = P.presents;
    out->skipped = P.skipped;
    out->swapchains = P.swapchains;
    out->suspends = P.suspends;
    out->width = P.extent.width;
    out->height = P.extent.height;
    out->active = P.ok && P.swapchain != VK_NULL_HANDLE;
}

int nv2a_vk_present_capture(uint8_t *rgba, unsigned max_bytes, unsigned *width, unsigned *height)
{
    uint32_t i, n;
    const uint8_t *src;
    int swap;

    if (!P.ok || !P.cap_valid || !P.cap.map)
        return -1;
    p_wait_slots();
    n = P.cap_w * P.cap_h;
    if ((uint64_t)n * 4 > max_bytes)
        return -1;
    src = (const uint8_t *)P.cap.map;
    swap = P.format == VK_FORMAT_B8G8R8A8_UNORM || P.format == VK_FORMAT_B8G8R8A8_SRGB;
    for (i = 0; i < n; i++) {
        rgba[4 * i + 0] = src[4 * i + (swap ? 2 : 0)];
        rgba[4 * i + 1] = src[4 * i + 1];
        rgba[4 * i + 2] = src[4 * i + (swap ? 0 : 2)];
        rgba[4 * i + 3] = src[4 * i + 3];
    }
    *width = P.cap_w;
    *height = P.cap_h;
    return 0;
}

void nv2a_vk_present_shutdown(void)
{
    pthread_mutex_lock(&s_life);
    if (!s_presenter_down) {
        p_teardown();
        s_presenter_down = 1;
        if (s_renderer_down) {
            nvk_device_down();
            memset(&V, 0, sizeof V);
        }
    }
    pthread_mutex_unlock(&s_life);
    snprintf(P.status, sizeof P.status, "off: shut down");
}
