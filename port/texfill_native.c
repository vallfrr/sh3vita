/* Native F_00436430: fills a locked 32-bit texture from an 8-bit indexed image, once per frame for the 128x128 texture
 * of F_00436840 (palette index bits 3 and 4 swapped, then R and B swapped in the palette entry).
 *   eax = object {+8 source bytes, +0xc palette (256 x uint32), +0x58 IDirect3DTexture8}; [esp+4] width, [esp+8] height.
 * LockRect(level 0, rect {0, 0, width, height}, flags 0) and UnlockRect(0) stay guest COM calls, with the original
 * argument blocks on the guest stack. Integer only. Verified against the original bytes under Unicorn:
 * tools/texfill_native_proof.py. Guest-visible effects reproduced: the texture bytes, the lock arguments (rect and
 * LockedRect slots), eax/ecx/edx as left by the last COM call, esp, callee-saved registers. Not reproduced: the dead
 * frame slot holding the last palette entry. Off unless g_texfill_native. */
#include <stdint.h>
#include <stdio.h>
#include "x86rt.h"

int g_texfill_native;
void rt_call(CPU *c, uint32_t target);

int texfill_native(CPU *c) {
    uint8_t *const mb = g_mem;
    const uint32_t E = c->esp, obj = c->eax, width = R32(E + 4), height = R32(E + 8);
    const uint32_t tex = R32(obj + 0x58), pal = R32(obj + 0xc), S = E - 0x34;
    uint32_t src = R32(obj + 8);
    const uint32_t ebx = c->ebx, ebp = c->ebp, esi = c->esi, edi = c->edi;
    W32(S + 0x24, 0); W32(S + 0x28, 0); W32(S + 0x2c, width); W32(S + 0x30, height); W32(S + 0x18, tex);
    W32(S - 4, 0); W32(S - 8, S + 0x24); W32(S - 12, S + 0x1c); W32(S - 16, 0); W32(S - 20, tex); W32(S - 24, 0x436470u);
    c->esp = S - 24; c->ebx = tex; c->ebp = width; c->esi = src; c->edi = pal;
    { static int once; if (!once) { once = 1; fprintf(stderr, "[texfill-native] native texture fill running %ux%u\n", width, height); } }
    rt_call(c, R32(R32(tex) + 0x40));
    if ((int32_t)c->eax >= 0) {
        const uint32_t pitch = R32(S + 0x1c);
        uint32_t row = R32(S + 0x20);
        for (int32_t y = (int32_t)height; y > 0; y--, row += pitch) {
            uint32_t d = row;
            for (int32_t x = (int32_t)width; x > 0; x--, d += 4) {
                uint32_t b = R8(src++), v = R32(pal + 4 * (((b >> 1) & 8) | ((b & 8) << 1) | (b & 0xe7)));
                W32(d, ((v >> 16) & 0xff) | ((v & 0xff) << 16) | (v & 0xff00ff00u));
            }
        }
        W32(S - 4, 0); W32(S - 8, tex); W32(S - 12, 0x436505u);
        c->esp = S - 12; c->ebx = tex;
        rt_call(c, R32(R32(tex) + 0x44));
    }
    c->ebx = ebx; c->ebp = ebp; c->esi = esi; c->edi = edi;
    c->esp = E + 4;
    return 1;
}

/* Native F_00678070(dst, src): 16-dword copy (rep movsd, forward, word by word as the original even when the ranges
 * overlap). Leaves ecx = 0, eax/edx untouched, esi/edi restored (pushed and popped by the original). Falls back to
 * the translation if the direction flag is set. Checked by tools/texfill_native_proof.py (mode copy64). */
int g_copy64_native;
int copy64_native(CPU *c) {
    uint8_t *const mb = g_mem;
    if (c->df) return 0;
    const uint32_t E = c->esp, dst = R32(E + 4), src = R32(E + 8);
    for (uint32_t k = 0; k < 64; k += 4) W32(dst + k, R32(src + k));
    c->ecx = 0;
    c->esp = E + 4;
    return 1;
}
