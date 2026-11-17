/* Native per-particle loop of the particle draw F_00479e30 (0x47a0b3..0x47a836), with its callees: F_00476800 (projection
 * through D3DXVec4Transform x87 0x648491), D3DXVec4Transform (fog-box test), D3DXVec3TransformNormal (0x648432),
 * D3DXVec3Normalize (0x64856f), __ftol, floor + __ftol (F_00477210), fabs (F_00477260) and the screen helpers F_00682ba0,
 * F_00682bd0 and F_00682c00 (= F_00413e10). The vertex allocator F_005f5990 and the commit F_005f5ad0 stay guest calls.
 *
 * Numeric contract: x87 with 24-bit precision control (PC, Direct3D 8 without FPU_PRESERVE), i.e. binary32 operations in
 * the original order (see shadow_native.c). Operations with an exactly loaded integer (fild/fimul/fidiv operands) round the
 * exact result once: mul_d/div_d/add_d compute in double and correct the only case where double rounding differs (a double
 * result exactly on a binary32 midpoint), using an exact error term (Dekker product, TwoSum, exact remainder).
 * Verified against the original bytes under Unicorn (PC24): tools/particle_draw_native_proof.py.
 *
 * Guest-visible effects reproduced: the particle fields +0x30..+0x44 written by the loop, the 6 vertices per particle written
 * through F_005f5990, the guest calls F_005f5990/F_005f5ad0 with the original arguments, esp and x87 condition codes C0/C2/C3
 * at each guest call and at loop exit, the loop counter [esp+0x4c]. Not reproduced: the other frame locals of the loop and
 * eax..edi at loop exit (the epilogue at 0x47a836 reloads every register it reads, and the locals are dead after the loop).
 * Preconditions checked on entry, else the translated loop runs: the lazy D3DX table is initialised with the x87
 * implementations above and the game screen size is set ([0x72bfd8] != 0, so F_00402ba0/F_00402bc0 read it directly).
 * The port wraps F_00682ba0 with the widescreen interface hook (diag_widescreen_hook.c, on by default on the Vita): when it
 * is active, the same remap is applied to the x87 value here (not covered by the Unicorn proof: the PC has no such hook).
 * Off unless g_particle_draw_native. */
#include <stdint.h>
#include <string.h>
#include <math.h>
#include <stdio.h>
#include "x86rt.h"

int g_particle_draw_native;
void F_005f5990(CPU *c);
void F_005f5ad0(CPU *c);
int diag_wide_interface_active(void);

typedef struct { uint32_t c_bits; int used; } Fsw;
static inline void fcmp(Fsw *s, float a, float b) {
    s->used = 1;
    s->c_bits = a < b ? FSW_C0 : a > b ? 0 : a == b ? FSW_C3 : FSW_C0 | FSW_C2 | FSW_C3;
}
static inline void fsw_out(CPU *c, Fsw *s) {
    if (s->used) c->fsw = (c->fsw & ~(FSW_C0 | FSW_C2 | FSW_C3)) | s->c_bits;
    s->used = 0;
}
static inline float gf(uint32_t a) { float f; memcpy(&f, g_mem + a, 4); return f; }
static inline void sf(uint32_t a, float f) { memcpy(g_mem + a, &f, 4); }

/* Single rounding of an exact result: p is the double-rounded result; when it sits exactly on a binary32 midpoint
   (low 29 bits = 1 followed by zeros), the sign of the exact error decides. Valid in the binary32 normal range. */
static inline int on_midpoint(double p) { uint64_t b; memcpy(&b, &p, 8); return (b & 0x1fffffffull) == 0x10000000ull; }
static inline float fix_round(double p, double e) { return e == 0 ? (float)p : (float)nextafter(p, e > 0 ? INFINITY : -INFINITY); }
static inline double prod_err(double a, double b, double p) {   /* Dekker: a*b - p exactly (needs -ffp-contract=off) */
    double t = 134217729.0 * a, ah = t - (t - a), al = a - ah;
    double u = 134217729.0 * b, bh = u - (u - b), bl = b - bh;
    return ((ah * bh - p) + ah * bl + al * bh) + al * bl;
}
static inline float mul_d(double a, double b) { double p = a * b; return on_midpoint(p) ? fix_round(p, prod_err(a, b, p)) : (float)p; }
static inline float add_d(double a, double b) {
    double s = a + b;
    if (!on_midpoint(s)) return (float)s;
    double bb = s - a; return fix_round(s, (a - (s - bb)) + (b - bb));
}
static inline float div_d(double a, double b) {
    double q = a / b;
    if (!on_midpoint(q)) return (float)q;
    double ph = q * b, r = (a - ph) - prod_err(q, b, ph);
    return fix_round(q, r == 0 ? 0.0 : (r > 0) == (b > 0) ? 1.0 : -1.0);   /* exact tie: (float)q rounds to even */
}
/* __ftol: truncation to int64, integer indefinite for NaN or out of range; callers keep eax (low 32 bits). */
static inline int32_t ftol32(float v) {
    int64_t t = (v == v && v >= -9223372036854775808.0f && v < 9223372036854775808.0f) ? (int64_t)v : INT64_MIN;
    return (int32_t)(uint32_t)t;
}
/* floor (CRT, SSE2 or x87 path) + __ftol. The x87 path (the one the port takes: its translated stmxcsr traps, so
   [0x7179b44] stays 0) ends with fcomp floor(v), v, which leaves C0/C3; the SSE2 path leaves the flags. Finite v only. */
