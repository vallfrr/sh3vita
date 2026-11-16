/* Native CPU shadow-volume builder F_0048e140 (types 2/3 of F_004906a0), with its callees: D3DXVec3Transform (x87 0x6483a7),
 * D3DXMatrixMultiply (0x648639), D3DXVec3Normalize (0x64856f), near-plane clip F_0048c290, side emitter F_0048c4e0 and cap
 * emitter F_0048c5b0.
 *
 * Numeric contract: the PC game runs these x87 sequences with the precision control set to 24 bits (native Direct3D 8
 * CreateDevice without FPU_PRESERVE, docs/HANDOFF-RUNTIME.md "Preuve numérique PC24"). Each x87 add/sub/mul/div/sqrt then
 * rounds to a 24-bit significand, which equals IEEE binary32 arithmetic in the same order while results stay inside the
 * binary32 exponent range (no overflow, no subnormal). Inputs here are int16 mesh coordinates, view-space floats and game
 * constants. Every operation below is written in the original order and association; constants are read from guest memory.
 * Verified against the original x86 bytes run under Unicorn with FCW=PC24: tools/shadow_native_proof.py.
 * The translated port computes these in double (53 bits), so its output differs from the PC in the last bits; this version
 * follows the PC.
 *
 * Guest-visible effects reproduced: output triangle buffers and counters ([0xb5445c]/[0xb544a0], [0xb54460]/[0xb544a8]),
 * x87 condition codes of the last compare, callee-saved registers, esp after return, the frame arrays of the original
 * (vertex slots, clip outputs, flags, view matrix, combined matrix, transformed light) written back to the guest stack.
 * Not reproduced: eax/ecx/edx at return and other dead scratch below esp (the only caller F_004906a0 returns to
 * F_00490810, which overwrites eax/ecx/edx before reading them).
 * Preconditions checked on every call, else the translated function runs: the lazy D3DX table is initialised and points
 * at the x87 implementations listed above. Off unless g_shadow_native (marker "shadownative" = 1 or ab). */
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
#include "x86rt.h"

int g_shadow_native;
int g_shadow_strip; unsigned g_ss_native;   /* strip volumes F_0048faa0: switch and native calls per window */
#include <time.h>
unsigned long long g_sn_part_ns[3];   /* diagnostic: GetTransform call, setup, vertex loop (logged per A/B window) */
#ifdef SH3_SHADOW_PARTS
static inline unsigned long long sn_now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (unsigned long long)t.tv_sec * 1000000000ull + t.tv_nsec; }
#else
#define sn_now() 0ull
#endif

typedef struct { uint32_t c_bits; int used; } Fsw;
/* fcom/fcomp: C0 a<b, C3 a==b, none a>b, all three unordered (same as rt_fcom). */
static inline void fcmp(Fsw *s, float a, float b) {
    s->used = 1;
    s->c_bits = a < b ? FSW_C0 : a > b ? 0 : a == b ? FSW_C3 : FSW_C0 | FSW_C2 | FSW_C3;
}
static inline float gf(uint32_t a) { float f; memcpy(&f, g_mem + a, 4); return f; }
static inline void sf(uint32_t a, float f) { memcpy(g_mem + a, &f, 4); }

/* D3DXVec3Transform x87: out[j] = ((v0*m[j] + v1*m[4+j]) + v2*m[8+j]) + m[12+j]; inputs read before outputs are written. */
static inline void vec3_transform(float *o, const float *v, const float *m) {
    float r[4];
    for (int j = 0; j < 4; j++) r[j] = ((v[0] * m[j] + v[1] * m[4 + j]) + v[2] * m[8 + j]) + m[12 + j];
    memcpy(o, r, 16);
}
/* D3DXMatrixMultiply x87: o[i][j] = (a[i][0]*b[0][j] + a[i][2]*b[2][j]) + (a[i][1]*b[1][j] + a[i][3]*b[3][j]). */
static void mat_mul(float *o, const float *a, const float *b) {
    float r[16];
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 4; j++)
            r[4 * i + j] = (a[4 * i] * b[j] + a[4 * i + 2] * b[8 + j]) + (a[4 * i + 1] * b[4 + j] + a[4 * i + 3] * b[12 + j]);
    memcpy(o, r, 64);
}
/* D3DXVec3Normalize x87 (table reciprocal square root). */
static void vec3_normalize(float *o, const float *v) {
    float s = (v[0] * v[0] + v[1] * v[1]) + v[2] * v[2];
    uint32_t sb; memcpy(&sb, &s, 4);
    if (!sb) { memset(o, 0, 12); return; }
    float d = s - 1.0f; uint32_t db; memcpy(&db, &d, 4);
    if ((int32_t)(db & 0x7fffffffu) <= 0x3727c5ac) { if (o != v) memmove(o, v, 12); return; }
    uint32_t idx = (sb >> 12) & 0xff8, mbits = (sb & 0xffffffu) | 0x3f000000u, ebits = ((0xbeffffffu - sb) >> 1) & 0xff800000u;
    float m, e; memcpy(&m, &mbits, 4); memcpy(&e, &ebits, 4);
    float r = (m * gf(0x726820u + idx) + gf(0x726824u + idx)) * e;
    float x = v[0] * r, y = v[1] * r, z = v[2] * r;
    o[0] = x; o[1] = y; o[2] = z;
}

