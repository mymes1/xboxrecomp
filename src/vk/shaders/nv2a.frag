#version 450
/*
 * [android-port] NV2A pixel stage of the Vulkan back end (src/vk/nv2a_vk.c).
 *
 * Every pixel the NV2A writes goes through its texture stages and register
 * combiners. This is the same pipeline the executor's CPU rasteriser runs
 * (xf_rows, rc_stage_fetch in nv2a_pb_exec.c; nv2a_rc_eval in
 * nv2a_combiner.c), rule for rule, so the two can be compared pixel by
 * pixel (tests/nv2a_vk_backend). The rules come from xemu's pixel-shader
 * generator (hw/xbox/nv2a/pgraph/glsl/psh.c); nv2a_combiner.c says where.
 *
 * Two paths, as in the executor:
 *   USE_RC 1  the combiners decide the colour (once a title has programmed
 *             them, which every D3D title does);
 *   USE_RC 0  diffuse, times stage 0's texel when the batch is textured.
 *
 * The combiner program -- the control words that say which register feeds
 * which input of which stage -- arrives as specialization constants, one
 * pipeline per program, so the driver folds every register switch and
 * mapping switch down to the arithmetic that program does: the shader the
 * GPU runs is what xemu would have generated for that program. As uniforms
 * the same code is a register-indexed interpreter, which a tiled mobile GPU
 * runs slowly and a software implementation (SwiftShader) could not even
 * compile within 3 GB. Registers are therefore named variables behind
 * switches, never an array indexed at run time. The data -- per-stage
 * constants, fog colour, alpha reference, border colours -- stays in the
 * uniform buffer, so changing a constant never needs another pipeline.
 *
 * Register numbers (an input or output byte's low nibble):
 *   0 zero  1 c0  2 c1  3 fog  4 v0  5 v1  8-11 t0-t3  12 r0  13 r1
 *   14 v1+r0 sum and 15 E*F product (final combiner only)
 */

layout(constant_id = 0) const uint CI0 = 0u;    /* SET_COMBINER_COLOR_ICW */
layout(constant_id = 1) const uint CI1 = 0u;
layout(constant_id = 2) const uint CI2 = 0u;
layout(constant_id = 3) const uint CI3 = 0u;
layout(constant_id = 4) const uint CI4 = 0u;
layout(constant_id = 5) const uint CI5 = 0u;
layout(constant_id = 6) const uint CI6 = 0u;
layout(constant_id = 7) const uint CI7 = 0u;
layout(constant_id = 8) const uint AI0 = 0u;    /* SET_COMBINER_ALPHA_ICW */
layout(constant_id = 9) const uint AI1 = 0u;
layout(constant_id = 10) const uint AI2 = 0u;
layout(constant_id = 11) const uint AI3 = 0u;
layout(constant_id = 12) const uint AI4 = 0u;
layout(constant_id = 13) const uint AI5 = 0u;
layout(constant_id = 14) const uint AI6 = 0u;
layout(constant_id = 15) const uint AI7 = 0u;
layout(constant_id = 16) const uint CO0 = 0u;   /* SET_COMBINER_COLOR_OCW */
layout(constant_id = 17) const uint CO1 = 0u;
layout(constant_id = 18) const uint CO2 = 0u;
layout(constant_id = 19) const uint CO3 = 0u;
layout(constant_id = 20) const uint CO4 = 0u;
layout(constant_id = 21) const uint CO5 = 0u;
layout(constant_id = 22) const uint CO6 = 0u;
layout(constant_id = 23) const uint CO7 = 0u;
layout(constant_id = 24) const uint AO0 = 0u;   /* SET_COMBINER_ALPHA_OCW */
layout(constant_id = 25) const uint AO1 = 0u;
layout(constant_id = 26) const uint AO2 = 0u;
layout(constant_id = 27) const uint AO3 = 0u;
layout(constant_id = 28) const uint AO4 = 0u;
layout(constant_id = 29) const uint AO5 = 0u;
layout(constant_id = 30) const uint AO6 = 0u;
layout(constant_id = 31) const uint AO7 = 0u;
layout(constant_id = 32) const uint FINAL0 = 0u;        /* SPECULAR_FOG_CW0 */
layout(constant_id = 33) const uint FINAL1 = 0u;        /* SPECULAR_FOG_CW1 */
layout(constant_id = 34) const uint CONTROL = 0u;       /* COMBINER_CONTROL */
layout(constant_id = 35) const uint STAGE_PROGRAM = 0u; /* SHADER_STAGE_PROGRAM */
layout(constant_id = 36) const uint USE_RC = 0u;
layout(constant_id = 37) const uint TEXTURED = 0u;      /* USE_RC 0: stage 0 */
layout(constant_id = 38) const uint CUBE_MASK = 0u;     /* bit s: stage s is a cube */
layout(constant_id = 39) const uint ALPHA_FUNC = 0x207u;/* GL compare; 0x207 no test */

