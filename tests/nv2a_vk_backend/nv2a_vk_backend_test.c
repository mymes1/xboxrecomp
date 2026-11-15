/* The Vulkan back end (src/vk/nv2a_vk.c) against the executor's own CPU
 * rasteriser. [android-port]
 *
 * Each scene is a pushbuffer -- methods as a title sends them -- run twice
 * from identical guest memory: once with no back end (the CPU rasteriser
 * draws) and once with nv2a_vk_register()'d (Vulkan draws, and writes the
 * surfaces back at the flip's present_sync). The two framebuffers must
 * agree: at most 0.5% of pixels may differ, and only by more than 3 in a
 * channel. The scenes cover what the back end translates:
 *
 *   gouraud     clear (clip rect) and per-vertex colour; undrawn pixels keep
 *               the title's own bytes (upload/readback round trip)
 *   textured    swizzled and linear textures, nearest, perspective-correct
 *               coordinates (w varies), wrap and mirror addressing
 *   depth       window-z depth test and write (Z24S8)
 *   blend       alpha blending and additive over the existing framebuffer
 *   rgb565      a 16-bit surface: 5:6:5 writes truncated as put_pixel does
 *   combiners   register combiners: texture * diffuse + c0, the final
 *               combiner, mux, per-stage constants, a tent-filtered stage
 *   alphatest   alpha test cutting texels before the depth write
 *   rtt         render to a swizzled surface, then texture from it
 *   stencil     (Vulkan only: the CPU path has no stencil) replace, then
 *               test EQUAL -- checked against the expected pixels
 *
 * Then the pipeline cache (RECOMP_VK_CACHE): the scenes' pipelines are saved
 * at shutdown and offered to the driver at the next start; a damaged,
 * truncated or foreign file, or one from other shaders, is not.
 *
 * Exit 77 (ctest SKIP_RETURN_CODE) when there is no Vulkan device.
 * RECOMP_VK_TEST_OUT=<dir> writes both renderings of every scene as PPM. */
#define _POSIX_C_SOURCE 200809L         /* setenv */
#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "nv2a_backend.h"
#include "nv2a_vk.h"
#include "d3d/d3d8_swizzle.h"

#define MEM_BYTES (8u << 20)
static uint8_t *s_mem;

/* ---- what the executor expects the runtime to provide (as nv2a_ffp_backend) */
ptrdiff_t xbox_GetMemoryOffset(void) { return (ptrdiff_t)s_mem; }
uint32_t g_xbox_image_lo, g_xbox_image_hi;
uint32_t g_xbox_stack_base, g_xbox_stack_size;
uint32_t xbox_ContiguousAllocatedBytes(void) { return 0; }
void xbox_FramebufferWindowSet(uint32_t va, uint32_t pitch) { (void)va; (void)pitch; }
void xbox_FramebufferWindowSetAA(uint32_t sy) { (void)sy; }
static uint32_t s_fence_va;
uint32_t xbox_Nv2aFenceWordVa(void) { return s_fence_va; }
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

/* ---- guest memory map ------------------------------------------------------ */
#define SURF_MAIN  0x100000u    /* 256x256 A8R8G8B8, pitch 1024 */
#define SURF_565   0x180000u    /* 256x256 R5G6B5, pitch 512 */
#define SURF_SWZ   0x1C0000u    /* 64x64 A8R8G8B8, swizzled */
#define ZETA_MAIN  0x200000u
#define ZETA_SWZ   0x280000u
#define TEX_SWZ    0x300000u    /* 64x64 A8R8G8B8 swizzled: checker */
#define TEX_LIN    0x310000u    /* 48x40 LIN_A8R8G8B8, pitch 192 */
#define TEX_ALPHA  0x320000u    /* 32x32 A8R8G8B8 swizzled: alpha ramp */
#define VB_POS     0x400000u
#define VB_COL     0x420000u
#define VB_T0      0x440000u
#define VB_T1      0x460000u

#define W 256
#define H 256
#define ZMAX 16777215.0f

extern void nv2a_pb_exec_method(uint32_t subch, uint32_t method, uint32_t param);
static void m(uint32_t method, uint32_t param) { nv2a_pb_exec_method(0, method, param); }
static uint32_t f2u(float f) { uint32_t u; memcpy(&u, &f, 4); return u; }
static void mf(uint32_t method, float f) { m(method, f2u(f)); }
static void put32(uint32_t at, uint32_t v) { memcpy(s_mem + at, &v, 4); }
static void put16(uint32_t at, uint16_t v) { memcpy(s_mem + at, &v, 2); }
static void putf(uint32_t at, float f) { memcpy(s_mem + at, &f, 4); }

/* ---- the initial guest memory, the same for both renderings --------------- */
static void put_swizzled(uint32_t base, uint32_t w, uint32_t h,
                         uint32_t (*texel)(uint32_t, uint32_t))
{
    uint32_t mx, my, x, y;
    xbox_swizzle_masks(w, h, &mx, &my);
    for (y = 0; y < h; y++)
        for (x = 0; x < w; x++)
            put32(base + 4 * (swizzle_deposit(x, mx) | swizzle_deposit(y, my)), texel(x, y));
}

