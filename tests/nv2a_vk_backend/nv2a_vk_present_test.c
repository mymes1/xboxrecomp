/* The Vulkan window presenter (nv2a_vk_present_*, src/vk/nv2a_vk.c) on a
 * headless surface. [android-port]
 *
 * VK_EXT_headless_surface gives a real VkSurfaceKHR and swapchain without a
 * display, so everything the Android window does is exercised: the instance
 * and device are brought up with the surface extensions, frames go through
 * acquire / upload / blit / present, and each presented image is copied
 * back (RECOMP_PRESENT_CAPTURE) and checked. The surface decides the window
 * size (SwiftShader's is a fixed 1280x720; others take the requested size),
 * so the expectations follow the swapchain's size:
 *
 *   1:1 X8R8G8B8   a frame the window's size is shown exactly (alpha forced
 *                  opaque)
 *   1:1 R5G6B5     the 16-bit format, widened (within 1 per channel)
 *   redraw         presenting "the last frame again" shows it again
 *   pillarbox      a 4:3 frame in a wider window: centred, black side bars,
 *                  every block of the test card in its place
 *   letterbox      a 32:9 frame: black bars above and below
 *   resize         a new swapchain, the last frame redrawn into it
 *   suspend        a suspended presenter drops frames; after resume the
 *                  surface is made again through the window's callback
 *   renderer       nv2a_vk_register on the presenter's device: a pushbuffer
 *                  scene the window's size drawn by Vulkan, written back at
 *                  the flip, then presented 1:1 -- the window shows exactly
 *                  what guest memory holds
 *   shutdown       the renderer can shut down first (the presenter keeps the
 *                  device), then the presenter; afterwards frames are refused
 *
 * RECOMP_ASPECT=native for the whole run (the frame's own aspect), so a
 * frame the window's shape fills it 1:1.
 *
 * Exit 77 (ctest SKIP_RETURN_CODE) when there is no Vulkan device or it has
 * no headless surface. */
#define _POSIX_C_SOURCE 200809L         /* setenv */
#include <dlfcn.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <vulkan/vulkan.h>

#include "nv2a_backend.h"
#include "nv2a_vk.h"

#define MEM_BYTES (48u << 20)
static uint8_t *s_mem;

/* ---- what the executor expects the runtime to provide (as nv2a_ffp_backend) */
ptrdiff_t xbox_GetMemoryOffset(void) { return (ptrdiff_t)s_mem; }
uint32_t g_xbox_image_lo, g_xbox_image_hi;
uint32_t g_xbox_stack_base, g_xbox_stack_size;
uint32_t xbox_ContiguousAllocatedBytes(void) { return 0; }
void xbox_FramebufferWindowSet(uint32_t va, uint32_t pitch) { (void)va; (void)pitch; }
void xbox_FramebufferWindowSetAA(uint32_t sy) { (void)sy; }
uint32_t xbox_Nv2aFenceWordVa(void) { return 0; }
int nv2a_pb_dma_walk(void) { return 1; }
void xbox_FramebufferWindowStart(void) {}
void xbox_FramebufferWindowFrameStats(uint32_t draws) { (void)draws; }
void xbox_FramebufferWindowPresent(uint32_t va, uint32_t pitch) { (void)va; (void)pitch; }
void xbox_Nv2aFrameCounterFlip(void) {}
int nv2a_pb_scan_in_call(void) { return 0; }
uint32_t xbox_VblankCount(void) { return 0; }
uint64_t xbox_HostNowNs(void) { return 0; }
uint32_t xbox_Nv2aFrameCountersOwned(void) { return 0; }
static volatile uint32_t s_pgraph_400b10;
volatile uint32_t *xbox_Nv2aRegPtr(uint32_t offset)
{
    return offset == 0x400B10u ? &s_pgraph_400b10 : NULL;
}

extern void nv2a_pb_exec_method(uint32_t subch, uint32_t method, uint32_t param);

/* ---- the "window": a headless surface ---------------------------------------- */

static int s_surfaces_made;