layout(set = 0, binding = 0, std140) uniform Rc {
    vec4  factor0[8];       /* SET_COMBINER_FACTOR0, r g b a */
    vec4  factor1[8];       /* SET_COMBINER_FACTOR1 */
    vec4  final_c0;         /* SET_SPECULAR_FOG_FACTOR */
    vec4  final_c1;
    uvec4 misc;             /* x alpha ref, y border mask (2 bits a stage: u, v),
                               z SHADER_CLIP_PLANE_MODE, w unused */
    vec4  fog_color;        /* r g b a */
    vec4  border[4];        /* SET_TEXTURE_BORDER_COLOR per stage */
} U;

/* Every stage is an array image: one layer, or six for a cube map, whose
 * face the shader picks exactly as rc_stage_fetch does. */
layout(set = 0, binding = 1) uniform sampler2DArray tex0;
layout(set = 0, binding = 2) uniform sampler2DArray tex1;
layout(set = 0, binding = 3) uniform sampler2DArray tex2;
layout(set = 0, binding = 4) uniform sampler2DArray tex3;

layout(push_constant) uniform PC {
    vec4 vp;
    vec4 scale01;           /* stage s: 1/size for linear textures, else 1 */
    vec4 scale23;
} pc;

layout(location = 0) noperspective in vec4 v_d0;
layout(location = 1) noperspective in vec4 v_d1;
layout(location = 2) noperspective in float v_fog;
layout(location = 3) in vec4 v_t0;
layout(location = 4) in vec4 v_t1;
layout(location = 5) in vec4 v_t2;
layout(location = 6) in vec4 v_t3;

layout(location = 0) out vec4 o_color;

/* ---- the register file -------------------------------------------------- */

vec4 R_c0, R_c1, R_fog, R_v0, R_v1, R_6, R_7;
vec4 R_t0, R_t1, R_t2, R_t3, R_r0, R_r1, R_sum, R_ef;

vec4 reg(uint i)
{
    switch (i) {
    case 1u:  return R_c0;
    case 2u:  return R_c1;
    case 3u:  return R_fog;
    case 4u:  return R_v0;
    case 5u:  return R_v1;
    case 6u:  return R_6;
    case 7u:  return R_7;
    case 8u:  return R_t0;
    case 9u:  return R_t1;
    case 10u: return R_t2;
    case 11u: return R_t3;
    case 12u: return R_r0;
    case 13u: return R_r1;
    case 14u: return R_sum;
    case 15u: return R_ef;
    default:  return vec4(0.0);             /* register 0 reads zero */
    }
}

