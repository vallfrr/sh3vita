/* Native particle grid kernel F_00478ac0 (called once per particle by the particle update F_0047abb0): deposits the particle
 * into the 8x8x8 density grid [0xbccfa0] and moves it down the gradient of the previous grid [0xbcc7a0].
 *
 * Numeric contract: x87 with 24-bit precision control (PC, Direct3D 8 without FPU_PRESERVE), i.e. binary32 operations in the
 * original order (see shadow_native.c). floor() + __ftol of the CRT become truncation of floorf() to int64, low 32 bits;
 * "value - (float)int" is computed exactly in double then rounded once, like x87 fsub of an exactly loaded integer.
 * Verified against the original bytes under Unicorn (PC24): tools/particle_native_proof.py.
 * Guest-visible effects reproduced: the eight grid cells written, particle fields +0x10..+0x1c, esp after return,
 * callee-saved registers. Not reproduced: eax/ecx/edx (the only caller, F_0047abb0, calls F_00475fb0 next, which
 * overwrites them before reading) and dead stack scratch. No x87 compare in this function. */
#include <stdint.h>
#include <string.h>
#include <math.h>
#include "x86rt.h"

int g_particle_native;

static inline float gf(uint32_t a) { float f; memcpy(&f, g_mem + a, 4); return f; }
static inline void sf(uint32_t a, float f) { memcpy(g_mem + a, &f, 4); }
/* floor + __ftol: integer indefinite (0x8000000000000000) for NaN or out of range; callers keep the low 32 bits. */
static inline int32_t floor_int(float v) {
    float f = floorf(v);
    int64_t t = (f == f && f >= -9223372036854775808.0f && f < 9223372036854775808.0f) ? (int64_t)f : INT64_MIN;
    return (int32_t)(uint32_t)t;
}
static inline float sub_int(float a, int32_t i) { return (float)((double)a - (double)i); }
#define CELL(z, y, x) ((((uint32_t)(z) << 3 | (uint32_t)(y)) << 3) | (uint32_t)(x))