static int make_surface(void *ctx, void *instance, uint64_t *out)
{
    static void *lib;
    PFN_vkGetInstanceProcAddr gipa;
    PFN_vkCreateHeadlessSurfaceEXT create;
    VkHeadlessSurfaceCreateInfoEXT ci;
    VkSurfaceKHR s = VK_NULL_HANDLE;
    const char *name = getenv("RECOMP_VK_LIB");

    (void)ctx;
    if (!lib)
        lib = dlopen(name && *name ? name : "libvulkan.so.1", RTLD_NOW | RTLD_LOCAL);
    if (!lib)
        return -1;
    gipa = (PFN_vkGetInstanceProcAddr)dlsym(lib, "vkGetInstanceProcAddr");
    create = gipa ? (PFN_vkCreateHeadlessSurfaceEXT)gipa((VkInstance)instance,
                                                         "vkCreateHeadlessSurfaceEXT")
                  : NULL;
    if (!create)
        return -1;
    memset(&ci, 0, sizeof ci);
    ci.sType = VK_STRUCTURE_TYPE_HEADLESS_SURFACE_CREATE_INFO_EXT;
    if (create((VkInstance)instance, &ci, NULL, &s) != VK_SUCCESS)
        return -1;
    memcpy(out, &s, sizeof s < sizeof *out ? sizeof s : sizeof *out);
    s_surfaces_made++;
    return 0;
}

/* Whether the loader offers VK_EXT_headless_surface at all (else: skip). */
static int have_headless(void)
{
    void *lib = dlopen(getenv("RECOMP_VK_LIB") && *getenv("RECOMP_VK_LIB")
                           ? getenv("RECOMP_VK_LIB") : "libvulkan.so.1", RTLD_NOW | RTLD_LOCAL);
    PFN_vkGetInstanceProcAddr gipa;
    PFN_vkEnumerateInstanceExtensionProperties en;
    VkExtensionProperties e[128];
    uint32_t n = 128, i;
    int found = 0;

    if (!lib)
        return 0;
    gipa = (PFN_vkGetInstanceProcAddr)dlsym(lib, "vkGetInstanceProcAddr");
    en = gipa ? (PFN_vkEnumerateInstanceExtensionProperties)gipa(
                    VK_NULL_HANDLE, "vkEnumerateInstanceExtensionProperties") : NULL;
    if (en && (en(NULL, &n, e) == VK_SUCCESS || n))
        for (i = 0; i < n; i++)
            if (!strcmp(e[i].extensionName, "VK_EXT_headless_surface"))
                found = 1;
    dlclose(lib);
    return found;
}

/* ---- frames -------------------------------------------------------------------- */

#define BX 8                            /* the test card: 8 x 8 blocks */
#define BY 8
#define MAXW 2048u
#define MAXH 2048u

static uint32_t card_rgb(int bx, int by)
{
    /* Distinct, saturated colours; none is black (the bars). */
    uint32_t r = (uint32_t)(40 + bx * 30), g = (uint32_t)(40 + by * 30);
    uint32_t b = (uint32_t)((bx * 7 + by * 13) % 8 * 30 + 20);
    return (r << 16) | (g << 8) | b;
}

static void make_card32(uint8_t *px, unsigned w, unsigned h)
{
    unsigned x, y;
    for (y = 0; y < h; y++)
        for (x = 0; x < w; x++) {
            /* X byte deliberately not 0xFF: the presenter forces it. */
            uint32_t v = 0x12000000u | card_rgb((int)(x * BX / w), (int)(y * BY / h));
            memcpy(px + ((size_t)y * w + x) * 4, &v, 4);
        }
}

static void make_card16(uint8_t *px, unsigned w, unsigned h)
{
    unsigned x, y;
    for (y = 0; y < h; y++)
        for (x = 0; x < w; x++) {
            /* Every 5- and 6-bit level appears somewhere. */
            uint16_t v = (uint16_t)((((x / 20) & 31) << 11) | (((y / 8) & 63) << 5)
                                    | ((x + y) / 7 & 31));
            memcpy(px + ((size_t)y * w + x) * 2, &v, 2);
        }
}

static uint8_t *s_cap;
#define CAP_MAX (4096u * 4096u * 4u)

static int capture(unsigned *w, unsigned *h)
{
    return nv2a_vk_present_capture(s_cap, CAP_MAX, w, h);
}

static int fails;

static void check(int ok, const char *what, const char *fmt, ...)
{
    va_list ap;
    printf("%s %s", ok ? "PASS" : "FAIL", what);
    if (fmt && *fmt) {
        printf(": ");
        va_start(ap, fmt);
        vprintf(fmt, ap);
        va_end(ap);
    }
    printf("\n");
    if (!ok)
        fails++;
}