void set_rgb(uint i, vec3 v)
{
    switch (i) {
    case 1u:  R_c0.rgb = v;  break;
    case 2u:  R_c1.rgb = v;  break;
    case 3u:  R_fog.rgb = v; break;
    case 4u:  R_v0.rgb = v;  break;
    case 5u:  R_v1.rgb = v;  break;
    case 6u:  R_6.rgb = v;   break;
    case 7u:  R_7.rgb = v;   break;
    case 8u:  R_t0.rgb = v;  break;
    case 9u:  R_t1.rgb = v;  break;
    case 10u: R_t2.rgb = v;  break;
    case 11u: R_t3.rgb = v;  break;
    case 12u: R_r0.rgb = v;  break;
    case 13u: R_r1.rgb = v;  break;
    case 14u: R_sum.rgb = v; break;
    case 15u: R_ef.rgb = v;  break;
    default:  break;                        /* writes to register 0 vanish */
    }
}

void set_a(uint i, float v)
{
    switch (i) {
    case 1u:  R_c0.a = v;  break;
    case 2u:  R_c1.a = v;  break;
    case 3u:  R_fog.a = v; break;
    case 4u:  R_v0.a = v;  break;
    case 5u:  R_v1.a = v;  break;
    case 6u:  R_6.a = v;   break;
    case 7u:  R_7.a = v;   break;
    case 8u:  R_t0.a = v;  break;
    case 9u:  R_t1.a = v;  break;
    case 10u: R_t2.a = v;  break;
    case 11u: R_t3.a = v;  break;
    case 12u: R_r0.a = v;  break;
    case 13u: R_r1.a = v;  break;
    case 14u: R_sum.a = v; break;
    case 15u: R_ef.a = v;  break;
    default:  break;
    }
}

/* PS_INPUTMAPPING (psh.c get_input_var). */
float map_in(float x, uint byte)
{
    float p = max(x, 0.0);
    switch ((byte >> 5) & 7u) {
    case 0u: return p;                              /* unsigned identity */
    case 1u: return 1.0 - clamp(x, 0.0, 1.0);       /* unsigned invert   */
    case 2u: return 2.0 * p - 1.0;                  /* expand normal     */
    case 3u: return -2.0 * p + 1.0;                 /* expand negate     */
    case 4u: return p - 0.5;                        /* half-bias normal  */
    case 5u: return -p + 0.5;                       /* half-bias negate  */
    case 6u: return x;                              /* signed identity   */
    default: return -x;                             /* signed negate     */
    }
}

vec3 map_in3(vec3 x, uint byte)
{
    vec3 p = max(x, vec3(0.0));
    switch ((byte >> 5) & 7u) {
    case 0u: return p;
    case 1u: return 1.0 - clamp(x, 0.0, 1.0);
    case 2u: return 2.0 * p - 1.0;
    case 3u: return -2.0 * p + 1.0;
    case 4u: return p - 0.5;
    case 5u: return -p + 0.5;
    case 6u: return x;
    default: return -x;
    }
}

/* PS_COMBINEROUTPUT scale/bias (psh.c get_output). */
float map_out(float x, uint m)
{
    switch (m) {
    case 0x08u: return x - 0.5;
    case 0x10u: return x * 2.0;
    case 0x18u: return (x - 0.5) * 2.0;
    case 0x20u: return x * 4.0;
    case 0x30u: return x * 0.5;
    default:    return x;
    }
}

vec3 map_out3(vec3 x, uint m)
{
    switch (m) {
    case 0x08u: return x - 0.5;
    case 0x10u: return x * 2.0;
    case 0x18u: return (x - 0.5) * 2.0;
    case 0x20u: return x * 4.0;
    case 0x30u: return x * 0.5;
    default:    return x;
    }
}

/* In the RGB portion the alpha bit replicates .a; in the alpha portion it
 * picks .a over .b. */
vec3 read_rgb(uint byte)
{
    vec4 r = reg(byte & 0xFu);
    return map_in3((byte & 0x10u) != 0u ? r.aaa : r.rgb, byte);
}

float read_alpha(uint byte)
{
    vec4 r = reg(byte & 0xFu);
    return map_in((byte & 0x10u) != 0u ? r.a : r.b, byte);
}