int particle_grid_native(CPU *c) {
    uint8_t *const mb = g_mem;
    float *p = (float *)(mb + R32(c->esp + 4));
    float *g0 = (float *)(mb + 0xbcc7a0u), *g1 = (float *)(mb + 0xbccfa0u);
    const float k = gf(0x6b8290u), k2 = gf(0x6b8298u), k3 = gf(0x6b8254u), one = gf(0x68a754u);
    float x = k * p[0], y = k * p[1], z = k * p[2];
    uint8_t sh = R8(0x7143f82u);
    if (sh) y = (float)(int32_t)(1u << (sh & 31)) * y;
    int32_t ux = floor_int(x), uy = floor_int(y), uz = floor_int(z);
    float fx = sub_int(x, ux), fy = sub_int(y, uy), fz = sub_int(z, uz);
    float wx = one - fx, wy = one - fy, wz = one - fz;
    uint32_t x0 = ux & 7, x1 = (x0 + 1) & 7, y0 = uy & 7, y1 = (y0 + 1) & 7, z0 = uz & 7, z1 = (z0 + 1) & 7;
    float *q;
    q = &g1[CELL(z0, y0, x0)]; *q = ((wz + *q) + wy) + wx;
    q = &g1[CELL(z0, y0, x1)]; *q = ((wz + *q) + wy) + fx;
    q = &g1[CELL(z0, y1, x0)]; *q = ((wz + *q) + wx) + fy;
    q = &g1[CELL(z0, y1, x1)]; *q = ((wz + *q) + fy) + fx;
    q = &g1[CELL(z1, y0, x0)]; *q = ((wy + *q) + wx) + fz;
    q = &g1[CELL(z1, y0, x1)]; *q = ((wy + *q) + fz) + fx;
    q = &g1[CELL(z1, y1, x0)]; *q = ((wx + *q) + fz) + fy;
    q = &g1[CELL(z1, y1, x1)]; *q = ((fz + *q) + fy) + fx;
    /* Gradient of the previous grid at the position advanced by the velocity p[8..10]. */
    float sx = x + p[8], sy = y + p[9], sz = z + p[10];
    int32_t vx = floor_int(sx), vy = floor_int(sy), vz = floor_int(sz);
    float gx = sub_int(sx, vx), gy = sub_int(sy, vy), gz = sub_int(sz, vz);
    uint32_t xa = vx & 7, xp = (xa + 1) & 7, xm = (xa - 1) & 7, xpp = (xp + 1) & 7;
    uint32_t ya = vy & 7, yp = (ya + 1) & 7, ym = (ya - 1) & 7, ypp = (yp + 1) & 7;
    uint32_t za = vz & 7, zp = (za + 1) & 7, zm = (za - 1) & 7, zpp = (zp + 1) & 7;
    #define G(z_, y_, x_) g0[CELL(z_, y_, x_)]
    float s8 = G(za, yp, xp) + G(zp, yp, xp);
    float p1 = G(zp, ya, xp), p2 = G(za, ya, xp), p3 = G(zp, yp, xa), p4 = G(za, yp, xa), p5 = G(zp, ya, xa), p6 = G(za, ya, xa);
    float dy = ((((((G(za, ypp, xp) + G(zp, ypp, xp)) + G(za, ypp, xa)) + G(zp, ypp, xa)) - p6) - p2) - p5) - p1;
    float uy_ = (((((s8 + p4) + p3) - G(za, ym, xa)) - G(za, ym, xp)) - G(zp, ym, xa)) - G(zp, ym, xp);
    float ry = (dy * (one - gy) + uy_ * gy) * k2;
    float dz = ((((((G(zpp, ya, xp) + G(zpp, yp, xp)) + G(zpp, ya, xa)) + G(zpp, yp, xa)) - p6) - p4) - p2) - G(za, yp, xp);
    float uz_ = (((((((p5 + G(zp, yp, xp)) + p1) + p3) - G(zm, ya, xa)) - G(zm, yp, xa)) - G(zm, ya, xp)) - G(zm, yp, xp));
    float rz = (dz * (one - gz) + uz_ * gz) * k2;
    float dx = ((((((G(zp, yp, xpp) + G(zp, ya, xpp)) + G(za, yp, xpp)) + G(za, ya, xpp)) - p6) - p4) - p5) - p3;
    float ux_ = (((((s8 + p1) + p2) - G(za, ya, xm)) - G(za, yp, xm)) - G(zp, ya, xm)) - G(zp, yp, xm);
    #undef G
    p[4] = p[4] - k2 * (dx * (one - gx) + ux_ * gx);
    p[5] = p[5] - ry;
    p[6] = p[6] - rz;
    p[4] = k3 * p[4]; p[5] = k3 * p[5]; p[6] = k3 * p[6];
    c->esp += 4;
    return 1;
}

/* Native F_00475fb0 (per-particle damped random drift of the field at +0x50/+0x54, called by F_0047abb0) with its random
 * source F_0060bfa0(a, b) and the CRT rand F_0066405e inline: rand = (s = s * 0x343fd + 0x269ec3) >> 16 & 0x7fff on
 * the per-thread seed at _getptd()+0x14 (thread data from the port's TLS slot, diag_rand_thread_data; absent: the
 * translated function runs). Same rand call count and order as the original (3 or 6 calls).
 * Numeric contract: PC24 = binary32 in the original order; the 31-bit integer times 2^-31 ([0x690610]) is exact in double.
 * Verified against the original bytes under Unicorn: tools/particle_native_proof.py (mode rand).
 * Guest-visible effects reproduced: particle +0x50/+0x54, the rand seed, esp, callee-saved registers, x87 C0/C2/C3.
 * Not reproduced: eax/ecx/edx (dead in the caller F_0047abb0) and the stale stack words of the inner calls. */