static void win_size(unsigned *w, unsigned *h)
{
    Nv2aVkPresentStats ps;
    nv2a_vk_present_get_stats(&ps);
    *w = ps.width;
    *h = ps.height;
}

/* The captured window against `px` (w x h, `pitch`) shown 1:1. */
static int compare_1to1(const uint8_t *px, unsigned fw, unsigned fh, unsigned pitch,
                        uint32_t bpp, int tol, char *why, size_t whyn)
{
    unsigned w = 0, h = 0, x, y;
    if (capture(&w, &h) != 0) {
        snprintf(why, whyn, "no capture");
        return 0;
    }
    if (w != fw || h != fh) {
        snprintf(why, whyn, "captured %ux%u, want %ux%u", w, h, fw, fh);
        return 0;
    }
    for (y = 0; y < h; y++)
        for (x = 0; x < w; x++) {
            const uint8_t *c = s_cap + ((size_t)y * w + x) * 4;
            int want[3], k;
            if (bpp == 4) {
                uint32_t v;
                memcpy(&v, px + (size_t)y * pitch + (size_t)x * 4, 4);
                want[0] = (int)(v >> 16 & 255);
                want[1] = (int)(v >> 8 & 255);
                want[2] = (int)(v & 255);
            } else {
                uint16_t v;
                memcpy(&v, px + (size_t)y * pitch + (size_t)x * 2, 2);
                want[0] = (int)(((v >> 11) & 31) * 255 + 15) / 31;
                want[1] = (int)(((v >> 5) & 63) * 255 + 31) / 63;
                want[2] = (int)((v & 31) * 255 + 15) / 31;
            }
            for (k = 0; k < 3; k++)
                if (abs((int)c[k] - want[k]) > tol) {
                    snprintf(why, whyn, "(%u,%u) channel %d is %d, want %d", x, y, k,
                             c[k], want[k]);
                    return 0;
                }
            if (c[3] != 255) {
                snprintf(why, whyn, "(%u,%u) alpha %d, want 255", x, y, c[3]);
                return 0;
            }
        }
    snprintf(why, whyn, "%ux%u exact%s", w, h, tol ? " within 1" : "");
    return 1;
}

/* A fw x fh test card in the window at its own aspect: bars black, each
 * block's centre the block's colour. `bars` must exist on the sides (1),
 * above and below (2), or either. */
static int check_boxed(unsigned fw, unsigned fh, int bars, char *why, size_t whyn)
{
    unsigned w = 0, h = 0, bx, by;
    double a = (double)fw / fh, dw, dh, x0, y0;
    unsigned pts[4][2];
    int n = 0, i;

    if (capture(&w, &h) != 0) {
        snprintf(why, whyn, "no capture");
        return 0;
    }
    if ((double)w / h > a) {
        dh = h;
        dw = h * a;
    } else {
        dw = w;
        dh = w / a;
    }
    x0 = (w - dw) / 2;
    y0 = (h - dh) / 2;
    if (x0 >= 2) {
        pts[n][0] = (unsigned)(x0 / 2); pts[n++][1] = h / 2;
        pts[n][0] = (unsigned)(w - x0 / 2 - 1); pts[n++][1] = h / 2;
    }
    if (y0 >= 2) {
        pts[n][0] = w / 2; pts[n++][1] = (unsigned)(y0 / 2);
        pts[n][0] = w / 2; pts[n++][1] = (unsigned)(h - y0 / 2 - 1);
    }
    if ((bars == 1 && x0 < 2) || (bars == 2 && y0 < 2)) {
        snprintf(why, whyn, "a %ux%u frame in a %ux%u window should have %s bars", fw, fh, w,
                 h, bars == 1 ? "side" : "top and bottom");
        return 0;
    }
    for (i = 0; i < n; i++) {
        const uint8_t *c = s_cap + ((size_t)pts[i][1] * w + pts[i][0]) * 4;
        if (c[0] || c[1] || c[2]) {
            snprintf(why, whyn, "bar at (%u,%u) is %d,%d,%d, want black", pts[i][0],
                     pts[i][1], c[0], c[1], c[2]);
            return 0;
        }
    }
    for (by = 0; by < BY; by++)
        for (bx = 0; bx < BX; bx++) {
            unsigned cx = (unsigned)(x0 + (bx + 0.5) * dw / BX);
            unsigned cy = (unsigned)(y0 + (by + 0.5) * dh / BY);
            const uint8_t *c = s_cap + ((size_t)cy * w + cx) * 4;
            uint32_t want = card_rgb((int)bx, (int)by);
            if (abs(c[0] - (int)(want >> 16 & 255)) > 2 || abs(c[1] - (int)(want >> 8 & 255)) > 2
                || abs(c[2] - (int)(want & 255)) > 2) {
                snprintf(why, whyn, "block %u,%u at (%u,%u) is %d,%d,%d, want %u,%u,%u", bx, by,
                         cx, cy, c[0], c[1], c[2], want >> 16 & 255, want >> 8 & 255, want & 255);
                return 0;
            }
        }
    snprintf(why, whyn, "%ux%u window, picture %.0fx%.0f at %.0f,%.0f", w, h, dw, dh, x0, y0);
    return 1;
}