static inline int32_t floor_int(Fsw *fs, int x87, float v) {
    float f = floorf(v);
    if (x87 && isfinite(v)) fcmp(fs, f, v);
    return ftol32(f);
}

/* D3DXVec4Transform x87: out[j] = ((v0*m[j] + v1*m[4+j]) + v2*m[8+j]) + v3*m[12+j]. */
static inline void vec4_transform(float *o, const float *v, uint32_t ma) {
    float m[16], r[4]; memcpy(m, g_mem + ma, 64);
    for (int j = 0; j < 4; j++) r[j] = ((v[0] * m[j] + v[1] * m[4 + j]) + v[2] * m[8 + j]) + v[3] * m[12 + j];
    memcpy(o, r, 16);
}
/* D3DXVec3TransformNormal x87: out[j] = (v0*m[j] + v1*m[4+j]) + v2*m[8+j]. */
static inline void vec3_normal(float *o, const float *v, uint32_t ma) {
    float m[12], r[3]; memcpy(m, g_mem + ma, 48);
    for (int j = 0; j < 3; j++) r[j] = (v[0] * m[j] + v[1] * m[4 + j]) + v[2] * m[8 + j];
    memcpy(o, r, 12);
}
/* D3DXVec3Normalize x87, in place (table reciprocal square root); ponytail: duplicate of shadow_native.c's, both labs are separate. */
static void vec3_normalize(float *v) {
    float s = (v[0] * v[0] + v[1] * v[1]) + v[2] * v[2];
    uint32_t sb; memcpy(&sb, &s, 4);
    if (!sb) { memset(v, 0, 12); return; }
    float d = s - 1.0f; uint32_t db; memcpy(&db, &d, 4);
    if ((int32_t)(db & 0x7fffffffu) <= 0x3727c5ac) return;
    uint32_t idx = (sb >> 12) & 0xff8, mbits = (sb & 0xffffffu) | 0x3f000000u, ebits = ((0xbeffffffu - sb) >> 1) & 0xff800000u;
    float m, e; memcpy(&m, &mbits, 4); memcpy(&e, &ebits, 4);
    float r = (m * gf(0x726820u + idx) + gf(0x726824u + idx)) * e;
    v[0] = v[0] * r; v[1] = v[1] * r; v[2] = v[2] * r;
}
/* F_00682ba0 / F_00682bd0: ((arg*k0 - k1 + k2) * size) * k3, size = [0x72bfd0] / [0x72bfd4] (F_00402ba0 / F_00402bc0). */
static inline float scr(uint32_t size_a, uint32_t k2, uint32_t k3, uint32_t arg) {
    uint8_t *const mb = g_mem;
    float t = mul_d((double)(int32_t)arg, gf(0x68e6e0u));
    t = t - gf(0x68e6dcu); t = t + gf(k2);
    return mul_d((double)(int32_t)R32(size_a), t) * gf(k3);
}
/* F_00682ba0 as the port runs it: through diag_wide_interface_x when the widescreen interface hook is active. */
static inline float scr_x(int wide, uint32_t arg) {
    float x = scr(0x72bfd0u, 0x68e6d8u, 0x68e6d4u, arg);
    return wide ? (float)(480.0 + ((double)x - 480.0) * ((4.0 / 3.0) / (960.0 / 544.0))) : x;
}
/* F_00413e10: 1 - (arg - z0) / (0x20000 - z0), integer operands. */
static inline float scr_z(uint32_t arg) {
    uint8_t *const mb = g_mem;
    uint32_t z0 = R32(0x72c274u);
    return gf(0x68a754u) - div_d((double)(int32_t)(arg - z0), (double)(int32_t)(0x20000u - z0));
}
static inline void gcall(CPU *c, Fsw *fs, gfn f, uint32_t ret) {
    fsw_out(c, fs); c->esp -= 4; uint8_t *const mb = g_mem; W32(c->esp, ret); f(c);
}

