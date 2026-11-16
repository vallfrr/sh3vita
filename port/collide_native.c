/* Native segment / quad intersection test F_004f3300 (with its cross product F_0060c030), called by the collision code
 * (F_004aecb0 and others): int F_004f3300(out, p2, p3, q0, q1, q2, q3) with the segment p2 -> p3 and the quad q0..q3.
 *   1. per axis, reject when min(p2, p3) > all four quad vertices, then when max(p2, p3) < all four;
 *   2. plane n = cross(q2 - q0, q1 - q0), d = -n.q0; reject unless p2 or p3 is below it (or NaN);
 *   3. for each edge (q0q1, q1q2, q2q3, q3q0): reject when (cross(edge, p3 - p2) . (p2 - edge start)) > 0;
 *   4. hit: t = -(n.p2 + d) / (n.(p3 - p2)) (0 when the denominator is 0), out = {1, p2 + t (p3 - p2)}, return 1.
 * Numeric contract: x87 PC24 = binary32 in the original order (see shadow_native.c); every association below follows the
 * x87 code. Verified against the original bytes under Unicorn: tools/collide_native_proof.py.
 * Guest-visible effects reproduced: eax, out (written only on a hit), esp after return, callee-saved registers, x87 C0/C2/C3
 * of the last compare. Not reproduced: ecx/edx and the dead locals below esp. Off unless g_collide_native. */
#include <stdint.h>
#include <string.h>
#include "x86rt.h"

int g_collide_native;

typedef struct { uint32_t bits; int used; } Fsw;
static inline void fcmp(Fsw *s, float a, float b) {
    s->used = 1;
    s->bits = a < b ? FSW_C0 : a > b ? 0 : a == b ? FSW_C3 : FSW_C0 | FSW_C2 | FSW_C3;
}
static inline void ld3(float *v, uint32_t a) { memcpy(v, g_mem + a, 12); }
/* F_0060c030(out, a, b) */
static inline void cross(float *o, const float *a, const float *b) {
    float x = b[2] * a[1] - a[2] * b[1], y = a[2] * b[0] - b[2] * a[0], z = a[0] * b[1] - b[0] * a[1];
    o[0] = x; o[1] = y; o[2] = z;
}

int collide_native(CPU *c) {
    uint8_t *const mb = g_mem;
    const uint32_t E = c->esp, out = R32(E + 4);
    float p2[3], p3[3], q[4][3];
    ld3(p2, R32(E + 8)); ld3(p3, R32(E + 12));
    for (int k = 0; k < 4; k++) ld3(q[k], R32(E + 16 + 4 * k));
    Fsw fs = {0, 0};
    int hit = 0;
    const float zero = rt_f32(R32(0x68a868u));
    for (int i = 0; i < 3; i++) {
        float A = p2[i], B = p3[i];
        fcmp(&fs, A, B);
        float m = A < B ? A : B;
        int k = 0;
        for (; k < 4; k++) { fcmp(&fs, m, q[k][i]); if (!(m > q[k][i])) break; }
        if (k == 4) goto done;
    }
    for (int i = 0; i < 3; i++) {
        float A = p2[i], B = p3[i];
        fcmp(&fs, A, B);
        float M = A > B ? A : B;
        int k = 0;
        for (; k < 4; k++) { fcmp(&fs, M, q[k][i]); if (!(M < q[k][i])) break; }
        if (k == 4) goto done;
    }
    {
        float u[3] = {q[1][0] - q[0][0], q[1][1] - q[0][1], q[1][2] - q[0][2]};
        float v[3] = {q[2][0] - q[0][0], q[2][1] - q[0][1], q[2][2] - q[0][2]};
        float n[3]; cross(n, v, u);
        const float d = -((n[0] * q[0][0] + n[1] * q[0][1]) + n[2] * q[0][2]);
        float s2 = ((n[1] * p2[1] + n[2] * p2[2]) + n[0] * p2[0]) + d;
        fcmp(&fs, s2, zero);
        if (!(s2 < zero || s2 != s2)) {
            float s3 = ((n[1] * p3[1] + n[2] * p3[2]) + n[0] * p3[0]) + d;
            fcmp(&fs, s3, zero);
            if (!(s3 < zero || s3 != s3)) goto done;
        }
        const float w[3] = {p3[0] - p2[0], p3[1] - p2[1], p3[2] - p2[2]};
        static const int edge[4][2] = {{1, 0}, {2, 1}, {3, 2}, {0, 3}};   /* e = q[a] - q[b], from q[b] */
        for (int k = 0; k < 4; k++) {
            const float *qa = q[edge[k][0]], *qb = q[edge[k][1]];
            float e[3] = {qa[0] - qb[0], qa[1] - qb[1], qa[2] - qb[2]};
            float r[3] = {p2[0] - qb[0], p2[1] - qb[1], p2[2] - qb[2]}, C[3];
            cross(C, e, w);
            float t = (C[2] * r[2] + C[1] * r[1]) + C[0] * r[0];
            fcmp(&fs, t, zero);
            if (t > zero) goto done;
        }
        float num = -(((n[1] * p2[1] + n[2] * p2[2]) + n[0] * p2[0]) + d);
        float den = (w[1] * n[1] + w[2] * n[2]) + w[0] * n[0];
        fcmp(&fs, den, zero);
        float t = den == zero ? zero : num / den;
        float o[3] = {w[0] * t + p2[0], w[1] * t + p2[1], w[2] * t + p2[2]};
        memcpy(mb + out + 4, o, 12); W32(out, 1);
        hit = 1;
    }
done:
    if (fs.used) c->fsw = (c->fsw & ~(FSW_C0 | FSW_C2 | FSW_C3)) | fs.bits;
    c->eax = (uint32_t)hit;
    c->esp = E + 4;
    return 1;
}

