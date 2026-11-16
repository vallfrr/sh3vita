/* Native MSVC CRT qsort F_00663a30 (with its shortsort F_006639c0) for the two hot comparators of the game:
 * F_00462240 (float at +8, descending: a<b -> 1, a>b -> -1, equal or unordered -> 0, x87 compare of binary32 values) and
 * F_005f49b0 (int32 [b] - [a], wrapping). Any other comparator runs the translated qsort.
 *
 * Same algorithm step for step (median of three, equal-key skipping, explicit 30-entry stack, shortsort below 9 elements,
 * byte swaps skipped when both pointers are equal), hence the same final order: the comparators are pure functions of the
 * element bytes. Verified against the original bytes under Unicorn: tools/qsort_native_proof.py.
 * Guest-visible effects reproduced: the sorted array, esp after return, callee-saved registers, x87 C0/C2/C3 of the last
 * comparator compare (F_00462240). Not reproduced: eax/ecx/edx (void function, cdecl scratch) and the dead stack below esp
 * (qsort's 0x100-byte lo/hi stacks and the comparators' argument slots). Off unless g_qsort_native. */
#include <stdint.h>
#include <string.h>
#include "x86rt.h"

int g_qsort_native;

typedef struct { uint32_t kind, bits, used; } Cmp;
static inline int cmp_call(Cmp *k, uint32_t a, uint32_t b) {
    uint8_t *const mb = g_mem;
    if (k->kind == 1) {
        float x, y; memcpy(&x, mb + a + 8, 4); memcpy(&y, mb + b + 8, 4);
        k->used = 1; k->bits = x < y ? FSW_C0 : x > y ? 0 : x == y ? FSW_C3 : FSW_C0 | FSW_C2 | FSW_C3;
        return x < y ? 1 : x > y ? -1 : 0;
    }
    return (int32_t)(R32(b) - R32(a));
}
static inline void swap_bytes(uint32_t a, uint32_t b, uint32_t w) {
    if (a == b) return;
    uint8_t *p = g_mem + a, *q = g_mem + b;
    while (w--) { uint8_t t = *p; *p++ = *q; *q++ = t; }
}
static void shortsort(Cmp *k, uint32_t lo, uint32_t hi, uint32_t w) {
    while (hi > lo) {
        uint32_t max = lo;
        for (uint32_t p = lo + w; p <= hi; p += w)
            if (cmp_call(k, p, max) > 0) max = p;
        swap_bytes(max, hi, w);
        hi -= w;
    }
}

int qsort_native(CPU *c) {
    uint8_t *const mb = g_mem;
    const uint32_t base = R32(c->esp + 4), num = R32(c->esp + 8), w = R32(c->esp + 12), fn = R32(c->esp + 16);
    Cmp k = {fn == 0x462240u ? 1u : fn == 0x5f49b0u ? 2u : 0u, 0, 0};
    if (!k.kind) return 0;
    if (num >= 2 && w) {
        uint32_t lostk[30], histk[30]; int sp = 0;
        uint32_t lo = base, hi = base + w * (num - 1);
        for (;;) {
            uint32_t size = (hi - lo) / w + 1;
            if (size <= 8) shortsort(&k, lo, hi, w);
            else {
                uint32_t mid = lo + (size / 2) * w;
                if (cmp_call(&k, lo, mid) > 0) swap_bytes(lo, mid, w);
                if (cmp_call(&k, lo, hi) > 0) swap_bytes(lo, hi, w);
                if (cmp_call(&k, mid, hi) > 0) swap_bytes(mid, hi, w);
                uint32_t loguy = lo, higuy = hi;
                for (;;) {
                    if (mid > loguy) { do loguy += w; while (loguy < mid && cmp_call(&k, loguy, mid) <= 0); }
                    if (mid <= loguy) { do loguy += w; while (loguy <= hi && cmp_call(&k, loguy, mid) <= 0); }
                    do higuy -= w; while (higuy > mid && cmp_call(&k, higuy, mid) > 0);
                    if (higuy < loguy) break;
                    swap_bytes(higuy, loguy, w);
                    if (mid == higuy) mid = loguy;
                }
                higuy += w;
                if (mid < higuy) { do higuy -= w; while (higuy > mid && cmp_call(&k, higuy, mid) == 0); }
                if (mid >= higuy) { do higuy -= w; while (higuy > lo && cmp_call(&k, higuy, mid) == 0); }
                if ((int32_t)(higuy - lo) >= (int32_t)(hi - loguy)) {
                    if (lo < higuy) { lostk[sp] = lo; histk[sp] = higuy; ++sp; }
                    if (loguy < hi) { lo = loguy; continue; }
                } else {
                    if (loguy < hi) { lostk[sp] = loguy; histk[sp] = hi; ++sp; }
                    if (lo < higuy) { hi = higuy; continue; }
                }
            }
            if (--sp < 0) break;
            lo = lostk[sp]; hi = histk[sp];
        }
    }
    if (k.used) c->fsw = (c->fsw & ~(FSW_C0 | FSW_C2 | FSW_C3)) | k.bits;
    c->esp += 4;
    return 1;
}