static uint32_t checker(uint32_t x, uint32_t y)
{
    uint32_t c = ((x >> 3) ^ (y >> 3)) & 1;
    return c ? 0xFFE0C020u | (x << 1) : 0xFF2040C0u | ((y << 2) << 8);
}

static uint32_t alpha_ramp(uint32_t x, uint32_t y)
{
    return ((x * 8u) << 24) | ((255u - y * 8u) << 16) | (0x80u << 8) | (y * 8u);
}

static void init_memory(void)
{
    uint32_t x, y;

    memset(s_mem, 0, MEM_BYTES);
    for (y = 0; y < H; y++)
        for (x = 0; x < W; x++) {
            put32(SURF_MAIN + y * 1024 + x * 4,
                  0xFF000000u | (x << 16) | (y << 8) | ((x ^ y) & 0xFFu));
            put16(SURF_565 + y * 512 + x * 2,
                  (uint16_t)(((x >> 3) << 11) | ((y >> 2) << 5) | ((x ^ y) >> 3 & 0x1F)));
        }
    put_swizzled(TEX_SWZ, 64, 64, checker);
    for (y = 0; y < 40; y++)
        for (x = 0; x < 48; x++)
            put32(TEX_LIN + y * 192 + x * 4,
                  0xFF000000u | ((x * 5u) << 16) | ((y * 6u) << 8) | (((x + y) & 1) ? 0xF0u : 0x10u));
    put_swizzled(TEX_ALPHA, 32, 32, alpha_ramp);
}

/* ---- state -------------------------------------------------------------------- */
static void reset_state(void)
{
    static const float I[16] = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1 };
    int i;

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
}

static void surface_linear(uint32_t color_fmt, uint32_t base, uint32_t pitch,
                           uint32_t cx, uint32_t cy, uint32_t cw, uint32_t ch)
{
    m(0x0208, color_fmt | (2u << 4) | (1u << 8));   /* Z24S8, pitch */
    m(0x0200, cx | (cw << 16));
    m(0x0204, cy | (ch << 16));
    m(0x020C, pitch | (pitch << 16));
    m(0x0210, base);
    m(0x0214, ZETA_MAIN);
}

static void surface_swizzled(uint32_t base, uint32_t logw, uint32_t logh)
{
    m(0x0208, 0x8u | (2u << 4) | (2u << 8) | (logw << 16) | (logh << 24));
    m(0x0200, (1u << logw) << 16);
    m(0x0204, (1u << logh) << 16);
    m(0x020C, (4u << logw) | ((4u << logw) << 16));
    m(0x0210, base);
    m(0x0214, ZETA_SWZ);
}

static void clear(uint32_t flags, uint32_t argb, uint32_t zs)
{
    m(0x1D90, argb);
    m(0x1D8C, zs);
    m(0x1D94, flags);
}

static void tex_swizzled(int st, uint32_t addr, uint32_t logw, uint32_t logh,
                         uint32_t address, uint32_t filter)
{
    uint32_t b = 0x1B00 + 0x40 * (uint32_t)st;
    m(b + 0x00, addr);
    m(b + 0x04, (0x06u << 8) | (2u << 4) | (1u << 16) | (logw << 20) | (logh << 24));
    m(b + 0x08, address);
    m(b + 0x14, filter);
}

static void tex_linear(int st, uint32_t addr, uint32_t w, uint32_t h, uint32_t pitch,
                       uint32_t address, uint32_t filter)
{
    uint32_t b = 0x1B00 + 0x40 * (uint32_t)st;
    m(b + 0x00, addr);
    m(b + 0x04, (0x12u << 8) | (2u << 4) | (1u << 16));
    m(b + 0x08, address);
    m(b + 0x10, pitch << 16);
    m(b + 0x14, filter);
    m(b + 0x1C, (w << 16) | h);
}

#define NEAREST 0x01010000u
#define TENT    0x02020000u
#define WRAP    (1u | (1u << 8))
#define MIRROR  (2u | (2u << 8))
#define CLAMP   (3u | (3u << 8))

typedef struct { float x, y, z, w; uint32_t c; float t0[4], t1[4]; } Vtx;

/* One batch of triangles: positions in pixels with window z, given w (the
 * transform divides it back out), D3DCOLOR diffuse, up to two coordinate sets. */
static void draw(const Vtx *v, int n, int ntex)
{
    int i;
    for (i = 0; i < n; i++) {
        putf(VB_POS + 16 * i + 0, v[i].x * v[i].w);
        putf(VB_POS + 16 * i + 4, v[i].y * v[i].w);
        putf(VB_POS + 16 * i + 8, v[i].z * v[i].w);
        putf(VB_POS + 16 * i + 12, v[i].w);
        put32(VB_COL + 4 * i, v[i].c);
        memcpy(s_mem + VB_T0 + 16 * i, v[i].t0, 16);
        memcpy(s_mem + VB_T1 + 16 * i, v[i].t1, 16);
    }
    m(0x1720, VB_POS); m(0x1760, 2 | (4 << 4) | (16 << 8));     /* attr 0: float4 */
    m(0x172C, VB_COL); m(0x176C, 0 | (4 << 4) | (4 << 8));      /* attr 3: D3DCOLOR */
    m(0x1744, ntex > 0 ? VB_T0 : 0); m(0x1784, ntex > 0 ? (2 | (4 << 4) | (16 << 8)) : 2);
    m(0x1748, ntex > 1 ? VB_T1 : 0); m(0x1788, ntex > 1 ? (2 | (4 << 4) | (16 << 8)) : 2);
    m(0x17FC, 5);
    m(0x1810, (uint32_t)(n - 1) << 24);
    m(0x17FC, 0);
}