/* One general combiner stage: both portions read before either writes. */
void rc_stage(uint s, uint icw, uint ocw, uint aicw, uint aocw)
{
    uint flags = CONTROL >> 8;
    uint fl = ocw >> 12, afl = aocw >> 12;
    bool mux_cd;
    vec3 A, B, C, D, pab, pcd, sum, ab, cd, ms;
    float aA, aB, aC, aD, apab, apcd, asum, aab, acd, ams;

    /* One c0/c1 for every stage, or one per stage (COMBINERCOUNT flags). */
    R_c0 = U.factor0[(flags & 0x010u) != 0u ? s : 0u];
    R_c1 = U.factor1[(flags & 0x100u) != 0u ? s : 0u];
    mux_cd = (flags & 1u) != 0u ? R_r0.a >= 0.5
                                : ((int(R_r0.a * 255.0) & 1) != 0);

    A = read_rgb(icw >> 24);  B = read_rgb(icw >> 16);
    C = read_rgb(icw >> 8);   D = read_rgb(icw);
    aA = read_alpha(aicw >> 24);  aB = read_alpha(aicw >> 16);
    aC = read_alpha(aicw >> 8);   aD = read_alpha(aicw);

    pab = (fl & 2u) != 0u ? vec3(dot(A, B)) : A * B;
    pcd = (fl & 1u) != 0u ? vec3(dot(C, D)) : C * D;
    sum = (fl & 4u) != 0u ? (mux_cd ? pcd : pab) : pab + pcd;
    ab = clamp(map_out3(pab, fl & 0x38u), -1.0, 1.0);
    cd = clamp(map_out3(pcd, fl & 0x38u), -1.0, 1.0);
    ms = clamp(map_out3(sum, fl & 0x38u), -1.0, 1.0);

    apab = aA * aB;
    apcd = aC * aD;
    asum = (afl & 4u) != 0u ? (mux_cd ? apcd : apab) : apab + apcd;
    aab = clamp(map_out(apab, afl & 0x38u), -1.0, 1.0);
    acd = clamp(map_out(apcd, afl & 0x38u), -1.0, 1.0);
    ams = clamp(map_out(asum, afl & 0x38u), -1.0, 1.0);

    /* RGB destinations; blue-to-alpha also writes the destination's .a. */
    if (((ocw >> 4) & 0xFu) != 0u) {
        set_rgb((ocw >> 4) & 0xFu, ab);
        if ((fl & 0x80u) != 0u)
            set_a((ocw >> 4) & 0xFu, ab.b);
    }
    if ((ocw & 0xFu) != 0u) {
        set_rgb(ocw & 0xFu, cd);
        if ((fl & 0x40u) != 0u)
            set_a(ocw & 0xFu, cd.b);
    }
    if (((ocw >> 8) & 0xFu) != 0u)
        set_rgb((ocw >> 8) & 0xFu, ms);
    if (((aocw >> 4) & 0xFu) != 0u)
        set_a((aocw >> 4) & 0xFu, aab);
    if ((aocw & 0xFu) != 0u)
        set_a(aocw & 0xFu, acd);
    if (((aocw >> 8) & 0xFu) != 0u)
        set_a((aocw >> 8) & 0xFu, ams);
}