/* F_0048c290(out, p, q): near-plane clip of the segment light p -> vertex q. Returns 0, 1 or 2. */
static int clip_segment(Fsw *fs, float *out, const float *p, const float *q) {
    const float zero = gf(0x68a868u);
    float n = gf(0x72c26cu) * gf(0x6909f4u);
    float t0 = q[0] - p[0], t1 = q[1] - p[1], dz = q[2] - p[2];
    float lim = gf(0x68ebacu) * dz;
    fcmp(fs, dz, zero);
    if (dz < zero) {
        fcmp(fs, q[2], n);
        if (q[2] > n) {
            fcmp(fs, fabsf(t0), lim);
            int is_near = fabsf(t0) < lim;
            if (!is_near) { fcmp(fs, fabsf(t1), lim); is_near = fabsf(t1) < lim; }
            if (is_near) {
                float r = gf(0x68a754u) / dz;
                float x = ((n - p[2]) * q[0] + (q[2] - n) * p[0]) * r;
                float y = ((q[2] - n) * p[1] + (n - p[2]) * q[1]) * r;
                out[0] = x; out[1] = y; out[2] = n;
                return 1;
            }
        }
    }
    float len = sqrtf((t1 * t1 + t0 * t0) + dz * dz);
    fcmp(fs, len, zero);
    float scale = len == zero ? gf(0x68a754u) : gf(0x690010u) / len;
    float x = t0 * scale + q[0], y = t1 * scale + q[1], z = scale * dz + q[2];
    out[0] = x; out[1] = y; out[2] = z;
    fcmp(fs, n, q[2]);
    return (n < q[2] || n != n || q[2] != q[2]) ? 0 : 2;
}

/* F_0048c4e0(a, b, c, d): side quad as two triangles a,b,c / a,c,d into the [0xb5445c] buffer (raw 32-bit copies). */
static void emit_side(const float *a, const float *b, const float *c3, const float *d) {
    uint8_t *const mb = g_mem;
    uint32_t n = R32(0xb544a0u), p = R32(0xb5445cu);
    if ((int32_t)((n * 3 + 3) * 2) > 0x8000) return;
    memcpy(mb + p, a, 12); memcpy(mb + p + 12, b, 12); memcpy(mb + p + 24, c3, 12);
    memcpy(mb + p + 36, a, 12); memcpy(mb + p + 48, c3, 12); memcpy(mb + p + 60, d, 12);
    W32(0xb5445cu, p + 72); W32(0xb544a0u, n + 1);
}

/* Normal of the cap edge (a - l) x (b - l) restricted to z = 0, as in the eight cases of F_0048c5b0, then normalised. */
static inline void cap_normal_ab(float *nrm, const float *a, const float *b, const float *l) {
    /* local_18/14/10 = a - l (stored), local_24 = b.x - l.x (stored), local_1c = b.z - l.z (fst), b.y - l.y in register */
    float ax = a[0] - l[0], ay = a[1] - l[1], az = a[2] - l[2];
    float bx = b[0] - l[0], by = b[1] - l[1], bz = b[2] - l[2];
    float y = bz * ay - by * az;
    float x = -(az * bx - bz * ax);
    nrm[0] = x; nrm[1] = y; nrm[2] = 0.0f;
    vec3_normalize(nrm, nrm);
}
static inline void cap_normal_ba(float *nrm, const float *a, const float *b, const float *l) {
    /* cases 3, 5, 6: y = (b.y - l.y)*az - bz*ay, x = -(bz*ax - az*bx) */
    float ax = a[0] - l[0], ay = a[1] - l[1], az = a[2] - l[2];
    float bx = b[0] - l[0], by = b[1] - l[1], bz = b[2] - l[2];
    float y = by * az - bz * ay;
    float x = -(bz * ax - az * bx);
    nrm[0] = x; nrm[1] = y; nrm[2] = 0.0f;
    vec3_normalize(nrm, nrm);
}