/* ---- a pushbuffer scene drawn by the renderer (as nv2a_vk_backend_test) ---- */

#define SURF   0x0100000u               /* A8R8G8B8, pitch sw * 4 */
#define ZETA   0x1200000u
#define VB_POS 0x2400000u
#define VB_COL 0x2410000u

static void m(uint32_t method, uint32_t param) { nv2a_pb_exec_method(0, method, param); }
static uint32_t f2u(float f) { uint32_t u; memcpy(&u, &f, 4); return u; }
static void mf(uint32_t method, float f) { m(method, f2u(f)); }

static void putf(uint32_t at, float f) { memcpy(s_mem + at, &f, 4); }

/* The encodings are nv2a_vk_backend_test.c's (surface_linear, clear, draw). */
static void draw_scene(unsigned sw, unsigned sh)
{
    static const float I[16] = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1 };
    static const float pos[6][2] = {        /* fractions of the surface */
        { .06f, .06f }, { .94f, .17f }, { .31f, .94f }, { .97f, .02f }, { .78f, .98f },
        { .52f, .5f },
    };
    static const uint32_t col[6] = {
        0xFFFF0000u, 0xFF00FF00u, 0xFF0000FFu, 0xFFFFFF00u, 0xFF00FFFFu, 0xFFFF00FFu,
    };
    uint32_t i;

    for (i = 0; i < 16; i++) {
        m(0x1720 + 4 * i, 0);                   /* every attribute off */
        m(0x1760 + 4 * i, 2);
        mf(0x0680 + 4 * i, I[i]);               /* composite: identity */
    }
    m(0x1E94, 0);                               /* TRANSFORM_EXECUTION_MODE: fixed */
    m(0x0304, 0); m(0x0344, 1); m(0x0348, 0); m(0x0350, 0x8006); m(0x034C, 0);
    m(0x0300, 0); m(0x033C, 0x207); m(0x0340, 0);
    m(0x030C, 0); m(0x0354, 0x201); m(0x035C, 0);
    m(0x0358, 0x01010101u);
    m(0x0308, 0);                               /* cull off */
    m(0x032C, 0); m(0x0360, 0xFF); m(0x0364, 0x207); m(0x0368, 0); m(0x036C, 0xFF);
    m(0x0370, 0x1E00); m(0x0374, 0x1E00); m(0x0378, 0x1E00);
    for (i = 0; i < 4; i++)
        m(0x1B00 + 0x40 * i, 0);                /* no texture */
    /* A8R8G8B8 linear, Z24S8, pitch 2560, the whole 640x480 */
    m(0x0208, 0x8u | (2u << 4) | (1u << 8));
    m(0x0200, 0u | (sw << 16));
    m(0x0204, 0u | (sh << 16));
    m(0x020C, (sw * 4) | ((sw * 4) << 16));
    m(0x0210, SURF);
    m(0x0214, ZETA);
    m(0x1D90, 0xFF203040u);                     /* clear colour, Z far */
    m(0x1D8C, 0xFFFFFF00u);
    m(0x1D94, 0xF3);
    for (i = 0; i < 6; i++) {
        putf(VB_POS + 16 * i + 0, pos[i][0] * (float)sw);
        putf(VB_POS + 16 * i + 4, pos[i][1] * (float)sh);
        putf(VB_POS + 16 * i + 8, 0.0f);
        putf(VB_POS + 16 * i + 12, 1.0f);
        memcpy(s_mem + VB_COL + 4 * i, &col[i], 4);
    }
    m(0x1720, VB_POS); m(0x1760, 2 | (4 << 4) | (16 << 8));     /* attr 0: float4 */
    m(0x172C, VB_COL); m(0x176C, 0 | (4 << 4) | (4 << 8));      /* attr 3: D3DCOLOR */
    m(0x17FC, 5);                               /* triangles */
    m(0x1810, 5u << 24);                        /* 6 vertices */
    m(0x17FC, 0);
    m(0x012C, 0);                               /* FLIP_INCREMENT_WRITE */
    m(0x0130, 0);                               /* FLIP_STALL: present_sync */
}

