#version 450
/*
 * [android-port] NV2A vertex stage of the Vulkan back end (src/vk/nv2a_vk.c).
 *
 * By the time a vertex gets here the executor has already run the title's
 * vertex program (or the fixed-function transform) on the CPU and clipped
 * the triangle at the near plane, exactly as for its own rasteriser. So a
 * vertex arrives in surface pixels, with the window z the title produced and
 * the clip-space w (Nv2aVertexEx). All this stage does is map pixels to
 * normalised device coordinates and multiply back by w, so that the
 * rasteriser interpolates texture coordinates perspective-correct.
 *
 * Colours and fog are `noperspective`: the CPU rasteriser (xf_rows in
 * nv2a_pb_exec.c) interpolates them linearly in screen space, and the two
 * renderers must agree pixel for pixel (tests/nv2a_vk_backend).
 *
 * Depth is the window z scaled by 1/zmax (0xFFFF for Z16, 0xFFFFFF for
 * Z24S8): the window z is compared as it is, never re-ranged
 * (docs/technical/pushbuffer-executor.md, "Render Back Ends").
 */

layout(location = 0) in vec4 a_pos;     /* x, y pixels; z window; w clip w */
layout(location = 1) in vec4 a_d0;
layout(location = 2) in vec4 a_d1;
layout(location = 3) in float a_fog;
layout(location = 4) in vec4 a_t0;
layout(location = 5) in vec4 a_t1;
layout(location = 6) in vec4 a_t2;
layout(location = 7) in vec4 a_t3;

layout(push_constant) uniform PC {
    vec4 vp;        /* 2/width, 2/height, 1/zmax, unused */
    vec4 scale01;   /* stages 0 and 1: 1/size for linear textures, else 1 */
    vec4 scale23;   /* stages 2 and 3 */
} pc;

layout(location = 0) noperspective out vec4 v_d0;
layout(location = 1) noperspective out vec4 v_d1;
layout(location = 2) noperspective out float v_fog;
layout(location = 3) out vec4 v_t0;
layout(location = 4) out vec4 v_t1;
layout(location = 5) out vec4 v_t2;
layout(location = 6) out vec4 v_t3;

void main()
{
    float w = a_pos.w;
    vec2 ndc = a_pos.xy * pc.vp.xy - 1.0;

    gl_Position = vec4(ndc * w, a_pos.z * pc.vp.z * w, w);
    v_d0 = a_d0;
    v_d1 = a_d1;
    v_fog = a_fog;
    v_t0 = a_t0;
    v_t1 = a_t1;
    v_t2 = a_t2;
    v_t3 = a_t3;
}