/* F_0048c5b0: near cap triangles into the [0xb54460] buffer. Arguments as in the original (p1..p10). */
static void emit_cap(Fsw *fs, const float *p1, const float *p2, const float *p3, const float *p4, uint32_t f5, uint32_t f6,
                     uint32_t f7, const float *p8, const float *p9, const float *p10) {
    uint8_t *const mb = g_mem;
    uint32_t out = R32(0xb54460u), cnt = R32(0xb544a8u);
    if ((int32_t)(cnt * 3 + 3) > 0x4000) return;
    float far_z = gf(0x72c270u), cap_z = gf(0x72c26cu) * gf(0x6909f4u);
    uint32_t mask = ((f7 & 1) << 1 | (f6 & 1)) << 1 | (f5 & 1);
    if (!mask || ((f5 | f6 | f7) & 2)) return;
    fcmp(fs, p8[2], far_z); if (!(p8[2] < far_z)) return;
    fcmp(fs, p9[2], far_z); if (!(p9[2] < far_z)) return;
    fcmp(fs, p10[2], far_z); if (!(p10[2] < far_z)) return;
    const float k = gf(0x68f10cu);
    float o[18], n1[3], n2[3]; int nw = 0;
    #define PUT3(v) (o[nw] = (v)[0], o[nw + 1] = (v)[1], o[nw + 2] = (v)[2], nw += 3)
    #define PUTX(nrm, v) (o[nw] = (nrm)[0] * k + (v)[0], o[nw + 1] = (nrm)[1] * k + (v)[1], o[nw + 2] = cap_z, nw += 3)
    switch (mask) {
    case 1: cnt += 1; cap_normal_ab(n1, p1, p9, p4); cap_normal_ab(n2, p1, p10, p4);
        PUT3(p1); PUTX(n1, p1); PUTX(n2, p1); break;
    case 2: cnt += 1; cap_normal_ab(n1, p2, p8, p4); cap_normal_ab(n2, p2, p10, p4);
        PUTX(n1, p2); PUT3(p2); PUTX(n2, p2); break;
    case 3: cnt += 2; cap_normal_ba(n1, p10, p1, p4); cap_normal_ba(n2, p10, p2, p4);
        PUT3(p1); PUT3(p2); PUTX(n2, p2); PUT3(p1); PUTX(n2, p2); PUTX(n1, p1); break;
    case 4: cnt += 1; cap_normal_ab(n1, p3, p8, p4); cap_normal_ab(n2, p3, p9, p4);
        PUTX(n1, p3); PUTX(n2, p3); PUT3(p3); break;
    case 5: cnt += 2; cap_normal_ba(n1, p9, p1, p4); cap_normal_ba(n2, p9, p3, p4);
        PUT3(p3); PUT3(p1); PUTX(n1, p1); PUT3(p3); PUTX(n1, p1); PUTX(n2, p3); break;
    case 6: cnt += 2; cap_normal_ba(n1, p8, p2, p4); cap_normal_ba(n2, p8, p3, p4);
        PUT3(p2); PUT3(p3); PUTX(n2, p3); PUT3(p2); PUTX(n2, p3); PUTX(n1, p2); break;
    case 7:
        fcmp(fs, p1[2], far_z); if (!(p1[2] < far_z)) return;
        fcmp(fs, p2[2], far_z); if (!(p2[2] < far_z)) return;
        fcmp(fs, p3[2], far_z); if (!(p3[2] < far_z)) return;
        cnt += 1; PUT3(p1); PUT3(p2); PUT3(p3); break;
    default: return;
    }
    #undef PUT3
    #undef PUTX
    memcpy(mb + out, o, 4 * nw);
    W32(0xb544a8u, cnt); W32(0xb54460u, out + 4 * nw);
}

/* GetTransform(device, 2, out) with the original stack layout (args at S-12..S-4, return address at S-16). Direct host
 * read when the vtable slot is the port's own method, else the real guest call. 0: contract not met, nothing to undo. */
static int get_view(CPU *c, uint32_t S, uint32_t ret, uint32_t out) {
    uint8_t *const mb = g_mem;
    uint32_t dev = R32(0x72c610u), fn = R32(R32(dev) + 0x98);
    extern uint32_t g_diag_gettransform_target; int diag_get_transform_host(uint32_t, uint32_t, uint32_t);
    W32(S - 16, ret); W32(S - 12, dev); W32(S - 8, 2); W32(S - 4, out);
    if (fn && fn == g_diag_gettransform_target && diag_get_transform_host(dev, 2, out)) return 1;
    CPU keep = *c;
    c->esp = S - 16; rt_call(c, fn);
    if (c->esp != S) { *c = keep; return 0; }
    keep.fsw = c->fsw; keep.ftop = c->ftop; *c = keep;
    return 1;
}

