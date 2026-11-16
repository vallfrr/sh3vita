/* Native F_00446b30 (toroidal 3x3 smoothing of a signed 8-bit image, one call per frame for the animated texture source)
 * with its row kernel F_004464e0. Integer only.
 *   F_00446b30(src, dst, stride, rows): rows 1..rows-2, then row rows-1 (below = row 0), then row 0 (above = row rows-1).
 *   F_004464e0(above, center, below, dst) with ecx = stride / 16: T[i] = above[i] + center[i] + below[i] (int16, kept in
 *   the global table at 0x74aa08), then dst[i] = (((T[i-1] + T[i] + T[i+1]) & 0xffff) - c) >> 3 + c) >> 1 with c =
 *   center[i]; at both ends the sum reads the 32-bit word T[0] | T[1] << 16 as in the original.
 * Every 16/32-bit truncation of the original is reproduced. Verified against the original bytes under Unicorn:
 * tools/blur_native_proof.py. Guest-visible effects reproduced: dst, the table at 0x74aa08, the two argument slots the
 * original reuses as locals ([esp+0xc], [esp+0x10]), eax/ecx/edx at return, esp, callee-saved registers.
 * Not reproduced: dead stack below esp. Off unless g_blur_native. */
#include <stdint.h>
#include "x86rt.h"

int g_blur_native;

/* F_004464e0; returns edx (last stored value before truncation) and sets n for ecx. */
static uint32_t blur_row(uint32_t above, uint32_t center, uint32_t below, uint32_t dst, int32_t n) {
    uint8_t *const mb = g_mem;
    for (int32_t i = 0; i < n; i++)
        W16(0x74aa08u + 2 * i, (uint16_t)((int8_t)R8(above + i) + (int8_t)R8(below + i) + (int8_t)R8(center + i)));
    int32_t s = (int8_t)R8(center);
    uint32_t e = (uint32_t)(uint16_t)(R16(0x74aa06u + 2 * n) + R16(0x74aa0au)) + R32(0x74aa08u);
    e = (uint32_t)((((int32_t)(e - (uint32_t)s) >> 3) + s) >> 1);
    W8(dst, e);
    for (int32_t i = 1; i < n - 1; i++) {
        int32_t si = (int8_t)R8(center + i);
        uint32_t b = (uint16_t)(R16(0x74aa06u + 2 * i) + R16(0x74aa0au + 2 * i) + R16(0x74aa08u + 2 * i));
        W8(dst + i, (uint32_t)((((int32_t)(b - (uint32_t)si) >> 3) + si) >> 1));
    }
    s = (int8_t)R8(center + n - 1);
    e = (uint32_t)(uint16_t)(R16(0x74aa04u + 2 * n) + R16(0x74aa06u + 2 * n)) + R32(0x74aa08u);
    e = (uint32_t)((((int32_t)(e - (uint32_t)s) >> 3) + s) >> 1);
    W8(dst + n - 1, e);
    return e;
}

int blur_native(CPU *c) {
    uint8_t *const mb = g_mem;
    const uint32_t E = c->esp, a1 = R32(E + 4), a2 = R32(E + 8), stride = R32(E + 12), rows = R32(E + 16);
    const int32_t n = (((int32_t)stride + (int32_t)(((uint32_t)((int32_t)stride >> 31)) & 0xf)) >> 4) * 16;
    uint32_t center = a1 + stride, dst = a2 + stride, edx;
    W32(E + 12, dst);
    int32_t cnt = (int32_t)rows - 2;
    if (cnt > 0) {
        uint32_t above = a1;
        do {
            blur_row(above, center, center + stride, dst, n);
            dst += stride; above += stride; center += stride;
            W32(E + 12, dst); W32(E + 16, (uint32_t)--cnt);
        } while (cnt);
    }
    blur_row(center - stride, center, a1, R32(E + 12), n);
    edx = blur_row(center, a1, a1 + stride, a2, n);
    c->eax = a2; c->ecx = (uint32_t)n; c->edx = edx;
    c->esp = E + 4;
    return 1;
}
