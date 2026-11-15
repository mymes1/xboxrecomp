/*
 * NV2A render back-end interface.
 *
 * The pushbuffer executor (nv2a_pb_exec.c) decodes what a title asks the GPU
 * to do. By default it carries that out itself, on the CPU, into guest memory.
 * A game project can instead register a back end -- a D3D11 renderer, say --
 * and the executor hands it the work in already-decoded form:
 *
 *   surface state + clears, triangles (transformed to surface pixels, with
 *   per-vertex colour and texel-space UVs), the texture each batch samples,
 *   and the flip that ends a frame.
 *
 * Everything is called on the NV2A poll thread, one call at a time, so a back
 * end may create its window and device lazily on the first call and pump its
 * window messages from flip().
 *
 * Fixed-function, pre-transformed and vertex-program batches all arrive the
 * same way: the executor transforms fixed-function vertices itself, runs
 * vertex programs on the CPU (nv2a_vsh_interp.c) and clips at the near plane,
 * so a back end only ever sees surface pixels. Blend, depth, alpha, cull and
 * stencil state arrive in Nv2aRenderState.
 *
 * draw() sees stage 0 and the diffuse colour only. [android-port] A back end
 * that runs the whole pixel pipeline -- register combiners, four texture
 * stages, specular, fog, alpha test -- sets draw_ex instead: Nv2aBatchEx
 * carries every per-vertex value the executor's own rasteriser interpolates
 * and the per-batch state it reads (see "Extended batches" below). clear_ex
 * likewise gets the surface's format and the exact rectangle the CPU path
 * clears. A back end sets either form or both; the extended one wins.
 */
#ifndef NV2A_BACKEND_H
#define NV2A_BACKEND_H

#include <stdint.h>
#include "nv2a_combiner.h"      /* Nv2aCombiner, for Nv2aBatchEx */

/* The colour surface being drawn into, in real (anti-aliased) pixels.
 * aa_sx/aa_sy give the anti-aliasing factor, so width/aa_sx is the logical
 * size the title thinks it renders at (e.g. 640x480). */
typedef struct {
    uint32_t color_va;          /* guest address of pixel (0,0) */
    uint32_t width, height;     /* clip rectangle size, real pixels */
    uint32_t pitch;             /* bytes per row */
    uint32_t bytes_per_pixel;   /* 2 or 4 */
    uint32_t aa_sx, aa_sy;      /* 1 or 2 each */
} Nv2aSurface;

/* A texture as the title programmed it. uv in Nv2aVertex are in texels. */
typedef struct {
    uint32_t offset;            /* guest address of texel (0,0) */
    uint32_t width, height;
    uint32_t pitch;             /* linear formats only */
    uint32_t color;             /* NV097 colour-format code */
    uint32_t addr_u, addr_v;    /* NV097 wrap mode per axis (1 wrap, 3 clamp) */
    /* Mip levels stored after level 0 (the format word's MIPMAP_LEVELS; 1 for
     * a linear texture) and the raw SET_TEXTURE_FILTER word (MIN in bits
     * 16-23). A back end that ignores them samples level 0. */
    uint32_t levels, filter;
} Nv2aTexture;

typedef struct {
    float    x, y, z;           /* surface pixels; z as the title produced it */
    float    rhw;               /* 1/w, 1 for pre-transformed batches */
    uint32_t diffuse;           /* 0xAARRGGBB */
    float    u, v;              /* texels */
} Nv2aVertex;

/* Render state as the title set it, in NV2A's own (OpenGL) enum values:
 * blend factors GL_ZERO/GL_ONE/0x300..0x308/0x8001..0x8004, equations
 * GL_FUNC_ADD 0x8006 etc., compare functions GL_NEVER 0x200 .. GL_ALWAYS 0x207.
 * color_mask uses NV2A's bytes: A 0x01000000, R 0x00010000, G 0x100, B 0x1. */