/* Returns 1 when the call was handled natively; 0: run the translated function instead (nothing was modified). */
int shadow_e140_native(CPU *c) {
    uint8_t *const mb = g_mem;
    if (R32(0x72601cu) == 0xffffu || R32(0x7257dcu) != 0x6483a7u || R32(0x7257e4u) != 0x648639u || R32(0x7257f4u) != 0x64856fu)
        return 0;
    const uint32_t esp0 = c->esp, mesh = R32(esp0 + 4), idx = R32(esp0 + 8), S = esp0 - 0x144;
    const uint32_t obj = idx * 0x78u + R32(0x8e439cu);
    int32_t sign = -(int32_t)(int8_t)R8(0xb544beu), rem = (int16_t)R16(mesh) - 2;
    unsigned long long t0 = sn_now();
    if (!get_view(c, S, 0x48e1bcu, S + 0x104)) return 0;
    unsigned long long t1 = sn_now();
    Fsw fs = { 0, 0 };
    float view[16], objm[16], m[16], lo[3], lv[4];
    memcpy(view, mb + S + 0x104, 64); memcpy(objm, mb + obj, 64); memcpy(lo, mb + obj + 0x40, 12);
    mat_mul(m, objm, view);
    vec3_transform(lv, lo, m);
    float slot[3][4], clipv[3][3]; int32_t flag[3]; float d;
    memcpy(slot, mb + S + 0x84, 48); memcpy(clipv, mb + S + 0x60, 36); memcpy(flag, mb + S + 0x40, 12); memcpy(&d, mb + S + 0x34, 4);
    unsigned long long t2 = sn_now();
    int open = 0, ist = 1, cur = 1, nxt = 0, prv = 0;
    const float zero = gf(0x68a868u);
    uint32_t ebx = mesh + 0x10;
    if (rem > 0) for (;;) {
        int cnt = 0;
        for (;;) {   /* L_230: next vertex; back-facing ones are only stored (raw) and skipped */
            ebx += 0x10; cur = cur + 1 > 2 ? 0 : cur + 1;
            float n0 = (float)(int16_t)R16(ebx), n1 = (float)(int16_t)R16(ebx + 2), n2 = (float)(int16_t)R16(ebx + 4);
            slot[cur][0] = (float)(int16_t)R16(ebx + 8); slot[cur][1] = (float)(int16_t)R16(ebx + 10); slot[cur][2] = (float)(int16_t)R16(ebx + 12);
            rem--; cnt++;
            d = ((slot[cur][2] - lo[2]) * n2 + (slot[cur][1] - lo[1]) * n1) + (slot[cur][0] - lo[0]) * n0;
            sign = -sign;
            fcmp(&fs, d, zero);
            if (!(d < zero) || rem <= 0 || open) break;
        }
        nxt = cur > 1 ? cur - 2 : cur + 1; prv = cur > 0 ? cur - 1 : cur + 2;
        vec3_transform(slot[cur], slot[cur], m);
        flag[cur] = clip_segment(&fs, clipv[cur], lv, slot[cur]);
        int full = 0;   /* 1: go to L_456 (transform prv, and nxt when cnt > 2) */
        if (ebx == mesh + 0x20) {
            slot[nxt][0] = (float)(int16_t)R16(ebx - 0x10); slot[nxt][1] = (float)(int16_t)R16(ebx - 0xe); slot[nxt][2] = (float)(int16_t)R16(ebx - 0xc);
            vec3_transform(slot[nxt], slot[nxt], m); flag[nxt] = clip_segment(&fs, clipv[nxt], lv, slot[nxt]);
            slot[prv][0] = (float)(int16_t)R16(ebx - 8); slot[prv][1] = (float)(int16_t)R16(ebx - 6); slot[prv][2] = (float)(int16_t)R16(ebx - 4);
            vec3_transform(slot[prv], slot[prv], m); flag[prv] = clip_segment(&fs, clipv[prv], lv, slot[prv]);
            full = cnt > 1;
        } else if (ebx == mesh + 0x30 && cnt == 2) {
            slot[nxt][0] = (float)(int16_t)R16(ebx - 0x18); slot[nxt][1] = (float)(int16_t)R16(ebx - 0x16); slot[nxt][2] = (float)(int16_t)R16(ebx - 0x14);
            vec3_transform(slot[nxt], slot[nxt], m); flag[nxt] = clip_segment(&fs, clipv[nxt], lv, slot[nxt]);
            full = 1;
        } else full = cnt > 1;
        if (full) {   /* L_456 */
            vec3_transform(slot[prv], slot[prv], m); flag[prv] = clip_segment(&fs, clipv[prv], lv, slot[prv]);
            if (cnt > 2) { vec3_transform(slot[nxt], slot[nxt], m); flag[nxt] = clip_segment(&fs, clipv[nxt], lv, slot[nxt]); }
        }
        /* L_4e3 */
        float *P = slot[cur], *sN = slot[nxt], *sP = slot[prv], *cC = clipv[cur], *cN = clipv[nxt], *cP = clipv[prv];
        fcmp(&fs, d, zero);
        if (d < zero || d != d) {
            if (open) {   /* L_74a */
                if (sign > 0) emit_side(sN, sP, cP, cN); else emit_side(sP, sN, cN, cP);
                open = 0; ist = 1;
            }
        } else {
            if (sign > 0) emit_cap(&fs, cP, cN, cC, lv, flag[prv], flag[nxt], flag[cur], sP, sN, P);
            else emit_cap(&fs, cN, cP, cC, lv, flag[nxt], flag[prv], flag[cur], sN, sP, P);
            fcmp(&fs, d, zero);
            if (d < zero || d != d) {
                if (open) {
                    if (sign > 0) emit_side(sN, sP, cP, cN); else emit_side(sP, sN, cN, cP);
                    open = 0; ist = 1;
                }
            } else {
                if (sign > 0) emit_side(sN, P, cC, cN); else emit_side(P, sN, cN, cC);
                if (ist) {
                    if (sign > 0) emit_side(sP, sN, cN, cP); else emit_side(sN, sP, cP, cN);
                    ist = 0; open = 1;
                }
            }
        }
        if (rem > 0) continue;   /* L_6e4 */
        if (open) {
            fcmp(&fs, d, zero);
            if (!(d < zero || d != d)) {
                if (sign > 0) emit_side(P, sP, cP, cC); else emit_side(sP, P, cC, cP);
            }
        }
        break;
    }
    /* Frame arrays as the original leaves them on the guest stack. */
    memcpy(mb + S + 0x84, slot, 48); memcpy(mb + S + 0x60, clipv, 36); memcpy(mb + S + 0x40, flag, 12); memcpy(mb + S + 0x34, &d, 4);
    memcpy(mb + S + 0xc4, m, 64); memcpy(mb + S + 0xb4, lv, 16); memcpy(mb + S + 0x4c, lv, 12);
    if (fs.used) c->fsw = (c->fsw & ~(FSW_C0 | FSW_C2 | FSW_C3)) | fs.c_bits;
    c->esp = esp0 + 4;
    unsigned long long t3 = sn_now();
    g_sn_part_ns[0] += t1 - t0; g_sn_part_ns[1] += t2 - t1; g_sn_part_ns[2] += t3 - t2;
    return 1;
}