static Vtx V4(float x, float y, float z, float w, uint32_t c, float u, float vv)
{
    Vtx r;
    memset(&r, 0, sizeof r);
    r.x = x; r.y = y; r.z = z; r.w = w; r.c = c;
    r.t0[0] = u; r.t0[1] = vv; r.t0[3] = 1.0f;
    r.t1[0] = u; r.t1[1] = vv; r.t1[3] = 1.0f;
    return r;
}

/* Two triangles: an axis-aligned quad, (x0,y0)-(x1,y1), uv (u0,v0)-(u1,v1). */
static int quad(Vtx *o, float x0, float y0, float x1, float y1, float z, uint32_t c,
                float u0, float v0, float u1, float v1)
{
    o[0] = V4(x0, y0, z, 1, c, u0, v0);
    o[1] = V4(x1, y0, z, 1, c, u1, v0);
    o[2] = V4(x1, y1, z, 1, c, u1, v1);
    o[3] = V4(x0, y0, z, 1, c, u0, v0);
    o[4] = V4(x1, y1, z, 1, c, u1, v1);
    o[5] = V4(x0, y1, z, 1, c, u0, v1);
    return 6;
}

static void flip(void)
{
    m(0x012C, 0);                               /* FLIP_INCREMENT_WRITE */
    m(0x0130, 0);                               /* FLIP_STALL: present_sync */
}

/* ---- scenes ------------------------------------------------------------------- */
static void scene_gouraud(void)
{
    Vtx v[6];
    surface_linear(0x8, SURF_MAIN, 1024, 16, 16, 224, 200);
    clear(0xF0, 0xFF203040u, 0);                /* only the clip: the rest stays */
    surface_linear(0x8, SURF_MAIN, 1024, 0, 0, W, H);
    v[0] = V4(20, 30, 0, 1, 0xFFFF0000u, 0, 0);
    v[1] = V4(230, 60, 0, 1, 0xFF00FF00u, 0, 0);
    v[2] = V4(90, 240, 0, 1, 0xFF0000FFu, 0, 0);
    v[3] = V4(250, 5, 0, 1, 0x80FFFFFFu, 0, 0);
    v[4] = V4(200, 250, 0, 1, 0xFF808000u, 0, 0);
    v[5] = V4(140, 120, 0, 1, 0xFF00C0C0u, 0, 0);
    draw(v, 6, 0);
    flip();
}

static void scene_textured(void)
{
    Vtx v[12];
    int n = 0;
    surface_linear(0x8, SURF_MAIN, 1024, 0, 0, W, H);
    clear(0xF0, 0xFF000000u, 0);
    /* swizzled, magnified x2, wrapping twice */
    tex_swizzled(0, TEX_SWZ, 6, 6, WRAP, NEAREST);
    n = quad(v, 8, 8, 136, 136, 0, 0xFFFFFFFFu, 0.0f, 0.0f, 2.0f, 2.0f);
    draw(v, n, 1);
    /* the same texture in perspective: w 1 at the top, 4 at the bottom */
    v[0] = V4(140, 10, 0, 1, 0xFFFFFFFFu, 0, 0);
    v[1] = V4(250, 10, 0, 1, 0xFFFFFFFFu, 1, 0);
    v[2] = V4(250, 130, 0, 4, 0xFFFFFFFFu, 1, 1);
    v[3] = V4(140, 10, 0, 1, 0xFFFFFFFFu, 0, 0);
    v[4] = V4(250, 130, 0, 4, 0xFFFFFFFFu, 1, 1);
    v[5] = V4(140, 130, 0, 4, 0xFFFFFFFFu, 0, 1);
    draw(v, 6, 1);
    /* linear texture (texel coordinates), mirrored, modulated by a colour.
     * The coordinates avoid round ratios on purpose: where a pixel centre
     * samples exactly on a texel boundary (-24..96 over 236 pixels puts
     * column 39 at u = -9.0) either texel is right, and float rounding
     * picks differently on the two renderers. */
    tex_linear(0, TEX_LIN, 48, 40, 192, MIRROR, NEAREST);
    n = quad(v, 10, 145, 246, 250, 0, 0xFFC0FFA0u, -24.37f, -10.21f, 95.83f, 69.67f);
    draw(v, n, 1);
    flip();
}