unsigned g_pd_native, g_pd_refused;   /* loop runs taken / refused (logged per A/B window) */
int particle_draw_native(CPU *c) {
    uint8_t *const mb = g_mem;
    if (R32(0x72601cu) == 0xffffu || R32(0x7257e0u) != 0x648491u || R32(0x7257f0u) != 0x648432u || R32(0x7257f4u) != 0x64856fu ||
        !R32(0x72bfd8u)) { g_pd_refused++; return 0; }
    g_pd_native++;
    { static int once; if (!once) { once = 1; fprintf(stderr, "[particle-draw] native loop running (widescreen remap %d)\n", diag_wide_interface_active()); } }
    /* Particles are anchored in the 3D world and use the widened projection: no interface squeeze (diag_widescreen_hook.c). */
    const int wide = 0; (void)diag_wide_interface_active;
    const uint32_t S = c->esp, ebp = c->ebp;
    const float zero = gf(0x68a868u), one = gf(0x68a754u), c25 = gf(0x68f030u), k16 = gf(0x68ecf4u);
    const float s24 = gf(S + 0x24), s70 = gf(S + 0x70), sc0 = gf(S + 0xc0);
    const int x87floor = !R32(0x7179b44u) || (c->fcw & 0x7fu) != 0x7fu;   /* MXCSR assumed at its default 0x1f80 */
    Fsw fs = {0, 0};
    uint32_t P = 0x712e3c0u;
    int32_t i = 0;
    do {
        #define PV(k) gf(P + 4u * (k))
        /* F_00476800(P, [0x7143e20]): homogeneous projection. */
        float T[4] = {PV(0), PV(1), PV(2), 1.0f}, V[4];
        vec4_transform(V, T, 0x7143e20u);
        float r = one / V[3];
        V[0] = r * V[0]; V[1] = r * V[1]; V[2] = r * V[2];
        float q = one / r, life = PV(0x12);
        fcmp(&fs, life, zero); if (life < zero || life == zero) goto next;
        fcmp(&fs, q, s24); if (q < s24) goto next;
        uint32_t color;
        if (R8(0x7143f88u) & 4) {
            float Tp[4] = {PV(0), PV(1), PV(2), PV(3)}, U[4];
            vec4_transform(U, Tp, 0x89eb10u);
            float t = -U[3]; int in = 0;
            fcmp(&fs, U[0], t);
            if (!(U[0] < t)) { fcmp(&fs, U[0], U[3]);
            if (!(U[0] > U[3])) { fcmp(&fs, U[1], t);
            if (!(U[1] < t)) { fcmp(&fs, U[1], U[3]);
            if (!(U[1] > U[3])) { fcmp(&fs, U[2], t);
            if (!(U[2] < t)) { fcmp(&fs, U[2], U[3]);
            if (!(U[2] > U[3])) in = 1; } } } } }
            color = in ? R8(0x6b8308u) : R8(0x6b8310u);
        } else color = R8(0x6b8310u);
        float d = gf(0x7143f70u) - PV(1);
        fcmp(&fs, d, zero); if (d < zero || d == zero) goto next;
        float H = gf(0x7143f60u), a, s48;
        fcmp(&fs, d, H);
        if (!(d >= H)) {
            float x = d / H;
            fcmp(&fs, x, s70);
            s48 = !(x >= s70) ? x / s70 : 1.0f;
            a = x;
        } else { a = one; s48 = 1.0f; }
        float g = a * life;
        fcmp(&fs, q, sc0);
        if (q < sc0) g = (g * (q - s24)) * gf(0x68e6d4u);
        const int32_t ia = ftol32(g), Z = ftol32(V[2]), X = ftol32(V[0] * k16), Y = ftol32(V[1] * k16);
        const float sz = ((H * gf(0x7143f64u)) * r) * k16;
        float N[3], Pn[3] = {PV(4), PV(5), PV(6)};
        vec3_normal(N, Pn, 0x7143e60u);
        float m = N[1] * N[0];
        fcmp(&fs, m, zero);
        float B = (m < zero ? -sqrtf(-m) : sqrtf(m)) * gf(0x68eff8u);
        float a0 = sqrtf(fabsf(N[0])) * gf(0x68ef18u), a1 = sqrtf(fabsf(N[1])) * gf(0x68ef18u);
        const float k38 = gf(0x690638u), k34 = gf(0x690634u);
        float omb = one - B;
        float t1 = omb - a0; fcmp(&fs, t1, c25); if (!(t1 > c25)) t1 = 0.25f;
        float t2 = omb - a1; fcmp(&fs, t2, c25); if (!(t2 > c25)) t2 = c25;
        const int32_t i1 = ftol32(t1 * sz), i2 = ftol32((sz * t2) * k38), i3 = ftol32(((t2 * s48) * sz) * k34);
        float bp1 = B + one;
        float u1 = bp1 - a0; fcmp(&fs, u1, c25); if (u1 < c25) u1 = 0.25f;
        float u2 = bp1 - a1; fcmp(&fs, u2, c25); if (u2 < c25) u2 = c25;
        const int32_t j1 = ftol32(u1 * sz), j2 = ftol32((sz * u2) * k38), j3 = ftol32(((u2 * s48) * sz) * k34);
        /* Direction from the camera, normalised; the particle keeps it in +0x30 and the previous one feeds the drift. */
        float D[3] = {PV(0) - gf(0x7143ea0u), PV(1) - gf(0x7143ea4u), PV(2) - gf(0x7143ea8u)};
        vec3_normalize(D);
        float E[3] = {D[0] - PV(12), D[1] - PV(13), D[2] - PV(14)}, F[3];
        sf(P + 0x30, D[0]); sf(P + 0x34, D[1]); sf(P + 0x38, D[2]); W32(P + 0x3c, R32(P + 0xc));
        vec3_normal(F, E, 0x7143e60u);
        float G0 = PV(16) - F[0] * c25, G1 = PV(17) - F[1] * c25;
        float G0p = add_d(G0, -(double)floor_int(&fs, x87floor, G0)), G1p = add_d(G1, -(double)floor_int(&fs, x87floor, G1));
        sf(P + 0x40, G0p); sf(P + 0x44, G1p);
        const float e8 = gf(0x6b82e8u);
        const float um = G0p - e8, vm = G1p - e8, up = G0p + e8, vp = e8 * s48 + G1p;
        const uint32_t R = ((R32(0x7143f8cu) & 0xffu) * color) >> 7, Gc = (R8(0x7143f8du) * color) >> 7, Bc = (R8(0x7143f8eu) * color) >> 7;
        const uint32_t alpha = ((uint32_t)R8(0x7143f8fu) * (uint32_t)ia) >> 7;
        if (!alpha) goto next;
        c->esp = S - 20; W32(S - 20, 6); W32(S - 16, 1); W32(S - 12, 2); W32(S - 8, R32(ebp + 8)); W32(S - 4, R32(ebp + 0xc));
        c->ebp = ebp; gcall(c, &fs, F_005f5990, 0x47a641u);
        const uint32_t vb = c->eax, rgb = ((R << 8 | Gc) << 8) | Bc;
        uint32_t a2 = alpha * 2; if (a2 > 0xffu) a2 = 0xffu;
        const float z = scr_z((uint32_t)Z);
        #define VTX(o, xa, ya, col, u, v) do { sf(vb + (o), scr_x(wide, (uint32_t)(xa))); \
            sf(vb + (o) + 4, scr(0x72bfd4u, 0x697ba8u, 0x6ab58cu, (uint32_t)(ya))); sf(vb + (o) + 8, z); \
            W32(vb + (o) + 12, 0x3f800000u); W32(vb + (o) + 16, col); sf(vb + (o) + 20, u); sf(vb + (o) + 24, v); } while (0)
        VTX(0x00, X, Y, a2 << 24 | rgb, G0p, G1p);
        VTX(0x1c, (uint32_t)X - i1, (uint32_t)Y - i2, rgb, um, vm);
        VTX(0x38, (uint32_t)X + j1, (uint32_t)Y - j2, rgb, up, vm);
        VTX(0x54, (uint32_t)X + i1, (uint32_t)Y - i3, rgb, up, vp);
        VTX(0x70, (uint32_t)X - j1, (uint32_t)Y - j3, rgb, um, vp);
        #undef VTX
        memmove(mb + vb + 0x8c, mb + vb + 0x1c, 0x1c);
        c->esp = S - 64; W32(S - 64, (uint32_t)Z);   /* [esp+0x64] = depth key */
        gcall(c, &fs, F_005f5ad0, 0x47a818u);
    next:
        #undef PV
        i++; P += 0x60;
    } while (i < (int16_t)R16(0x7143f74u));
    W32(S + 0x4c, (uint32_t)i);
    c->esp = S; c->ebp = ebp; c->ebx = P + 0x48;
    fsw_out(c, &fs);
    return 1;
}
