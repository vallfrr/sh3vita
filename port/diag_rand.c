/* SH3 MSVC rand warm path, adapted from sh4vita rt/natives.c (GPL-3.0).
 * Preserve the original initialization path and the warm guest stack writes.
 * Experimental: built only with SH3_RAND_LAB, enabled only by marker. */
#include "x86rt.h"
#include <stddef.h>

extern uint32_t g_mem_size;
extern uint32_t diag_rand_thread_data(CPU *c);
extern void __real_F_0066405e(CPU *c);
int diag_rand_enabled;

static uint64_t code_hash(const uint8_t *p,size_t n) {
    uint64_t h=UINT64_C(0xcbf29ce484222325);
    while(n--) h=(h^*p++)*UINT64_C(0x100000001b3);
    return h;
}
int diag_rand_layout_valid(void) {
    return g_mem && g_mem_size>0x7296bcu &&
        code_hash(g_mem+0x66405e,0x22)==UINT64_C(0x69aa1c4bc85d4916) &&
        code_hash(g_mem+0x667a7f,0x71)==UINT64_C(0xf31478d55f95bd4d);
}
void __wrap_F_0066405e(CPU *c) {
    if (!diag_rand_enabled || c->esp<20 || c->esp>g_mem_size-4) {
        __real_F_0066405e(c); return;
    }
    uint32_t p=diag_rand_thread_data(c);
    if (!p || p>g_mem_size-0x88) { __real_F_0066405e(c); return; }
    uint8_t *const mb=g_mem;
    /* Warm __getptd calls GetLastError, TlsGetValue and SetLastError. Their
       final TIB state is unchanged; eax/ecx below match the original rand.
       Keep all five stale stack words, not just the cdecl-visible result. */
    W32(c->esp-4,0x664063u);
    W32(c->esp-8,c->ebx); W32(c->esp-12,c->esi);
    W32(c->esp-16,R32(c->fs_base+0x34));
    W32(c->esp-20,0x667aebu);
    uint32_t next=R32(p+0x14)*0x343fdu+0x269ec3u;
    W32(p+0x14,next);
    c->eax=(next>>16)&0x7fffu; c->ecx=next; c->esp+=4;
}