static void scene_depth(void)
{
    Vtx v[9];
    surface_linear(0x8, SURF_MAIN, 1024, 0, 0, W, H);
    clear(0xF3, 0xFF101010u, 0xFFFFFF00u);       /* colour, depth far, stencil 0 */
    m(0x030C, 1); m(0x0354, 0x201); m(0x035C, 1);   /* LESS, write */
    v[0] = V4(10, 10, 0.50f * ZMAX, 1, 0xFFFF0000u, 0, 0);
    v[1] = V4(240, 20, 0.50f * ZMAX, 1, 0xFFFF0000u, 0, 0);
    v[2] = V4(60, 240, 0.50f * ZMAX, 1, 0xFFFF0000u, 0, 0);
    /* sloped: crosses the first one */
    v[3] = V4(30, 200, 0.20f * ZMAX, 1, 0xFF00FF00u, 0, 0);
    v[4] = V4(250, 120, 0.90f * ZMAX, 1, 0xFF00FF00u, 0, 0);
    v[5] = V4(120, 5, 0.40f * ZMAX, 1, 0xFF00FF00u, 0, 0);
    /* behind everything drawn: only visible outside both */
    v[6] = V4(0, 0, 0.95f * ZMAX, 1, 0xFF0000FFu, 0, 0);
    v[7] = V4(255, 0, 0.95f * ZMAX, 1, 0xFF0000FFu, 0, 0);
    v[8] = V4(128, 255, 0.95f * ZMAX, 1, 0xFF0000FFu, 0, 0);
    draw(v, 9, 0);
    flip();
}

static void scene_blend(void)
{
    Vtx v[12];
    int n;
    surface_linear(0x8, SURF_MAIN, 1024, 0, 0, W, H);   /* over the initial pattern */
    m(0x0304, 1); m(0x0344, 0x0302); m(0x0348, 0x0303); m(0x0350, 0x8006);
    n = quad(v, 20, 20, 180, 200, 0, 0x80FF8000u, 0, 0, 0, 0);
    v[1].c = v[2].c = 0x20FFFFFFu;              /* alpha varies across */
    v[4].c = 0x20FFFFFFu;
    draw(v, n, 0);
    m(0x0344, 1); m(0x0348, 1);                 /* additive */
    n = quad(v, 100, 90, 250, 250, 0, 0xFF302010u, 0, 0, 0, 0);
    draw(v, n, 0);
    m(0x0344, 0x0306); m(0x0348, 0);            /* modulate by the destination */
    n = quad(v, 0, 210, 120, 256, 0, 0xFFFFFFFFu, 0, 0, 0, 0);
    v[0].c = v[3].c = 0xFF808080u;
    draw(v, n, 0);
    flip();
}

static void scene_565(void)
{
    Vtx v[6];
    surface_linear(0x3, SURF_565, 512, 8, 8, 240, 240);
    clear(0xF0, 0xFF5A7BC6u, 0);
    surface_linear(0x3, SURF_565, 512, 0, 0, W, H);
    v[0] = V4(0, 0, 0, 1, 0xFFFF0000u, 0, 0);
    v[1] = V4(256, 40, 0, 1, 0xFF00FF00u, 0, 0);
    v[2] = V4(40, 256, 0, 1, 0xFF0000FFu, 0, 0);
    v[3] = V4(256, 100, 0, 1, 0xFFFFFFFFu, 0, 0);
    v[4] = V4(256, 256, 0, 1, 0xFF123456u, 0, 0);
    v[5] = V4(100, 256, 0, 1, 0xFFABCDEFu, 0, 0);
    draw(v, 6, 0);
    flip();
}

static void rc_program(uint32_t stages, uint32_t control_flags)
{
    m(0x1E60, stages | control_flags);          /* COMBINER_CONTROL (rc_seen) */
}