/* CRT __ftol on a PC24 value: truncation to int64 (integer indefinite out of range or NaN), then the callers keep the low
 * 32 bits and take cdq/xor/sub absolute value. */
static inline int32_t ftol_abs_lo(float v) {
    int64_t t = (v == v && v >= -9223372036854775808.0f && v < 9223372036854775808.0f) ? (int64_t)v : INT64_MIN;
    uint32_t lo = (uint32_t)t, s = (uint32_t)((int32_t)lo >> 31);
    return (int32_t)((lo ^ s) - s);
}
/* x87 PC24 fiadd/fild+fadd of an int32 into a float accumulator: the exact sum fits a double, one rounding to float. */
static inline float add_int(float acc, int32_t v) { return (float)((double)acc + (double)v); }

/* F_0048f040 (type 4 meshes of F_004906a0): contour shadow volume with edge subdivision; persistent arrays
 * VP (0xb544c8, vertices), CP (0x8e2b90, clipped points), FL (0x8e2388, clip results), stride 12/12/4. */
int shadow_f040_native(CPU *c) {
    uint8_t *const mb = g_mem;
    if (R32(0x72601cu) == 0xffffu || R32(0x7257dcu) != 0x6483a7u || R32(0x7257e4u) != 0x648639u || R32(0x7257f4u) != 0x64856fu)
        return 0;
    const uint32_t esp0 = c->esp, mesh = R32(esp0 + 4), idx = R32(esp0 + 8), type = R32(esp0 + 12), S = esp0 - 0x1dc;
    const uint32_t obj = idx * 0x78u + R32(0x8e439cu);
    int32_t left = (int16_t)R16(mesh);
    if (left <= 0) { c->esp = esp0 + 4; return 1; }
    if (!get_view(c, S, 0x48f0b2u, S + 0x19c)) return 0;
    #define VP(k) ((float *)(mb + 0xb544c8u + 12u * (uint32_t)(k)))
    #define CP(k) ((float *)(mb + 0x8e2b90u + 12u * (uint32_t)(k)))
    #define FLA(k) (0x8e2388u + 4u * (uint32_t)(k))
    Fsw fs = { 0, 0 };
    const float zero = gf(0x68a868u), one = gf(0x68a754u), quarter = gf(0x68f030u), hundred = gf(0x69026cu);
    float view[16], objm[16], m[16], lo[3], lv[4], u[3], a[3], b[3], r[16], m2[16];
    memcpy(view, mb + S + 0x19c, 64); memcpy(objm, mb + obj, 64); memcpy(lo, mb + obj + 0x40, 12);
    uint32_t up = R32(0x8e43b4u); memcpy(u, mb + up, 12);
    int32_t flag_on = (int32_t)R32(R32(0x8e43b0u) + 8);
    mat_mul(m, objm, view);
    vec3_transform(lv, lo, m);
    a[0] = u[1] * lo[2] - u[2] * lo[1]; a[1] = u[2] * lo[0] - u[0] * lo[2]; a[2] = u[0] * lo[1] - u[1] * lo[0];
    b[0] = a[1] * u[2] - a[2] * u[1]; b[1] = a[2] * u[0] - a[0] * u[2]; b[2] = a[0] * u[1] - a[1] * u[0];
    vec3_normalize(a, a); vec3_normalize(b, b);
    r[0] = a[0]; r[1] = a[1]; r[2] = a[2]; r[3] = 0; r[4] = u[0]; r[5] = u[1]; r[6] = u[2]; r[7] = 0;
    r[8] = b[0]; r[9] = b[1]; r[10] = b[2]; r[11] = 0; r[12] = 0; r[13] = 0; r[14] = 0; r[15] = 1.0f;
    int32_t mode;
    mode = 0;
    if (type == 4) {
        if (!flag_on) mode = 2;
        else { float g = gf(0xb544b4u); fcmp(&fs, g, zero); mode = (g < zero || g != g) ? 2 : 1; }
    }
    /* Frame arrays of the original (uninitialised slots keep the guest stack bytes). */
    float t[2][4], cs[2][3], v0[4], c0[3]; int32_t fl[2], other;
    memcpy(t, mb + S + 0x68, 32); memcpy(cs, mb + S + 0xb4, 24); memcpy(fl, mb + S + 0x28, 8); memcpy(&other, mb + S + 0x4c, 4);
    uint32_t ev = mesh + 0x18;
    v0[0] = (float)(int16_t)R16(ev); v0[1] = (float)(int16_t)R16(ev + 2); v0[2] = (float)(int16_t)R16(ev + 4);
    memcpy(&v0[3], mb + S + 0x84, 4);   /* w of the slot at S+0x78, as left on the stack */
    left--;
    mat_mul(m2, r, m);
    vec3_transform(v0, v0, m2);
    int32_t f0 = clip_segment(&fs, c0, lv, v0);
    memcpy(VP(0), v0, 12); memcpy(CP(0), c0, 12); W32(FLA(0), f0);
    memcpy(t[1], v0, 16); memcpy(cs[1], c0, 12);   /* the original builds them in slot 1 (S+0x78, S+0xc0) */
    int32_t k = 1, cur = 1;
    for (; left > 0; left--) {
        ev += 8; cur = cur + 1 > 1 ? 0 : cur + 1;
        float *tc = t[cur];
        tc[0] = (float)(int16_t)R16(ev); tc[1] = (float)(int16_t)R16(ev + 2); tc[2] = (float)(int16_t)R16(ev + 4);
        vec3_transform(tc, tc, m2);
        int32_t f = clip_segment(&fs, cs[cur], lv, tc);
        fl[cur] = f; other = 1 - cur;
        float *to = t[other];
        int split = 0;
        if (mode) {
            int32_t dy = ftol_abs_lo(to[1] - tc[1]);
            float dyf = (float)dy; fcmp(&fs, dyf, hundred);
            split = dyf > hundred;
        }
        if (split) {
            float tt = 0.25f;   /* 0x3e800000 written as an immediate */
            int32_t ks = k;
            for (int32_t j = k; j < ks + 3; j++) {
                float w = one - tt;
                float *vp = VP(j);
                vp[0] = tt * tc[0] + w * to[0]; vp[1] = tt * tc[1] + w * to[1]; vp[2] = tt * tc[2] + w * to[2];
                W32(FLA(j), clip_segment(&fs, CP(j), lv, vp));
                if (j == ks) emit_side(vp, to, cs[other], CP(j)); else emit_side(vp, VP(j - 1), CP(j - 1), CP(j));
                tt = tt + quarter;
            }
            k = ks + 3;
            emit_side(tc, VP(k - 1), CP(k - 1), cs[cur]);
        } else emit_side(tc, to, cs[other], cs[cur]);
        memcpy(VP(k), tc, 12); memcpy(CP(k), cs[cur], 12); W32(FLA(k), f);
        k++;
    }
    /* After the contour: closing edge back to the first vertex. */
    int split = 0;
    if (mode) {
        /* other is 0/1 once the contour loop ran; with a single vertex the original reads a stale stack slot */
        float oy = (other == 0 || other == 1) ? t[other][1] : gf(S + 0x6c + 16u * (uint32_t)other);
        int32_t dy = ftol_abs_lo(oy - t[cur][1]);
        float dyf = (float)dy; fcmp(&fs, dyf, hundred);
        split = dyf > hundred;
    }
    if (split) {
        float tt = 0.25f, *tc = t[cur]; int32_t ks = k;
        for (int32_t j = k; j < ks + 3; j++) {
            float w = one - tt, *vp = VP(j);
            vp[0] = w * tc[0] + v0[0] * tt; vp[1] = w * tc[1] + v0[1] * tt; vp[2] = w * tc[2] + v0[2] * tt;
            W32(FLA(j), clip_segment(&fs, CP(j), lv, vp));
            if (j == ks) emit_side(vp, tc, cs[cur], CP(j)); else emit_side(vp, VP(j - 1), CP(j - 1), CP(j));
            tt = tt + quarter;
        }
        k = ks + 3;
        emit_side(v0, VP(k - 1), CP(k - 1), c0);
    } else emit_side(v0, t[cur], cs[cur], c0);
    /* Triangulation direction from two sums of |dy| over the clipped contour, then the caps. */
    int32_t n = k, last = n - 1, half = n / 2;
    float accA = zero, accB = zero;
    { int32_t i = 0, jb = last;
      for (int32_t q = half; q > 0; q--) {
          int32_t dy = ftol_abs_lo(CP(i)[1] - CP(jb)[1]);
          i++; if (i > last) i = 0;
          accB = add_int(accB, dy);
          jb--; if (jb < 0) jb = last;
      } }
    { int32_t i = 1, jb = 0;
      for (int32_t q = half; q > 0; q--) {
          int32_t dy = ftol_abs_lo(CP(i)[1] - CP(jb)[1]);
          i++; if (i > last) i = 0;
          accA = add_int(accA, dy);
          jb--; if (jb < 0) jb = last;
      } }
    fcmp(&fs, accB, accA);
    int32_t es, eb;
    if (accB < accA) { es = last; eb = 0; } else { es = 0; eb = 1; }
    for (int32_t j = 0; j < n - 2; j++) {
        if (!(j & 1)) {
            int32_t ed = eb + 1 > last ? 0 : eb + 1;
            emit_cap(&fs, CP(eb), CP(es), CP(ed), lv, R32(FLA(eb)), R32(FLA(es)), R32(FLA(ed)), VP(eb), VP(es), VP(ed));
            es--; eb = ed; if (es < 0) es = last;
        } else {
            int32_t ed = es + 1 > last ? 0 : es + 1;
            emit_cap(&fs, CP(es), CP(eb), CP(ed), lv, R32(FLA(es)), R32(FLA(eb)), R32(FLA(ed)), VP(es), VP(eb), VP(ed));
        }
    }
    #undef VP
    #undef CP
    #undef FLA
    /* Frame arrays as left by the original. */
    memcpy(mb + S + 0x68, t, 32); memcpy(mb + S + 0xb4, cs, 24); memcpy(mb + S + 0x34, v0, 16); memcpy(mb + S + 0x5c, c0, 12);
    memcpy(mb + S + 0x88, lv, 12); memcpy(mb + S + 0x10c, lv, 16); memcpy(mb + S + 0x11c, m, 64); memcpy(mb + S + 0x15c, m2, 64);
    memcpy(mb + S + 0xcc, r, 64); memcpy(mb + S + 0xa0, a, 12); memcpy(mb + S + 0x94, b, 12);
    if (fs.used) c->fsw = (c->fsw & ~(FSW_C0 | FSW_C2 | FSW_C3)) | fs.c_bits;
    c->esp = esp0 + 4;
    return 1;
}