typedef struct {
    uint32_t blend_enable, blend_src, blend_dst, blend_eq, blend_color;
    uint32_t alpha_test_enable, alpha_func, alpha_ref;   /* ref 0..255 */
    uint32_t depth_test_enable, depth_func, depth_write;
    uint32_t color_mask;
    uint32_t cull_enable, cull_face, front_face;
    uint32_t zeta_va;                                    /* 0: no depth surface */
    float    depth_min, depth_max;                       /* SET_CLIP_MIN/MAX */
    /* SET_STENCIL_*: compare function as above; ops GL_KEEP 0x1E00, GL_ZERO 0,
     * GL_REPLACE 0x1E01, GL_INCR 0x1E02, GL_DECR 0x1E03, GL_INVERT 0x150A,
     * GL_INCR_WRAP 0x8507, GL_DECR_WRAP 0x8508. Reset values GL_ALWAYS, masks
     * 0xFF, GL_KEEP. A title can confine a pass to the pixels an earlier pass
     * marked (a full-screen quad that only marked pixels pass); a back end
     * that ignores these draws that pass over the whole surface. */
    uint32_t stencil_enable, stencil_func, stencil_ref, stencil_func_mask;
    uint32_t stencil_write_mask, stencil_fail, stencil_zfail, stencil_zpass;
} Nv2aRenderState;

typedef struct {
    const Nv2aVertex  *vertices;    /* triangle list: count is a multiple of 3 */
    uint32_t           count;
    const Nv2aTexture *texture;     /* NULL: untextured */
    const Nv2aRenderState *state;
} Nv2aBatch;

/* ---- Extended batches [android-port] ---------------------------------
 *
 * What the executor's own rasteriser (xf_rows in nv2a_pb_exec.c) works from,
 * so a GPU back end can produce the same pixels: */

/* The colour surface with its format. base is what draw() gets. */
typedef struct {
    Nv2aSurface base;
    uint32_t clip_x, clip_y;        /* clip origin, real pixels */
    uint32_t color_format;          /* SET_SURFACE_FORMAT bits 0-3 */
    uint32_t zeta_format;           /* bits 4-7: 1 Z16, 2 Z24S8 (depth range) */
    uint32_t swizzled;              /* type 2: pixels in Morton order */
    uint32_t swz_width, swz_height; /* that surface's size, when swizzled */
} Nv2aSurfaceEx;

/* One vertex. pos: x, y in real surface pixels, z the window z as the title
 * produced it (compared as it is; see docs/technical/pushbuffer-executor.md),
 * w the clip-space w (perspective-correct texturing; 1/rhw for a
 * pre-transformed vertex). Colours are r, g, b, a in [0,1] and interpolate
 * linearly in screen space, as the CPU rasteriser does; fog is the fog
 * unit's factor (1 = no fog). tex: stage s's s, t, r, q as the vertex
 * program wrote them -- normalised for swizzled and DXT textures, texels for
 * linear ones (Nv2aStage.normalized) -- interpolated perspective-correct. */
typedef struct {
    float pos[4];
    float d0[4], d1[4];
    float fog;
    float tex[4][4];
} Nv2aVertexEx;

/* A texture stage as programmed. tex.levels/filter as in Nv2aTexture. */
typedef struct {
    Nv2aTexture tex;
    uint32_t valid;                 /* the stage can be sampled */
    uint32_t palette;               /* guest address of the CLUT (P8), or 0 */
    uint32_t border;                /* SET_TEXTURE_BORDER_COLOR, 0xAARRGGBB */
    uint32_t border_color;          /* FORMAT: address mode 4 reads `border` */
    uint32_t cube;                  /* six faces of face_stride bytes each */
    uint32_t face_stride;
    uint32_t normalized;            /* coordinates in [0,1) (swizzled, DXT),
                                       not texels (linear formats) */
} Nv2aStage;

/* The combiners decide each pixel when use_rc is set; otherwise the pixel is
 * d0, times stage 0's texel when `textured` -- exactly the executor's two
 * paths. A stage contributes when combiner.stage_program says so (5 bits a
 * stage). Fog colour as sent: R in bits 0-7.
 *
 * screen_space: the batch came through the executor's screen-space path (a
 * batch neither a vertex program nor the fixed-function pipeline
 * transforms), which raster_triangle draws with no depth test or write, no
 * alpha test and no combiners. The executor has already shaped its vertices
 * for the rule above: w = 1 (affine texturing), d0 the flat colour, or
 * white when textured (the texel replaces the colour). */