vec4 combine()
{
    uint n = min(CONTROL & 0xFFu, 8u);

    R_c0 = vec4(0.0);  R_c1 = vec4(0.0);
    R_6 = vec4(0.0);   R_7 = vec4(0.0);
    R_r1 = vec4(0.0);  R_sum = vec4(0.0);  R_ef = vec4(0.0);
    /* r0.a starts as texture 0's alpha, or 1 with stage 0 off (psh.c). */
    R_r0 = vec4(0.0, 0.0, 0.0, (STAGE_PROGRAM & 0x1Fu) != 0u ? R_t0.a : 1.0);

    if (n > 0u) rc_stage(0u, CI0, CO0, AI0, AO0);
    if (n > 1u) rc_stage(1u, CI1, CO1, AI1, AO1);
    if (n > 2u) rc_stage(2u, CI2, CO2, AI2, AO2);
    if (n > 3u) rc_stage(3u, CI3, CO3, AI3, AO3);
    if (n > 4u) rc_stage(4u, CI4, CO4, AI4, AO4);
    if (n > 5u) rc_stage(5u, CI5, CO5, AI5, AO5);
    if (n > 6u) rc_stage(6u, CI6, CO6, AI6, AO6);
    if (n > 7u) rc_stage(7u, CI7, CO7, AI7, AO7);

    if (FINAL0 == 0u && FINAL1 == 0u)
        return R_r0;
    {
        uint ff = FINAL1 & 0xFFu;
        vec3 a, b, A, B, C, D, E, F;

        R_c0 = U.final_c0;
        R_c1 = U.final_c1;
        /* V1R0 sum: optional complements (0x40 v1, 0x20 r0), clamp (0x80). */
        a = (ff & 0x40u) != 0u ? 1.0 - R_v1.rgb : R_v1.rgb;
        b = (ff & 0x20u) != 0u ? 1.0 - R_r0.rgb : R_r0.rgb;
        R_sum = vec4((ff & 0x80u) != 0u ? clamp(a + b, 0.0, 1.0) : a + b, 0.0);
        E = read_rgb(FINAL1 >> 24);
        F = read_rgb(FINAL1 >> 16);
        R_ef = vec4(E * F, 0.0);
        A = read_rgb(FINAL0 >> 24);  B = read_rgb(FINAL0 >> 16);
        C = read_rgb(FINAL0 >> 8);   D = read_rgb(FINAL0);
        /* D + mix(C, B, A), alpha from G. */
        return vec4(D + C * (1.0 - A) + B * A, read_alpha(FINAL1 >> 8));
    }
}

/* ---- texture stages ------------------------------------------------------ */

vec4 sample_stage(uint st, vec3 uvl)
{
    switch (st) {
    case 0u:  return texture(tex0, uvl);
    case 1u:  return texture(tex1, uvl);
    case 2u:  return texture(tex2, uvl);
    default:  return texture(tex3, uvl);
    }
}

vec2 stage_scale(uint st)
{
    switch (st) {
    case 0u:  return pc.scale01.xy;
    case 1u:  return pc.scale01.zw;
    case 2u:  return pc.scale23.xy;
    default:  return pc.scale23.zw;
    }
}

/* uv normalised. Address mode BORDER with the border colour selected reads
 * SET_TEXTURE_BORDER_COLOR outside the image; the other modes are the
 * sampler's (wrap, mirror, clamp). */
vec4 texel2d(uint st, vec2 uv, float layer)
{
    uint bm = (U.misc.y >> (st * 2u)) & 3u;
    if (((bm & 1u) != 0u && (uv.x < 0.0 || uv.x >= 1.0))
     || ((bm & 2u) != 0u && (uv.y < 0.0 || uv.y >= 1.0)))
        return U.border[st];
    return sample_stage(st, vec3(uv, layer));
}

bool g_kill = false;

/* What texture stage `st` contributes, by its SHADER_STAGE_PROGRAM mode
 * (rc_stage_fetch; xemu psh.c). A clip-plane stage may kill the pixel. */