static void scene_combiners(void)
{
    Vtx v[12];
    int n;
    surface_linear(0x8, SURF_MAIN, 1024, 0, 0, W, H);
    clear(0xF0, 0xFF000000u, 0);
    tex_swizzled(0, TEX_SWZ, 6, 6, WRAP, NEAREST);
    tex_swizzled(1, TEX_ALPHA, 5, 5, CLAMP, TENT);
    m(0x1E70, 1u | (1u << 5));                  /* stages 0 and 1: 2D */

    /* stage 0: r0 = t0 * v0 + c0 * 1;  r0.a = t0.a * v0.a */
    rc_program(1, 0);
    m(0x0AC0, (0x08u << 24) | (0x04u << 16) | (0x01u << 8) | 0x20u);
    m(0x0260, (0x18u << 24) | (0x14u << 16));
    m(0x1E40, 0xCu << 8);
    m(0x0AA0, 0xCu << 8);
    m(0x0A60, 0x00302010u);
    m(0x0288, 0); m(0x028C, 0);
    n = quad(v, 4, 4, 124, 124, 0, 0xFFFFC080u, 0, 0, 1.5f, 1.5f);
    v[2].c = v[4].c = 0xFF4080FFu;
    draw(v, n, 1);

    /* final combiner: rgb = t1 * v0 + (1 - t1) * c1 (lerp), a = t1.a */
    m(0x0288, (0x09u << 24) | (0x04u << 16) | (0x02u << 8) | 0x00u);
    m(0x028C, 0x19u << 8);
    m(0x1E24, 0xFF20E040u);                     /* final c1 */
    n = quad(v, 132, 4, 252, 124, 0, 0xFFFF2020u, 0, 0, 1, 1);
    draw(v, n, 2);

    /* two stages: per-stage constants, a mux on r0.a (MSB), signed mappings
     * stage 0: r0 = t0 (A=t0, B=1), r0.a = t1.a
     * stage 1: mux: r0.a >= 0.5 ? c0(stage 1) : v0 * expand(t0) */
    m(0x0288, 0); m(0x028C, 0);
    rc_program(2, 0x1000u | 0x100u);            /* per-stage c0 (0x10<<8), MSB mux (1<<8) */
    m(0x0AC0, (0x08u << 24) | (0x20u << 16));   /* A t0, B one */
    m(0x0260, (0x19u << 24) | (0x20u << 16));   /* A t1.a, B one */
    m(0x1E40, 0xCu << 4);                       /* AB -> r0 */
    m(0x0AA0, 0xCu << 4);
    m(0x0AC4, (0x01u << 24) | (0x20u << 16) | (0x04u << 8) | 0x48u); /* A c0 B 1 C v0 D expand(t0) */
    m(0x0264, (0x11u << 24) | (0x20u << 16));
    m(0x1E44, (0xCu << 8) | (0x4u << 12));      /* mux sum -> r0 */
    m(0x0AA4, 0xCu << 8);
    m(0x0A60, 0x00000000u); m(0x0A64, 0xFF10F0F0u);
    n = quad(v, 4, 132, 124, 252, 0, 0xFFC0C0C0u, 0, 0, 1, 1);
    draw(v, n, 2);

    /* back to one stage with a projective coordinate: q != 1 */
    rc_program(1, 0);
    m(0x0AC0, (0x08u << 24) | (0x20u << 16));
    m(0x0260, (0x18u << 24) | (0x20u << 16));
    m(0x1E40, 0xCu << 4);
    m(0x0AA0, 0xCu << 4);
    n = quad(v, 132, 132, 252, 252, 0, 0xFFFFFFFFu, 0, 0, 2, 2);
    v[2].t0[3] = v[4].t0[3] = 2.0f;             /* q */
    draw(v, n, 1);
    flip();
}

static void scene_alphatest(void)
{
    Vtx v[12];
    int n;
    surface_linear(0x8, SURF_MAIN, 1024, 0, 0, W, H);
    clear(0xF3, 0xFF000040u, 0xFFFFFF00u);
    tex_swizzled(0, TEX_ALPHA, 5, 5, WRAP, NEAREST);
    m(0x1E70, 1u);
    rc_program(1, 0);
    m(0x0AC0, (0x08u << 24) | (0x04u << 16));   /* t0 * v0 */
    m(0x0260, (0x18u << 24) | (0x14u << 16));
    m(0x1E40, 0xCu << 4);
    m(0x0AA0, 0xCu << 4);
    m(0x0288, 0); m(0x028C, 0);
    /* near quad, alpha-tested: cut texels must not write depth. Coordinates
     * a hundredth of a texel off the grid, so no pixel centre samples exactly
     * on a texel boundary (see scene_textured). */
    m(0x030C, 1); m(0x0354, 0x203); m(0x035C, 1);
    m(0x0300, 1); m(0x033C, 0x204); m(0x0340, 0x80);   /* GREATER 0x80 */
    n = quad(v, 16, 16, 240, 240, 0.25f * ZMAX, 0xFFFFFFFFu, 0.01f, 0.01f, 2.01f, 2.01f);
    draw(v, n, 1);
    /* far quad, untested: shows through only where the first was cut */
    m(0x0300, 0);
    m(0x1B00, 0);                               /* stage 0 off: reads white */
    n = quad(v, 0, 0, 256, 256, 0.75f * ZMAX, 0xFF30FF30u, 0, 0, 1, 1);
    draw(v, n, 1);
    flip();
}

static void scene_rtt(void)
{
    Vtx v[12];
    int n;
    /* into the swizzled 64x64 target */
    surface_swizzled(SURF_SWZ, 6, 6);
    m(0x1E70, 0);
    rc_program(1, 0);
    m(0x0AC0, (0x04u << 24) | (0x20u << 16));   /* v0 */
    m(0x0260, (0x14u << 24) | (0x20u << 16));
    m(0x1E40, 0xCu << 4);
    m(0x0AA0, 0xCu << 4);
    m(0x0288, 0); m(0x028C, 0);
    clear(0xF3, 0xFF404040u, 0xFFFFFF00u);
    v[0] = V4(0, 0, 0, 1, 0xFFFF0000u, 0, 0);
    v[1] = V4(64, 8, 0, 1, 0xFF00FF00u, 0, 0);
    v[2] = V4(16, 64, 0, 1, 0xFF0000FFu, 0, 0);
    draw(v, 3, 0);
    /* then the main surface, textured from what was just drawn */
    surface_linear(0x8, SURF_MAIN, 1024, 0, 0, W, H);
    clear(0xF0, 0xFF000000u, 0);
    tex_swizzled(0, SURF_SWZ, 6, 6, CLAMP, NEAREST);
    m(0x1E70, 1u);
    m(0x0AC0, (0x08u << 24) | (0x20u << 16));   /* t0 */
    m(0x0260, (0x18u << 24) | (0x20u << 16));
    n = quad(v, 32, 32, 224, 224, 0, 0xFFFFFFFFu, 0, 0, 1, 1);
    draw(v, n, 1);
    flip();
}

