/* Exact compact implementation of the retained SH3 CRT conversion, including
   scratch writes and volatile registers. Original x87 rounding helper retained. */
#include "x86rt.h"
extern unsigned g_diag_perf;
extern void diag_ftol_retained(CPU *c);
void F_00662c40(CPU *c) {
    if (!(g_diag_perf&4096u)) { diag_ftol_retained(c); return; }
    uint8_t *const mb=g_mem;
    uint32_t saved_sp=c->esp, scratch=(saved_sp-36u)&~15u;
    double value=c->st[c->ftop&7u];
    W32(saved_sp-4,c->ebp);
    uint32_t sign=rt_u32f((float)value);
    W32(scratch+24,sign);
    uint64_t rounded=(uint64_t)rt_fist64(c,value);
    W64(scratch+16,rounded);
    uint64_t result=rounded;
    if ((uint32_t)rounded || ((uint32_t)(rounded>>32)&0x7fffffffu)) {
        uint32_t diff=rt_u32f((float)(value-(double)(int64_t)rounded));
        W32(scratch,diff);
        uint64_t sum=(uint64_t)(sign&0x80000000u ? diff^0x80000000u : diff)+0x7fffffffu;
        c->ecx=(uint32_t)sum;
        result=sign&0x80000000u ? rounded+(sum>>32) : rounded-(sum>>32);
    } else {
        /* Both stores exist in the original, with the last one surviving. */
        W32(scratch+24,rt_u32f((float)(int64_t)rounded));
        W32(scratch+24,rt_u32f((float)value));
    }
    c->eax=(uint32_t)result;c->edx=(uint32_t)(result>>32);
    c->esp=saved_sp+4;c->ftop=(c->ftop+1)&7u;
}