typedef struct { uint32_t bits; int used; } PFsw;
static inline void pcmp(PFsw *s, float a, float b) {
    s->used = 1; s->bits = a < b ? FSW_C0 : a > b ? 0 : a == b ? FSW_C3 : FSW_C0 | FSW_C2 | FSW_C3;
}
uint32_t diag_rand_thread_data(CPU *c);
extern uint32_t g_mem_size;
static inline uint32_t crt_rand(uint32_t ptd) {
    uint8_t *const mb = g_mem;
    uint32_t s = R32(ptd + 0x14) * 0x343fdu + 0x269ec3u;
    W32(ptd + 0x14, s);
    return (s >> 16) & 0x7fffu;
}
int particle_rand_native(CPU *c) {
    uint8_t *const mb = g_mem;
    const uint32_t ptd = diag_rand_thread_data(c);
    if (!ptd || ptd > g_mem_size - 0x88) return 0;
    const uint32_t p = R32(c->esp + 4);
    const float zero = gf(0x68a868u), one = gf(0x68a754u), M = gf(0x6b8260u);
    PFsw fs = {0, 0};
    float s = (float)(int16_t)R16(0x7143f7eu), y = gf(p + 4);
    const float H0 = gf(0x7143f30u), H1 = gf(0x7143f34u), H2 = gf(0x7143f38u);
    pcmp(&fs, y, H0);
    if (y < H0) { pcmp(&fs, y, H1); s = (y < H1) ? 0.0f : ((y - H1) / (H0 - H1)) * s; }
    pcmp(&fs, y, H2);
    if (y > H2) s = M;
    /* F_0060bfa0(a = [0x6b8268], b = 0.02f) */
    float a = gf(0x6b8268u), b; { uint32_t bb = 0x3ca3d70au; memcpy(&b, &bb, 4); }
    pcmp(&fs, a, zero);
    if (a < zero) { uint32_t r1 = crt_rand(ptd); crt_rand(ptd); crt_rand(ptd); if (r1 & 0x1000u) a = -a; }
    uint32_t r2 = crt_rand(ptd), r3 = crt_rand(ptd);
    float tmp = (b + one) * b;
    uint32_t r4 = crt_rand(ptd), v = ((((r4 & 1u) << 15) | r3) << 15) | r2;
    float t = (float)((double)(int32_t)v * (double)gf(0x690610u));
    t = t + b; t = tmp / t; t = t - b;
    float r = t * a;
    float A = r + gf(p + 0x54), Bv = (gf(p + 0x50) - s) * gf(0x68ee94u), V = (A - Bv) * gf(0x68eff0u);
    float q = V + gf(p + 0x50);
    sf(p + 0x54, V); sf(p + 0x50, q);
    pcmp(&fs, q, zero);
    if (q < zero) W32(p + 0x50, 0);
    q = gf(p + 0x50);
    pcmp(&fs, q, M);
    if (q > M) W32(p + 0x50, R32(0x6b8260u));
    if (fs.used) c->fsw = (c->fsw & ~(FSW_C0 | FSW_C2 | FSW_C3)) | fs.bits;
    c->esp += 4;
    return 1;
}

/* Native F_00478850 (per-particle collision against the [0x7143f76] planes at 0xbcd7a0, 0x40 bytes each: normal +0,
 * point +0x10, polygon data +0x20/+0x30), called by F_0047abb0 instead of the plain velocity step when planes exist.
 * The polygon test F_0060c0a0 (reached only within [0x6b824c] + [0x7143f68] of a plane) stays a guest call with the
 * original stack frame (projected point at frame +0x30, particle word 3 at +0x3c). PC24 = binary32 in the x87 order.
 * Verified against the original bytes under Unicorn: tools/particle_native_proof.py (mode planes).
 * Guest-visible effects reproduced: particle +0..+0x1c and +0x58, the frame words passed to F_0060c0a0, esp,
 * callee-saved registers, x87 C0/C2/C3. Not reproduced: eax/ecx/edx (reloaded by the caller) and dead locals. */