int main(void)
{
    static const char *exts[] = { "VK_KHR_surface", "VK_EXT_headless_surface" };
    uint8_t *f32 = (uint8_t *)malloc((size_t)MAXW * MAXH * 4);
    uint8_t *f16 = (uint8_t *)malloc((size_t)MAXW * MAXH * 2);
    Nv2aVkPresentStats ps;
    Nv2aVkStats st;
    char why[256];
    unsigned sw = 0, sh = 0, w2 = 0, h2 = 0;
    uint64_t chains;
    int r, made;

    s_cap = (uint8_t *)malloc(CAP_MAX);
    s_mem = (uint8_t *)calloc(1, MEM_BYTES);
    if (!f32 || !f16 || !s_cap || !s_mem)
        return 1;
    if (!have_headless()) {
        printf("SKIP: no Vulkan loader with VK_EXT_headless_surface\n");
        return 77;
    }
    setenv("RECOMP_PRESENT_CAPTURE", "1", 1);
    setenv("RECOMP_ASPECT", "native", 1);
    setenv("RECOMP_VK_CACHE", "0", 1);

    if (nv2a_vk_present_init(exts, 2, make_surface, NULL, 640, 480) != 0) {
        const char *s = nv2a_vk_present_status();
        if (strstr(s, "no Vulkan") || strstr(s, "none of") || strstr(s, "headless")) {
            printf("SKIP: %s\n", s);
            return 77;
        }
        printf("FAIL present_init: %s\n", s);
        return 1;
    }
    printf("presenter: %s\n", nv2a_vk_present_status());
    win_size(&sw, &sh);
    check(s_surfaces_made == 1 && sw && sh && sw <= MAXW && sh <= MAXH, "init",
          "surfaces made %d, window %ux%u", s_surfaces_made, sw, sh);
    if (!sw || !sh || sw > MAXW || sh > MAXH)
        return 1;

    /* 1:1, both formats, and a redraw */
    make_card32(f32, sw, sh);
    r = nv2a_vk_present_frame(f32, sw, sh, sw * 4, 4);
    check(r == 0 && compare_1to1(f32, sw, sh, sw * 4, 4, 0, why, sizeof why), "1:1 X8R8G8B8",
          "%s (r=%d)", why, r);
    make_card16(f16, sw, sh);
    r = nv2a_vk_present_frame(f16, sw, sh, sw * 2, 2);
    check(r == 0 && compare_1to1(f16, sw, sh, sw * 2, 2, 1, why, sizeof why), "1:1 R5G6B5",
          "%s (r=%d)", why, r);
    r = nv2a_vk_present_frame(NULL, 0, 0, 0, 0);
    check(r == 0 && compare_1to1(f16, sw, sh, sw * 2, 2, 1, why, sizeof why), "redraw",
          "%s (r=%d)", why, r);

    /* Boxed: a 4:3 frame (side bars in a wider window), a 32:9 one (above
     * and below) */
    make_card32(f32, 640, 480);
    r = nv2a_vk_present_frame(f32, 640, 480, 640 * 4, 4);
    check(r == 0 && check_boxed(640, 480, (double)sw / sh > 4.0 / 3.0 + 0.01 ? 1 : 0, why,
                                sizeof why), "pillarbox 4:3", "%s (r=%d)", why, r);
    make_card32(f32, 1280, 360);
    r = nv2a_vk_present_frame(f32, 1280, 360, 1280 * 4, 4);
    check(r == 0 && check_boxed(1280, 360, 2, why, sizeof why), "letterbox 32:9",
          "%s (r=%d)", why, r);

    /* Resize: a new swapchain (the size is the surface's to decide), the
     * last frame redrawn into it */
    nv2a_vk_present_get_stats(&ps);
    chains = ps.swapchains;
    nv2a_vk_present_resize(800, 800);
    r = nv2a_vk_present_frame(NULL, 0, 0, 0, 0);
    nv2a_vk_present_get_stats(&ps);
    win_size(&w2, &h2);
    check(r == 0 && ps.swapchains == chains + 1 && check_boxed(1280, 360, 2, why, sizeof why),
          "resize", "%s; swapchains %llu -> %llu (r=%d)", why, (unsigned long long)chains,
          (unsigned long long)ps.swapchains, r);
    nv2a_vk_present_resize(640, 480);
    r = nv2a_vk_present_frame(NULL, 0, 0, 0, 0);
    win_size(&sw, &sh);
    if (sw > MAXW || sh > MAXH)
        return 1;

    /* Suspend / resume: the surface goes and is made again */
    made = s_surfaces_made;
    nv2a_vk_present_suspend();
    make_card32(f32, sw, sh);
    r = nv2a_vk_present_frame(f32, sw, sh, sw * 4, 4);
    check(r == 1, "suspended", "frame %s (r=%d)", r == 1 ? "dropped" : "not dropped", r);
    nv2a_vk_present_resume();
    r = nv2a_vk_present_frame(f32, sw, sh, sw * 4, 4);
    win_size(&w2, &h2);
    check(r == 0 && s_surfaces_made == made + 1 && w2 == sw && h2 == sh
              && compare_1to1(f32, sw, sh, sw * 4, 4, 0, why, sizeof why),
          "resume", "%s; surfaces made %d (r=%d)", why, s_surfaces_made, r);

    /* The renderer on the presenter's device: a scene the window's size */
    if ((size_t)sw * sh * 4 > ZETA - SURF) {
        check(0, "renderer", "window %ux%u too large for the test's guest memory", sw, sh);
    } else if (nv2a_vk_register() != 0) {
        check(0, "renderer", "nv2a_vk_register: %s", nv2a_vk_status());
    } else {
        uint32_t c0, c1;
        printf("renderer: %s\n", nv2a_vk_status());
        draw_scene(sw, sh);
        nv2a_vk_get_stats(&st);
        r = nv2a_vk_present_frame(s_mem + SURF, sw, sh, sw * 4, 4);
        check(st.draws > 0 && st.readbacks > 0 && r == 0
                  && compare_1to1(s_mem + SURF, sw, sh, sw * 4, 4, 0, why, sizeof why),
              "renderer", "%s; %llu draws, %llu readbacks (r=%d)", why,
              (unsigned long long)st.draws, (unsigned long long)st.readbacks, r);
        /* And the scene really drew: the clear colour and a vertex colour. */
        memcpy(&c0, s_mem + SURF + 2 * (size_t)sw * 4 + 2 * 4, 4);
        memcpy(&c1, s_mem + SURF + (size_t)(sh / 2) * sw * 4 + (size_t)(sw / 2) * 4, 4);
        check((c0 & 0xFFFFFFu) == 0x203040u && (c1 & 0xFFFFFFu) != 0x203040u, "scene",
              "corner %08X, centre %08X", c0, c1);
        nv2a_backend_register(NULL);
        nv2a_vk_shutdown();             /* the renderer first: the device stays */
        r = nv2a_vk_present_frame(f32, sw, sh, sw * 4, 4);
        check(r == 0 && compare_1to1(f32, sw, sh, sw * 4, 4, 0, why, sizeof why),
              "after renderer shutdown", "%s (r=%d)", why, r);
    }
    nv2a_vk_present_get_stats(&ps);
    printf("presents %llu, skipped %llu, swapchains %llu, suspends %llu, %ux%u\n",
           (unsigned long long)ps.presents, (unsigned long long)ps.skipped,
           (unsigned long long)ps.swapchains, (unsigned long long)ps.suspends, ps.width, ps.height);
    check(ps.presents >= 9 && ps.swapchains >= 4 && ps.suspends == 1, "stats", "");
    nv2a_vk_present_shutdown();
    r = nv2a_vk_present_frame(f32, sw, sh, sw * 4, 4);
    check(r < 0, "after shutdown", "frames refused (r=%d)", r);

    free(f32);
    free(f16);
    free(s_cap);
    free(s_mem);
    printf(fails ? "nv2a_vk_present: FAIL\n" : "nv2a_vk_present: PASS\n");
    return fails ? 1 : 0;
}