typedef struct {
    const char *name;
    void (*fn)(void);
    uint32_t base, pitch, bpp;
} Scene;

static const Scene k_scenes[] = {
    { "gouraud",   scene_gouraud,   SURF_MAIN, 1024, 4 },
    { "textured",  scene_textured,  SURF_MAIN, 1024, 4 },
    { "depth",     scene_depth,     SURF_MAIN, 1024, 4 },
    { "blend",     scene_blend,     SURF_MAIN, 1024, 4 },
    { "rgb565",    scene_565,       SURF_565,  512,  2 },
    /* COMBINER_CONTROL is sticky (rc_seen): combiner scenes come last */
    { "combiners", scene_combiners, SURF_MAIN, 1024, 4 },
    { "alphatest", scene_alphatest, SURF_MAIN, 1024, 4 },
    { "rtt",       scene_rtt,       SURF_MAIN, 1024, 4 },
};

/* ---- comparison ------------------------------------------------------------- */
static void px_rgb(const uint8_t *img, uint32_t pitch, uint32_t bpp, int x, int y, int c[3])
{
    if (bpp == 4) {
        uint32_t v;
        memcpy(&v, img + (size_t)y * pitch + (size_t)x * 4, 4);
        c[0] = (int)((v >> 16) & 0xFF); c[1] = (int)((v >> 8) & 0xFF); c[2] = (int)(v & 0xFF);
    } else {
        uint16_t v;
        memcpy(&v, img + (size_t)y * pitch + (size_t)x * 2, 2);
        c[0] = (v >> 11) << 3; c[1] = ((v >> 5) & 0x3F) << 2; c[2] = (v & 0x1F) << 3;
    }
}

static void dump_ppm(const char *dir, const char *name, const char *tag,
                     const uint8_t *img, uint32_t pitch, uint32_t bpp)
{
    char path[512];
    FILE *f;
    int x, y, c[3];
    snprintf(path, sizeof path, "%s/%s-%s.ppm", dir, name, tag);
    f = fopen(path, "wb");
    if (!f)
        return;
    fprintf(f, "P6\n%d %d\n255\n", W, H);
    for (y = 0; y < H; y++)
        for (x = 0; x < W; x++) {
            unsigned char b[3];
            px_rgb(img, pitch, bpp, x, y, c);
            b[0] = (unsigned char)c[0]; b[1] = (unsigned char)c[1]; b[2] = (unsigned char)c[2];
            fwrite(b, 1, 3, f);
        }
    fclose(f);
}

/* Pixels that differ by more than `tol` in any channel, and the largest
 * difference (alpha included for 32-bit surfaces). */
static int diff_count(const uint8_t *a, const uint8_t *b, uint32_t pitch, uint32_t bpp,
                      int tol, int *maxd, int *fx, int *fy)
{
    int x, y, k, n = 0, ca[3], cb[3];
    *maxd = 0;
    *fx = *fy = -1;
    for (y = 0; y < H; y++)
        for (x = 0; x < W; x++) {
            int d = 0;
            px_rgb(a, pitch, bpp, x, y, ca);
            px_rgb(b, pitch, bpp, x, y, cb);
            for (k = 0; k < 3; k++)
                if (abs(ca[k] - cb[k]) > d) d = abs(ca[k] - cb[k]);
            if (bpp == 4) {
                int aa = a[(size_t)y * pitch + (size_t)x * 4 + 3];
                int ab = b[(size_t)y * pitch + (size_t)x * 4 + 3];
                if (abs(aa - ab) > d) d = abs(aa - ab);
            }
            if (d > *maxd) *maxd = d;
            if (d > tol) {
                if (n == 0) { *fx = x; *fy = y; }
                n++;
            }
        }
    return n;
}

static uint8_t *run(const Scene *sc, int vk)
{
    uint8_t *out = (uint8_t *)malloc((size_t)sc->pitch * H);
    init_memory();
    if (vk) {
        if (nv2a_vk_register() != 0) {
            printf("FAIL %s: nv2a_vk_register: %s\n", sc->name, nv2a_vk_status());
            exit(1);
        }
    } else {
        nv2a_backend_register(NULL);
    }
    reset_state();
    sc->fn();
    memcpy(out, s_mem + sc->base, (size_t)sc->pitch * H);
    if (vk)
        nv2a_backend_register(NULL);
    return out;
}

/* Stencil, Vulkan only: a quad writes 1 where it covers (colour off), then a
 * full-screen quad tests EQUAL 1 -- only the first quad's pixels change. */