void F_0060c0a0(CPU *c);
int particle_planes_native(CPU *c) {
    uint8_t *const mb = g_mem;
    const uint32_t E = c->esp, p = R32(E + 4), A = ((E - 4) & ~0xfu) - 0x34, S = A - 0xc;
    const float R = gf(0x7143f68u), c24c = gf(0x6b824cu), zero = gf(0x68a868u);
    PFsw fs = {0, 0};
    uint32_t hits = 0;
    int32_t count = (int16_t)R16(0x7143f76u);
    if (count > 0) {
        const float T = c24c + R;
        for (uint32_t pl = 0xbcd7a0u; count > 0; count--, pl += 0x40) {
            const float N0 = gf(pl), N1 = gf(pl + 4), N2 = gf(pl + 8);
            float d = ((gf(p + 8) - gf(pl + 0x18)) * N2 + (gf(p) - gf(pl + 0x10)) * N0) + (gf(p + 4) - gf(pl + 0x14)) * N1;
            float ad = fabsf(d);
            pcmp(&fs, ad, T);
            if (!(ad < T)) continue;
            {   /* F_0060c0a0(plane + 0x20, plane + 0x30, frame + 0x30) */
                sf(S + 0x30, gf(p) - d * N0); sf(S + 0x34, gf(p + 4) - d * N1); sf(S + 0x38, gf(p + 8) - d * N2);
                W32(S + 0x3c, R32(p + 0xc));
                W32(S - 4, S + 0x30); W32(S - 8, pl + 0x30); W32(S - 12, pl + 0x20); W32(S - 16, 0x478929u);
                const uint32_t ebx = c->ebx, ebp = c->ebp, esi = c->esi, edi = c->edi;
                c->esp = S - 16; c->ebx = hits; c->esi = p; c->edi = pl + 4;
                if (fs.used) { c->fsw = (c->fsw & ~(FSW_C0 | FSW_C2 | FSW_C3)) | fs.bits; fs.used = 0; }
                F_0060c0a0(c);
                const uint32_t r = c->eax;
                c->ebx = ebx; c->ebp = ebp; c->esi = esi; c->edi = edi;
                if (r) continue;
            }
            pcmp(&fs, d, zero);
            if (d < zero || d == zero) hits += 100;
            float v = (gf(p + 0x18) * N2 + gf(p + 0x14) * N1) + N0 * gf(p + 0x10);
            pcmp(&fs, ad, c24c);
            if (ad < c24c) {
                hits++;
                float k = c24c - ad, k1 = k * N1, k2 = k * N2;
                sf(p, k * N0 + gf(p)); sf(p + 4, k1 + gf(p + 4)); sf(p + 8, k2 + gf(p + 8));
            } else v = v * ((T - ad) / R);
            float u0 = gf(p + 0x10) - v * N0, u1 = gf(p + 0x14) - v * N1, u2 = gf(p + 0x18) - v * N2;
            float L = sqrtf((u2 * u2 + u1 * u1) + u0 * u0);
            pcmp(&fs, L, zero);
            if (L == zero) continue;
            float q4 = gf(p + 0x10), q5 = gf(p + 0x14), q6 = gf(p + 0x18);
            float f = sqrtf((q4 * q4 + q5 * q5) + q6 * q6) / L;
            sf(p + 0x10, u0 * f); sf(p + 0x14, u1 * f); sf(p + 0x18, u2 * f);
        }
    }
    const float s = gf(0x7143f3cu);
    float v4 = s * gf(p + 0x10), v5 = s * gf(p + 0x14), v6 = s * gf(p + 0x18);
    W32(p + 0x58, hits);
    sf(p, v4 + gf(p)); sf(p + 4, v5 + gf(p + 4)); sf(p + 8, v6 + gf(p + 8));
    if (fs.used) c->fsw = (c->fsw & ~(FSW_C0 | FSW_C2 | FSW_C3)) | fs.bits;
    c->esp = E + 4;
    return 1;
}

/* Native F_00476090: wraps the particle position into the box of half-size H = [0x7143f58] centred on (cx, cy, cz) =
 * [0x7143ed0..ed8] (crossing x mirrors z and crossing z mirrors x), with a y rule depending on the shift
 * [0x7143f82] (0: box, 1: half box, n: 2H / 2^n), then fades the field +0x50 into +0x48 near the box faces
 * (300 = [0x68ebc0], 1/300 = [0x690604]). PC24 = binary32 in the x87 order (the stack juggling of the original is
 * followed value by value). Verified against the original bytes under Unicorn: tools/particle_native_proof.py
 * (mode box). Guest-visible effects reproduced: particle +0..+8, +0x48, +0x50, the argument slot reused for y, esp,
 * callee-saved registers, x87 C0/C2/C3. Not reproduced: eax/ecx/edx (reloaded by the caller) and dead locals. */