/* Native early exit of the wall-edge test F_004ae5c0(out, wall, obj) (collision, called by F_004b0150 in another unit):
 * out[0] = 0; n = F_004f26d0(wall) (normal of the wall quad: cross product of two edges, n[3] = -n.p0); if
 * (n2*d2 + d0*n0) + n1*d1 < 0 with d = obj+0x10 - wall+0x30, the original returns at once. That common case is done
 * here with the original's leftovers (eax = out high half + x87 status word, ecx = edx = the normal's frame address,
 * normal and d in the frame); any other case returns 0 and the translated function runs from the start (the only
 * side effect so far, out[0] = 0, is redone identically). PC24 = binary32 in the x87 order.
 * Verified against the original bytes under Unicorn: tools/collide_native_proof.py (mode wall). Off unless g_wall_native. */
int g_wall_native;
int wall_prefilter_native(CPU *c) {
    uint8_t *const mb = g_mem;
    const uint32_t E = c->esp, out = R32(E + 4), w = R32(E + 8), o = R32(E + 12);
    float W[16]; memcpy(W, mb + w, 64);
    const float d0 = rt_f32(R32(o + 0x10)) - W[12], d1 = rt_f32(R32(o + 0x14)) - W[13], d2 = rt_f32(R32(o + 0x18)) - W[14];
    const float a = W[8] - W[4], b = W[9] - W[5], c3 = W[10] - W[6], e0 = W[12] - W[8], e1 = W[13] - W[9], e2 = W[14] - W[10];
    const float n0 = e1 * c3 - e2 * b, n1 = e2 * a - e0 * c3, n2 = e0 * b - a * e1;
    const float n3 = -((W[6] * n2 + W[5] * n1) + W[4] * n0);
    const float dot = (n2 * d2 + d0 * n0) + n1 * d1, zero = rt_f32(R32(0x68a868u));
    if (!(dot < zero)) return 0;
    const uint32_t N = E - 0x68;
    W32(out, 0);
    memcpy(mb + N, &n0, 4); memcpy(mb + N + 4, &n1, 4); memcpy(mb + N + 8, &n2, 4); memcpy(mb + N + 12, &n3, 4);
    memcpy(mb + E - 0x58, &d0, 4); memcpy(mb + E - 0x54, &d1, 4); memcpy(mb + E - 0x50, &d2, 4);
    c->fsw = (c->fsw & ~(FSW_C0 | FSW_C2 | FSW_C3)) | FSW_C0;
    c->eax = (out & 0xffff0000u) | (rt_fnstsw(c) & 0xffffu);
    c->ecx = c->edx = N;
    c->esp = E + 4;
    return 1;
}