static int stencil_scene(const char *outdir)
{
    Vtx v[6];
    int x, y, bad = 0, inside = 0;
    if (nv2a_vk_register() != 0)
        return 1;
    init_memory();
    reset_state();
    surface_linear(0x8, SURF_MAIN, 1024, 0, 0, W, H);
    clear(0xF3, 0xFF000000u, 0xFFFFFF00u);          /* stencil 0 */
    m(0x032C, 1); m(0x0364, 0x207); m(0x0368, 1);    /* ALWAYS, ref 1 */
    m(0x0378, 0x1E01);                               /* zpass REPLACE */
    m(0x0358, 0);                                    /* no colour */
    m(0x0300, 1); m(0x033C, 0x207);                  /* (alpha test so it shades) */
    m(0x1E70, 0);
    m(0x0AC0, (0x04u << 24) | (0x20u << 16));
    m(0x0260, (0x14u << 24) | (0x20u << 16));
    quad(v, 64, 64, 192, 160, 0, 0xFFFFFFFFu, 0, 0, 0, 0);
    draw(v, 6, 0);
    m(0x0358, 0x01010101u);
    m(0x0300, 0);
    m(0x0364, 0x202); m(0x0378, 0x1E00);             /* EQUAL 1, keep */
    quad(v, 0, 0, 256, 256, 0, 0xFF00FF00u, 0, 0, 0, 0);
    draw(v, 6, 0);
    m(0x032C, 0);
    flip();
    nv2a_backend_register(NULL);
    for (y = 0; y < H; y++)
        for (x = 0; x < W; x++) {
            uint32_t p, want;
            int in = x >= 64 && x < 192 && y >= 64 && y < 160;
            memcpy(&p, s_mem + SURF_MAIN + y * 1024 + x * 4, 4);
            want = in ? 0xFF00FF00u : 0xFF000000u;
            inside += in;
            if (p != want) {
                if (bad < 3)
                    printf("  stencil: (%d,%d) is %08X, want %08X\n", x, y, p, want);
                bad++;
            }
        }
    if (outdir)
        dump_ppm(outdir, "stencil", "vk", s_mem + SURF_MAIN, 1024, 4);
    printf("%s stencil: %d/%d pixels wrong (%d inside)\n", bad ? "FAIL" : "PASS",
           bad, W * H, inside);
    return bad != 0;
}

/* ---- pipeline cache ------------------------------------------------------ */

static uint8_t *slurp(const char *path, long *n)
{
    FILE *f = fopen(path, "rb");
    uint8_t *b = NULL;
    *n = -1;
    if (!f)
        return NULL;
    if (fseek(f, 0, SEEK_END) == 0 && (*n = ftell(f)) > 0 && fseek(f, 0, SEEK_SET) == 0) {
        b = (uint8_t *)malloc((size_t)*n);
        if (b && fread(b, 1, (size_t)*n, f) != (size_t)*n) {
            free(b);
            b = NULL;
        }
    }
    fclose(f);
    return b;
}

static int spill(const char *path, const uint8_t *b, long n)
{
    FILE *f = fopen(path, "wb");
    int ok = f && fwrite(b, 1, (size_t)n, f) == (size_t)n;
    if (f)
        ok = (fclose(f) == 0) && ok;
    return ok;
}

static uint64_t t_fnv1a64(const uint8_t *p, size_t n)
{
    uint64_t h = 1469598103934665603ull;
    while (n--) {
        h ^= *p++;
        h *= 1099511628211ull;
    }
    return h;
}

/* Start the back end with `path` as it is, expect `want` in the status line
 * (and a load exactly when `loaded`), shut down again. */
static int pc_start(const char *what, const char *want, int loaded)
{
    Nv2aVkStats st;
    const char *s, *pc;
    int bad;

    if (nv2a_vk_register() != 0) {
        printf("FAIL pipeline cache (%s): %s\n", what, nv2a_vk_status());
        return 1;
    }
    nv2a_backend_register(NULL);
    nv2a_vk_get_stats(&st);
    s = nv2a_vk_status();
    pc = strstr(s, "pipeline cache");
    bad = !pc || !strstr(pc, want) || (loaded != (st.pcache_load_bytes > 0));
    printf("%s pipeline cache (%s): %s\n", bad ? "FAIL" : "PASS", what, pc ? pc : s);
    nv2a_vk_shutdown();
    return bad;
}