int particle_box_native(CPU *c) {
    uint8_t *const mb = g_mem;
    const uint32_t E = c->esp, p = R32(E + 4);
    const float H = gf(0x7143f58u), cx = gf(0x7143ed0u), cy = gf(0x7143ed4u), cz = gf(0x7143ed8u);
    const float C = gf(0x68ebc0u), k = gf(0x690604u), zero = gf(0x68a868u);
    PFsw fs = {0, 0};
    float X = gf(p) - cx, Y = gf(p + 4) - cy, Z = gf(p + 8) - cz, nH = -H;
    pcmp(&fs, X, nH);
    if (X < nH) { X = X + (H + H); Z = -Z; }
    pcmp(&fs, X, H);
    if (X > H) { X = X - (H + H); Z = -Z; }
    pcmp(&fs, Z, nH);
    if (Z < nH) { Z = Z + (H + H); X = -X; }
    pcmp(&fs, Z, H);
    if (Z > H) { Z = Z - (H + H); X = -X; }
    const uint8_t sh = R8(0x7143f82u);
    int tail = 1;
    float Yout;   /* the argument slot keeps Y; the early exits of shift 0 and 1 only adjust the stored position */
    if (sh == 0) {
        pcmp(&fs, Y, nH);
        if (Y < nH) Y = (H + H) + Y;
        pcmp(&fs, Y, H);
        if (Y > H) { W32(p + 0x50, 0); W32(p + 0x48, 0); Yout = Y - (H + H); tail = 0; }
    } else if (sh == 1) {
        pcmp(&fs, Y, nH);
        if (Y < nH) Y = H + Y;
        pcmp(&fs, Y, zero);
        if (Y > zero) { W32(p + 0x50, 0); W32(p + 0x48, 0); Yout = Y - H; tail = 0; }
    } else {
        const float K = (float)(int32_t)(1u << (sh & 31));
        float t = (H * gf(0x68f044u)) / K;
        pcmp(&fs, t, Y);
        if (t > Y) Y = (H + H) / K + Y;
        pcmp(&fs, Y, zero);
        if (Y > zero) { W32(p + 0x50, 0); W32(p + 0x48, 0); Y = Y - (H + H) / K; Yout = Y; tail = 0; }
    }
    if (tail) {   /* 0x476254: fade near the faces */
        Yout = Y;
        float A = gf(p + 0x50);
        const float W = C - H, V = H - C;
        pcmp(&fs, X, W);
        if (X < W) A = (A * (H + X)) * k;
        pcmp(&fs, X, V);
        if (X > V) A = (A * (H - X)) * k;
        if (sh == 0) {
            pcmp(&fs, Y, W);
            if (Y < W) A = (A * (H + Y)) * k;
        } else {
            const float q = (H + H) / (float)(int32_t)(1u << (sh & 31)), r = C - q;
            pcmp(&fs, r, Y);
            if (r > Y) A = (A * (q + Y)) * k;
        }
        pcmp(&fs, Y, V);
        if (Y > V) A = (A * (H - Y)) * k;
        pcmp(&fs, Z, W);
        if (Z < W) A = (A * (H + Z)) * k;
        pcmp(&fs, Z, V);
        if (Z > V) A = (A * (H - Z)) * k;
        sf(p + 0x48, A);
    }
    sf(E + 4, Y);
    sf(p, cx + X); sf(p + 4, cy + Yout); sf(p + 8, cz + Z);
    if (fs.used) c->fsw = (c->fsw & ~(FSW_C0 | FSW_C2 | FSW_C3)) | fs.bits;
    c->esp = E + 4;
    return 1;
}

/* Native F_00479340 (particle against the [0x7143f7c] vertical cylinders at 0xbce720, 0x2c bytes each: centre x/y/z,
 * radius +0xc, top +0x1c, gain +0x24, layer mask +0x28 tested with 1 << particle[+0x4c]); pushes the particle out
 * radially and counts contacts in +0x58. Called by F_0047abb0 in the same unit (dispatcher in the f040 copy,
 * tools/particle_unit.py). PC24 = binary32 in the x87 order. Verified against the original bytes under Unicorn:
 * tools/particle_native_proof.py (mode cyl). Guest-visible effects reproduced: particle +0x10..+0x1c and +0x58, esp,
 * callee-saved registers, x87 C0/C2/C3. Not reproduced: eax/ecx/edx (reloaded by the caller) and dead locals. */