/* F_0048faa0: triangle-strip shadow volumes, types 5 and 6 of the dispatcher F_00490760 (which stores the strip parity
 * in [0xb544be] before jumping). Records of 16 bytes: int16 normal at +0, +2, +4 and position at +8, +0xa, +0xc. A ring
 * of three vertex slots keeps each position transformed by the light matrix [0x8e43bc] (B) and by the object matrix (A);
 * a vertex is transformed lazily, only when an edge needs it. Silhouette edges go through F_0048c4e0 (emit_side).
 * Same contract as F_0048e140 (PC24, D3DX x87 0x6483a7). The frame slots of the original are dead after return: the
 * caller F_00490cf0 reloads every register it reads. */
int shadow_strip_native(CPU *c) {
    uint8_t *const mb = g_mem;
    if (R32(0x72601cu) == 0xffffu || R32(0x7257dcu) != 0x6483a7u) return 0;
    const uint32_t esp0 = c->esp, mesh = R32(esp0 + 4), idx = R32(esp0 + 8);
    const int type = (int16_t)R16(mesh + 2);
    if (type != 5 && type != 6) return 0;
    W8(0xb544beu, type == 5 ? 0xffu : 1u);
    { static int once; if (!once) { once = 1; fprintf(stderr, "[shadow-strip] native strip volumes running\n"); } }
    float M1[16], MO[16], L[3], A[3][4], B[3][4];
    memcpy(M1, mb + 0x8e43bcu, 64); memcpy(MO, mb + idx * 0x78u + R32(0x8e439cu), 64); memcpy(L, mb + 0x8e4450u, 12);
    const float zero = gf(0x68a868u);
    Fsw fs = {0, 0};
    int32_t sgn = -(int32_t)(int8_t)R8(0xb544beu), count = (int16_t)R16(mesh) - 2;
    int e = 1, k = 0, flag78 = 0, first = 1, n = 0, C = 0;
    float dot = 0;
    uint32_t rec = mesh + 0x10;
    const uint32_t rec1 = mesh + 0x20;
    #define SH(o) ((float)(int16_t)R16(rec + (o)))
    #define LOADP(s, o) (A[s][0] = SH(o), A[s][1] = SH((o) + 2), A[s][2] = SH((o) + 4))
    #define XFORM(s) (vec3_transform(B[s], A[s], M1), vec3_transform(A[s], A[s], MO))
    if (count <= 0) goto out;
    for (;;) {
        /* 0x48fb20 */
        rec += 0x10;
        if (++e > 2) e = 0;
        { float n0 = SH(0), n1 = SH(2), n2 = SH(4);
          LOADP(e, 8);
          count--; k++; sgn = -sgn;
          dot = (n1 * L[1] + n2 * L[2]) + n0 * L[0]; }
        fcmp(&fs, dot, zero);
        if (dot < zero && count > 0 && !flag78) continue;
        n = e > 1 ? e - 2 : e + 1;
        C = e > 0 ? e - 1 : e + 2;
        XFORM(e);
        if (rec == rec1) {
            LOADP(n, -0x10); XFORM(n);
            LOADP(C, -8); XFORM(C);
            if (k > 1) goto c_slot;
        } else if (rec == rec1 + 0x10 && k == 2) {
            LOADP(n, -0x18); XFORM(n);
            goto c_slot;
        } else if (k > 1) {
        c_slot:
            XFORM(C);
            if (k > 2) XFORM(n);
        }
        fcmp(&fs, dot, zero);
        if (!(dot < zero) && dot == dot) {
            if (sgn > 0) emit_side(A[n], A[e], B[e], B[n]); else emit_side(A[e], A[n], B[n], B[e]);
            if (first) {
                if (sgn > 0) emit_side(A[C], A[n], B[n], B[C]); else emit_side(A[n], A[C], B[C], B[n]);
                first = 0; flag78 = 1;
            }
        } else if (flag78) {
            if (sgn > 0) emit_side(A[n], A[C], B[C], B[n]); else emit_side(A[C], A[n], B[n], B[C]);
            flag78 = 0; first = 1;
        }
        if (count <= 0) break;
        k = 0;
    }
    if (flag78) {
        fcmp(&fs, dot, zero);
        if (!(dot < zero) && dot == dot) {
            if (sgn > 0) emit_side(A[e], A[C], B[C], B[e]); else emit_side(A[C], A[e], B[e], B[C]);
        }
    }
out:
    #undef SH
    #undef LOADP
    #undef XFORM
    if (fs.used) c->fsw = (c->fsw & ~(FSW_C0 | FSW_C2 | FSW_C3)) | fs.c_bits;
    c->esp = esp0 + 4;
    return 1;
}