typedef struct {
    const Nv2aVertexEx    *vertices; /* triangle list */
    uint32_t               count;
    const Nv2aRenderState *state;
    Nv2aStage              stages[4];
    Nv2aCombiner           combiner;
    uint32_t               use_rc, textured;
    uint32_t               fog_color;
    uint32_t               clip_plane_mode;     /* SET_SHADER_CLIP_PLANE_MODE */
    uint32_t               screen_space;
} Nv2aBatchEx;

typedef struct {
    /* flags: CLEAR_SURFACE bits (Z 0x1, stencil 0x2, colour 0xF0).
     * zstencil: SET_ZSTENCIL_CLEAR_VALUE (depth in the top 24 bits for Z24S8). */
    void (*clear)(const Nv2aSurface *s, const Nv2aRenderState *rs,
                  uint32_t flags, uint32_t argb, uint32_t zstencil);
    void (*draw)(const Nv2aSurface *s, const Nv2aBatch *b);
    void (*flip)(void);
    /* [android-port] Optional; when set they are called instead of draw and
     * clear. rect: x0, y0, x1, y1 (exclusive), real pixels -- the surface
     * clip, or CLEAR_RECT on a swizzled surface, as the CPU path clears. */
    void (*draw_ex)(const Nv2aSurfaceEx *s, const Nv2aBatchEx *b);
    void (*clear_ex)(const Nv2aSurfaceEx *s, const Nv2aRenderState *rs,
                     uint32_t flags, uint32_t argb, uint32_t zstencil,
                     const uint32_t rect[4]);
} Nv2aBackend;

/* Register (or, with NULL, remove) the back end. Call before the title starts
 * submitting work; the executor must also be enabled (RECOMP_PB_EXEC). */
void nv2a_backend_register(const Nv2aBackend *backend);

/* Register a back end that must synchronize a surface into guest memory
 * before the flip's window snapshot or framebuffer dump. present_sync runs
 * synchronously on the executor thread and must complete pending GPU work and
 * readback before returning; it must not present a window. flip still runs
 * after the snapshot and dump. A NULL present_sync disables synchronization;
 * a NULL backend removes both registrations. A later nv2a_backend_register
 * call replaces both registrations. */
void nv2a_backend_register_with_present_sync(const Nv2aBackend *backend,
                                             void (*present_sync)(void));

/* Decode a whole texture to 0xAARRGGBB, row-major, width*height entries.
 * Handles every format the executor can sample (swizzled, linear, DXT).
 * Returns 0 if the format is not supported. */
int nv2a_backend_decode_texture(const Nv2aTexture *tex, uint32_t *argb_out);

/* [android-port] Decode one face of a stage's level 0 to 0xAARRGGBB, as the
 * executor samples it: every format nv2a_backend_decode_texture handles,
 * plus palettised textures (the stage's CLUT) and cube faces (face 0-5).
 * Returns 0 if the format is not supported. */
int nv2a_backend_decode_stage(const Nv2aStage *stage, uint32_t face,
                              uint32_t *argb_out);

/* [android-port] How many guest bytes a stage's level 0 spans from
 * tex.offset, every face included: what a back end hashes to notice a
 * rewritten texture, and compares with surfaces it has drawn into. 0 for a
 * stage that cannot be sampled. */
uint32_t nv2a_backend_stage_bytes(const Nv2aStage *stage);

/* Where BACK_END_WRITE_SEMAPHORE_RELEASE (0x1D70) values land: the guest VA
 * of the semaphore the title reads GPU progress from (for XDK D3D, the
 * pointer at device+0x30). The executor writes each release there once it
 * has executed everything before it -- which is
 * what lets D3D's fence waits and ring-space checks see real progress.
 *
 * ponytail: the title supplies the address instead of the executor resolving
 * the semaphore context DMA object through RAMIN. */
void nv2a_pb_set_semaphore_target(uint32_t guest_va);

#endif /* NV2A_BACKEND_H */