int particle_cyl_native(CPU *c) {
    uint8_t *const mb = g_mem;
    const uint32_t E = c->esp, p = R32(E + 4);
    const float c248 = gf(0x6b8248u), zero = gf(0x68a868u), one = gf(0x68a754u), c250 = gf(0x6b8250u);
    const float yv = c248 + gf(p + 4);
    const uint32_t bit = 1u << (R32(p + 0x4c) & 31);
    uint32_t hits = R32(p + 0x58);
    PFsw fs = {0, 0};
    for (int32_t n = (int16_t)R16(0x7143f7cu), r = 0xbce720; n > 0; n--, r += 0x2c) {
        if (!(R32(r + 0x28) & bit)) continue;
        pcmp(&fs, yv, gf(r + 4));
        if (yv < gf(r + 4)) continue;
        float dx = gf(p) - gf(r), dz = gf(p + 8) - gf(r + 8);
        float D = dz * dz + dx * dx;
        float a = c248 + gf(r + 0xc), b = a + gf(r + 0x1c);
        float bb = b * b;
        pcmp(&fs, D, bb);
        if (!(D < bb)) continue;
        float L = sqrtf(D);
        hits++;
        float inv = one / L, ux = dx * inv, uy = zero * inv, uz = inv * dz;
        pcmp(&fs, L, a);
        float s = (L < a) ? one : (b - L) * gf(r + 0x24);
        float e0 = gf(p + 0x10) - gf(r + 0x10), e2 = gf(p + 0x18) - gf(r + 0x18);
        float m = s * (((e2 * uz) + (e0 * ux)) + uy * zero);
        pcmp(&fs, m, zero);
        if (!(m < zero || m != m)) m = m * c250;
        float my = m * uy, mz = uz * m;
        sf(p + 0x10, gf(p + 0x10) - ux * m);
        sf(p + 0x14, gf(p + 0x14) - my);
        sf(p + 0x18, gf(p + 0x18) - mz);
    }
    W32(p + 0x58, hits);
    if (fs.used) c->fsw = (c->fsw & ~(FSW_C0 | FSW_C2 | FSW_C3)) | fs.bits;
    c->esp = E + 4;
    return 1;
}

/* Native body of the particle update F_0047abb0. Every callee stays a call with its original argument (so the native
 * kernels behind their own switches apply: grid F_00478ac0 and cylinders F_00479340 through the f040 copy, planes
 * F_00478850, randomizer F_00475fb0 and box F_00476090 through --wrap); F_0060d8e0 (fld [0x70e67ac]) is inlined.
 * Native here: the far-camera test, the grid copies, the per-particle bounds / lifetime / velocity step, the minimum
 * search over the 512 density cells and the new field centre, the per-particle cell code (3 x __ftol, clamped 0..3).
 * PC24 = binary32 in the x87 order. Verified against the original bytes under Unicorn with every callee replaced by
 * the same logging stub on both sides: tools/particle_native_proof.py (mode update).
 * Guest-visible effects reproduced: particles, globals [0x7143f3c], [0x7143f10..f1c], the grids 0xbcc7a0 / 0xbccfa0 /
 * 0x713ea40, the plane copy 0xbcd7a0, the records at 0x7142150, the calls (order, arguments, x87 C0/C2/C3 at entry),
 * esp, callee-saved registers. Not reproduced: eax/ecx/edx at return (void, cdecl) and dead locals. */
