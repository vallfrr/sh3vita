/* Optional linker wrappers for profiling SH3's observed worker subcalls.
 * The build option is disabled by default; no guest behavior is replaced. */
#include "x86rt.h"
#include <SDL.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

static void timed(unsigned i,uint32_t target,void (*original)(CPU *),CPU *cpu) {
    static uint64_t ticks[8]; static unsigned calls[8];
    if (!getenv("SH3_DIAG_WORKER_PARTS")) { original(cpu); return; }
    uint64_t start=SDL_GetPerformanceCounter(); original(cpu);
    uint64_t duration=SDL_GetPerformanceCounter()-start;
    __atomic_fetch_add(&ticks[i],duration,__ATOMIC_RELAXED);
    unsigned count=__atomic_add_fetch(&calls[i],1,__ATOMIC_RELAXED);
    if (!(count%1024u)) {
        uint64_t total=__atomic_load_n(&ticks[i],__ATOMIC_RELAXED), hz=SDL_GetPerformanceFrequency();
        if (hz) fprintf(stderr,"[worker-part] target=%08x calls=%u total_ms=%.2f avg_us=%.2f\n",
                        target,count,(double)total*1000.0/hz,(double)total*1000000.0/hz/count);
    }
}
#define WRAP(addr,slot) \
    extern void __real_F_##addr(CPU *); \
    void __wrap_F_##addr(CPU *cpu) { timed(slot,0x##addr##u,__real_F_##addr,cpu); }
WRAP(00614fe0,0)
WRAP(006176c0,1)
WRAP(00614640,2)
WRAP(00617f70,3)
WRAP(006181d0,4)
WRAP(006183e0,5)
WRAP(006183a0,6)
WRAP(00617ca0,7)