vec4 stage_fetch(uint st, vec4 c)
{
    uint mode = (STAGE_PROGRAM >> (st * 5u)) & 0x1Fu;
    float q = c.w != 0.0 ? c.w : 1.0;

    switch (mode) {
    case 0u:                                        /* NONE */
        return vec4(0.0, 0.0, 0.0, 1.0);
    case 1u:                                        /* PROJECT2D */
    case 2u:                                        /* PROJECT3D */
        return texel2d(st, c.xy / q * stage_scale(st), 0.0);
    case 3u: {                                      /* CUBEMAP */
        vec3 ac = abs(c.xyz);
        float ma, s, t, face;
        if (((CUBE_MASK >> st) & 1u) == 0u)
            return texel2d(st, c.xy * stage_scale(st), 0.0);
        if (ac.x >= ac.y && ac.x >= ac.z) {
            face = c.x > 0.0 ? 0.0 : 1.0;  ma = ac.x;
            s = c.x > 0.0 ? -c.z : c.z;    t = -c.y;
        } else if (ac.y >= ac.z) {
            face = c.y > 0.0 ? 2.0 : 3.0;  ma = ac.y;
            s = c.x;                       t = c.y > 0.0 ? c.z : -c.z;
        } else {
            face = c.z > 0.0 ? 4.0 : 5.0;  ma = ac.z;
            s = c.z > 0.0 ? c.x : -c.x;    t = -c.y;
        }
        if (ma == 0.0)
            ma = 1.0;
        return texel2d(st, vec2(s / ma + 1.0, t / ma + 1.0) * 0.5, face);
    }
    case 4u:                                        /* PASSTHRU */
        return clamp(c, 0.0, 1.0);
    case 5u: {                                      /* CLIPPLANE */
        for (uint j = 0u; j < 4u; j++) {
            bool ge = ((U.misc.z >> (st * 4u + j)) & 1u) != 0u;
            if (ge ? c[j] >= 0.0 : c[j] < 0.0)
                g_kill = true;
        }
        return vec4(0.0);
    }
    default:
        /* ponytail: bump-env and dot-product modes read zero, as on the
         * CPU rasteriser; add them when a title's look depends on one. */
        return vec4(0.0);
    }
}

bool alpha_pass(float a)
{
    int v = int(clamp(a, 0.0, 1.0) * 255.0 + 0.5);
    int r = int(U.misc.x & 0xFFu);
    switch (ALPHA_FUNC) {
    case 0x200u: return false;                      /* NEVER    */
    case 0x201u: return v <  r;                     /* LESS     */
    case 0x202u: return v == r;                     /* EQUAL    */
    case 0x203u: return v <= r;                     /* LEQUAL   */
    case 0x204u: return v >  r;                     /* GREATER  */
    case 0x205u: return v != r;                     /* NOTEQUAL */
    case 0x206u: return v >= r;                     /* GEQUAL   */
    default:     return true;                       /* ALWAYS, or no test */
    }
}

void main()
{
    vec4 c;

    if (USE_RC != 0u) {
        R_v0 = v_d0;
        R_v1 = v_d1;
        /* Fog colour with the fog unit's factor in .a. */
        R_fog = vec4(U.fog_color.rgb, v_fog);
        R_t0 = (STAGE_PROGRAM & 0x1Fu) != 0u ? stage_fetch(0u, v_t0)
                                              : vec4(0.0, 0.0, 0.0, 1.0);
        R_t1 = ((STAGE_PROGRAM >> 5) & 0x1Fu) != 0u ? stage_fetch(1u, v_t1)
                                                     : vec4(0.0, 0.0, 0.0, 1.0);
        R_t2 = ((STAGE_PROGRAM >> 10) & 0x1Fu) != 0u ? stage_fetch(2u, v_t2)
                                                      : vec4(0.0, 0.0, 0.0, 1.0);
        R_t3 = ((STAGE_PROGRAM >> 15) & 0x1Fu) != 0u ? stage_fetch(3u, v_t3)
                                                      : vec4(0.0, 0.0, 0.0, 1.0);
        if (g_kill)
            discard;
        c = combine();
    } else {
        c = v_d0;
        if (TEXTURED != 0u)
            c *= texel2d(0u, v_t0.xy * pc.scale01.xy, 0.0);
    }
    c = clamp(c, 0.0, 1.0);
    if (!alpha_pass(c.a))
        discard;
    o_color = c;
}