void F_00479a20(CPU *c); void F_004cc330(CPU *c); void F_00479d80(CPU *c); void F_00479dd0(CPU *c); void F_004786e0(CPU *c);
void F_00678c00(CPU *c); void F_00678c10(CPU *c); void F_00477de0(CPU *c); void F_00478850(CPU *c); void F_00478ac0(CPU *c);
void F_00475fb0(CPU *c); void F_00478420(CPU *c); void F_004791d0(CPU *c); void F_00479340(CPU *c); void F_00476090(CPU *c);
static inline int32_t u_ftol(float v) {
    int64_t t = (v == v && v >= -9223372036854775808.0f && v < 9223372036854775808.0f) ? (int64_t)v : INT64_MIN;
    return (int32_t)(uint32_t)t;
}
/* Guest call with the stack pointer at S before the pushes and up to two cdecl arguments (a1 = [esp+4]). */
static void ucall(CPU *c, PFsw *fs, gfn f, uint32_t S, uint32_t ret, int n, uint32_t a1, uint32_t a2) {
    uint8_t *const mb = g_mem;
    if (fs->used) { c->fsw = (c->fsw & ~(FSW_C0 | FSW_C2 | FSW_C3)) | fs->bits; fs->used = 0; }
    uint32_t sp = S;
    if (n > 1) { sp -= 4; W32(sp, a2); }
    if (n > 0) { sp -= 4; W32(sp, a1); }
    sp -= 4; W32(sp, ret);
    const uint32_t ebx = c->ebx, ebp = c->ebp, esi = c->esi, edi = c->edi;
    c->esp = sp; f(c);
    c->ebx = ebx; c->ebp = ebp; c->esi = esi; c->edi = edi; c->esp = S;
}
int particle_update_native(CPU *c) {
    uint8_t *const mb = g_mem;
    const uint32_t E = c->esp, S = (((E - 4) & ~0xfu) - 0x24) - 0xc;
    const uint32_t ebx0 = c->ebx, ebp0 = c->ebp, esi0 = c->esi, edi0 = c->edi;
    PFsw fs = {0, 0};
    c->ebp = E - 4;
    ucall(c, &fs, F_00479a20, S, 0x47abc1u, 0, 0, 0);
    ucall(c, &fs, F_004cc330, S, 0x47abc6u, 0, 0, 0);
    if ((int16_t)R16(0x7143f74u) > 0) {
        sf(0x7143f3cu, gf(0x70e67acu) * gf(0x690654u));
        if (!(R32(0x7143f88u) & 0x400u)) {
            const float zero = gf(0x68a868u), one = gf(0x68a754u);
            {   float dx = gf(0x7143ec0u) - gf(0x712e3c0u), dz = gf(0x7143ec8u) - gf(0x712e3c8u);
                float r = sqrtf(dz * dz + dx * dx), lim = gf(0x690650u);
                pcmp(&fs, r, lim);
                if (r > lim) { ucall(c, &fs, F_00479d80, S, 0x47ac2eu, 0, 0, 0); ucall(c, &fs, F_00479dd0, S, 0x47ac33u, 0, 0, 0); } }
            const float f2c = gf(0x7143f2cu);
            pcmp(&fs, f2c, zero);
            memmove(mb + 0xbcc7a0u, mb + 0x713ea40u, 0x800); memset(mb + 0xbccfa0u, 0, 0x800);
            if (f2c != zero) ucall(c, &fs, F_004786e0, S, 0x47ac6au, 0, 0, 0);
            int16_t np = (int16_t)R16(0x7143f76u);
            if (np > 0) {
                ucall(c, &fs, F_00678c00, S, 0x47ac8bu, 2, 0x713f240u, 0x713f240u + (uint32_t)(int32_t)np * 64);
                np = (int16_t)R16(0x7143f76u);
                if (np > 0) memmove(mb + 0xbcd7a0u, mb + 0x713f240u, (uint32_t)((int32_t)np << 6));
            }
            const int32_t planes = np;
            const float lo = gf(0x69064cu), hi = gf(0x690648u), c75 = gf(0x68f11cu), m2 = gf(0x68f044u);
            for (int32_t n = (int16_t)R16(0x7143f74u), k = 0; k < n; k++) {
                const uint32_t P = 0x712e3c0u + 0x60u * (uint32_t)k;
                int out = 0;
                for (int i = 0; i < 3 && !out; i++) {
                    float v = gf(P + 4 * i);
                    pcmp(&fs, v, lo); if (v < lo) { out = 1; break; }
                    pcmp(&fs, v, hi); if (v > hi) out = 1;
                }
                if (out) ucall(c, &fs, F_00477de0, S, 0x47ad38u, 1, P, 0);
                if (R32(P + 0x5c)) {
                    float l = gf(P + 0x50) * c75;
                    sf(P + 0x50, l);
                    pcmp(&fs, l, one);
                    if (l < one || l == one) ucall(c, &fs, F_00477de0, S, 0x47ad61u, 1, P, 0);
                } else if ((int32_t)R32(P + 0x58) > 0) W32(P + 0x5c, 1);
                if (planes) ucall(c, &fs, F_00478850, S, 0x47ad82u, 1, P, 0);
                else {
                    const float s = gf(0x7143f3cu);
                    float v0 = s * gf(P + 0x10), v1 = s * gf(P + 0x14), v2 = s * gf(P + 0x18), v3 = s * gf(P + 0x1c);
                    W32(P + 0x58, 0);
                    sf(P, v0 + gf(P)); sf(P + 4, v1 + gf(P + 4)); sf(P + 8, v2 + gf(P + 8)); sf(P + 0xc, v3 + gf(P + 0xc));
                }
                sf(P + 0x10, gf(0x7143f00u) + gf(P + 0x10)); sf(P + 0x14, gf(0x7143f04u) + gf(P + 0x14)); sf(P + 0x18, gf(0x7143f08u) + gf(P + 0x18));
                /* push edi; call F_00478ac0; push edi; call F_00475fb0; add esp, 8 */
                W32(S - 4, P);
                ucall(c, &fs, F_00478ac0, S - 4, 0x47ae07u, 0, 0, 0);
                W32(S - 8, P);
                ucall(c, &fs, F_00475fb0, S - 8, 0x47ae0du, 0, 0, 0);
                c->esp = S;
                float y = gf(P + 0x14);
                pcmp(&fs, y, m2);
                if (y < m2) W32(P + 0x5c, 1);
            }
            ucall(c, &fs, F_00678c10, S, 0x47ae42u, 2, 0x713ea40u, 0x713f240u);
            memmove(mb + 0x713ea40u, mb + 0xbccfa0u, 0x800);
            float m = gf(0x690644u); uint32_t best = 0;
            for (uint32_t i = 0; i < 0x200; i++) { float v = gf(0xbccfa0u + 4 * i); pcmp(&fs, v, m); if (v < m) { m = v; best = i; } }
            int32_t ix = (int32_t)best >> 6; if (ix >= 4) ix -= 8;
            int32_t iy = ((int32_t)best >> 3) & 7; if (iy >= 4) iy -= 8;
            int32_t iz = (int32_t)best & 7; if (iz >= 4) iz -= 8;
            const float q = gf(0x7143f58u) * gf(0x68f030u);
            const float fx = (float)ix * q, fy = (float)iy * q, fz = (float)iz * q;
            W32(0x7143f1cu, R32(0x7143edcu));
            sf(0x7143f10u, gf(0x7143ed0u) + fx); sf(0x7143f14u, gf(0x7143ed4u) + fy); sf(0x7143f18u, gf(0x7143ed8u) + fz);
            ucall(c, &fs, F_00478420, S, 0x47aff5u, 0, 0, 0);
            int32_t cyl = (int16_t)R16(0x7143f7cu), f7a = (int16_t)R16(0x7143f7au);
            if ((int8_t)R8(0x7143f88u) < 0) { cyl = 0; f7a = 0; }
            const float c294 = gf(0x6b8294u), h = gf(0x69004cu);
            for (int32_t n = (int16_t)R16(0x7143f74u), k = 0; k < n; k++) {
                const uint32_t P = 0x712e3c0u + 0x60u * (uint32_t)k;
                float x = gf(P) - gf(0x7143ed0u), y = gf(P + 4) - gf(0x7143ed4u), z = gf(P + 8) - gf(0x7143ed8u);
                float X = c294 * x, Y = c294 * y, Z = c294 * z;
                int32_t a = u_ftol(X + h), b = u_ftol(Y + h), d = u_ftol(Z + h);
                a = a < 0 ? 0 : a > 3 ? 3 : a; b = b < 0 ? 0 : b > 3 ? 3 : b; d = d < 0 ? 0 : d > 3 ? 3 : d;
                W32(P + 0x4c, (((uint32_t)d * 4 | (uint32_t)b) << 2) | (uint32_t)a);
                if (f7a) ucall(c, &fs, F_004791d0, S, 0x47b0fau, 1, P, 0);
                if (cyl) ucall(c, &fs, F_00479340, S, 0x47b10bu, 1, P, 0);
                ucall(c, &fs, F_00476090, S, 0x47b114u, 1, P, 0);
            }
            for (int32_t n = (int16_t)R16(0x7143f7au), k = 0; k < n; k++) {
                const uint32_t r = 0x7142150u + 0x2cu * (uint32_t)k;
                W32(r + 8, 0); W32(r + 4, 0); W32(r, 0);
            }
        }
    }
    if (fs.used) c->fsw = (c->fsw & ~(FSW_C0 | FSW_C2 | FSW_C3)) | fs.bits;
    c->ebx = ebx0; c->ebp = ebp0; c->esi = esi0; c->edi = edi0;
    c->esp = E + 4;
    return 1;
}