static int pipeline_cache_checks(const char *path)
{
    uint8_t *good, *b;
    long n;
    uint64_t h;
    int fail = 0;

    good = slurp(path, &n);
    if (!good || n < 32 + 32) {
        printf("FAIL pipeline cache: nothing saved at shutdown (%s, %ld bytes)\n", path, n);
        free(good);
        return 1;
    }
    printf("PASS pipeline cache: %ld bytes saved at shutdown\n", n);
    fail |= pc_start("saved file", "loaded", 1);

    b = (uint8_t *)malloc((size_t)n);
    memcpy(b, good, (size_t)n);
    b[32 + 20] ^= 0x40;                     /* inside the payload; checksum stale */
    fail |= !spill(path, b, n) || pc_start("one byte flipped", "ignored (checksum)", 0);

    memcpy(b, good, (size_t)n);
    b[32 + 8] ^= 0x01;                      /* payload's vendorID, checksum fixed */
    h = t_fnv1a64(b + 32, (size_t)n - 32);
    memcpy(b + 16, &h, 8);
    fail |= !spill(path, b, n) || pc_start("another device", "another device", 0);

    memcpy(b, good, (size_t)n);
    b[24] ^= 0x01;                          /* the SPIR-V hash */
    fail |= !spill(path, b, n) || pc_start("other shaders", "the shaders changed", 0);

    fail |= !spill(path, good, n - 1) || pc_start("truncated", "truncated", 0);

    fail |= !spill(path, (const uint8_t *)"not a cache file at all, really", 32)
            || pc_start("foreign file", "not a pipeline cache", 0);

    remove(path);
    fail |= pc_start("no file", "pipeline cache new", 0);
    free(b);
    free(good);
    return fail;
}

int main(void)
{
    const char *outdir = getenv("RECOMP_VK_TEST_OUT");
    size_t i;
    int fail = 0;
    Nv2aVkStats st;

    char pc_path[1024];

    snprintf(pc_path, sizeof pc_path, "%s/nv2a_vk_test_pipeline_cache.bin",
             outdir && *outdir ? outdir
                               : (getenv("TMPDIR") && *getenv("TMPDIR") ? getenv("TMPDIR") : "/tmp"));
    remove(pc_path);
    setenv("RECOMP_VK_CACHE", pc_path, 1);

    s_mem = (uint8_t *)calloc(1, MEM_BYTES);
    if (!s_mem)
        return 1;
    if (nv2a_vk_register() != 0) {
        printf("SKIP: no Vulkan back end: %s\n", nv2a_vk_status());
        return 77;
    }
    nv2a_backend_register(NULL);
    printf("Vulkan back end: %s\n", nv2a_vk_status());
    if (!strstr(nv2a_vk_status(), "pipeline cache new")) {
        printf("FAIL pipeline cache: a first start should report \"pipeline cache new\"\n");
        fail = 1;
    }

    for (i = 0; i < sizeof k_scenes / sizeof k_scenes[0]; i++) {
        const Scene *sc = &k_scenes[i];
        uint8_t *init, *ref, *got;
        int maxd, fx, fy, nd, changed, md2, x2, y2;
        init_memory();
        init = (uint8_t *)malloc((size_t)sc->pitch * H);
        memcpy(init, s_mem + sc->base, (size_t)sc->pitch * H);
        ref = run(sc, 0);
        got = run(sc, 1);
        nd = diff_count(ref, got, sc->pitch, sc->bpp, 3, &maxd, &fx, &fy);
        changed = diff_count(init, ref, sc->pitch, sc->bpp, 0, &md2, &x2, &y2);
        if (outdir) {
            dump_ppm(outdir, sc->name, "cpu", ref, sc->pitch, sc->bpp);
            dump_ppm(outdir, sc->name, "vk", got, sc->pitch, sc->bpp);
        }
        if (nd * 200 > W * H || changed * 100 < W * H) {
            fail = 1;
            printf("FAIL %s: %d/%d pixels differ by more than 3 (max %d, first at %d,%d);"
                   " the scene changed %d pixels\n", sc->name, nd, W * H, maxd, fx, fy, changed);
            if (fx >= 0) {
                int a[3], b[3];
                px_rgb(ref, sc->pitch, sc->bpp, fx, fy, a);
                px_rgb(got, sc->pitch, sc->bpp, fx, fy, b);
                printf("  at (%d,%d): cpu %d,%d,%d  vk %d,%d,%d\n", fx, fy,
                       a[0], a[1], a[2], b[0], b[1], b[2]);
            }
        } else {
            printf("PASS %s: %d/%d pixels differ by more than 3 (max %d); %d changed\n",
                   sc->name, nd, W * H, maxd, changed);
        }
        free(init);
        free(ref);
        free(got);
    }
    fail |= stencil_scene(outdir);
    nv2a_vk_get_stats(&st);
    printf("vk: %llu draws, %llu triangles, %llu clears, %llu pipelines (%.1f s to build),"
           " %llu texture uploads, %llu surface uploads, %llu readbacks, %llu submits\n",
           (unsigned long long)st.draws, (unsigned long long)st.triangles,
           (unsigned long long)st.clears, (unsigned long long)st.pipelines,
           (double)st.pipeline_build_us / 1e6,
           (unsigned long long)st.texture_uploads, (unsigned long long)st.surface_uploads,
           (unsigned long long)st.readbacks, (unsigned long long)st.submits);
    if (!st.draws || !st.readbacks) {
        printf("FAIL: the Vulkan back end drew nothing\n");
        fail = 1;
    }
    nv2a_vk_shutdown();                     /* saves the pipeline cache */
    fail |= pipeline_cache_checks(pc_path);
    free(s_mem);
    printf(fail ? "nv2a_vk_backend: FAIL\n" : "nv2a_vk_backend: PASS\n");
    return fail;
}
