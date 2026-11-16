/* Bound the scratch allocation of the original compressed-archive reader.
 * A full-room read previously needed another contiguous ~9 MB guest block.
 * Retain the actual decoder and file operations, at their native 64 KiB block
 * granularity; do not acknowledge a partial failed map as successful input.
 */
#include "x86rt.h"
#include <stdio.h>
#include <stdlib.h>

extern uint32_t g_mem_size;
extern void diag_archive_read_retained(CPU *c);

static void archive_failure(uint32_t object,uint32_t destination,uint32_t requested,
                            uint32_t done,uint32_t result) {
    fprintf(stderr,"[archive-stream] read failure object=%08x destination=%08x requested=%u completed=%u returned=%u\n",
            object,destination,requested,done,result);
    fflush(stderr);
    exit(36);
}

void F_00608f80(CPU *c) {
    uint8_t *const mb=g_mem;
    uint32_t sp=c->esp,object=c->ecx;
    if (sp>g_mem_size-12u || object>g_mem_size-32u) {
        archive_failure(object,0,0,0,0); return;
    }
    uint32_t destination=R32(sp+4u),requested=R32(sp+8u);
    uint32_t position=R32(object+0x10u),length=R32(object+0x14u);
    /* Preserve the original path for small or unusual/unaligned requests.
       Streaming requests start on native 64 KiB archive block boundaries. */
    if (requested<=0x10000u || (position&0xffffu) || position>=length ||
        requested<length-position ||
        length>0x7fffffffu || requested>0x7fffffffu) {
        diag_archive_read_retained(c); return;
    }
    if (!destination || destination>g_mem_size || requested>g_mem_size-destination) {
        archive_failure(object,destination,requested,0,0); return;
    }
    uint32_t done=0;
    while (done<requested) {
        uint32_t before=R32(object+0x10u);
        if (before>=length) break;
        uint32_t count=requested-done;
        if (count>0x10000u) count=0x10000u;
        W32(sp+4u,destination+done); W32(sp+8u,count);
        c->ecx=object;c->esp=sp;
        diag_archive_read_retained(c);
        uint32_t got=c->eax,after=R32(object+0x10u);
        /* Retained code returns a 2 KiB padded count while the object tracks
           the actual decompressed byte count. A non-final block is 64 KiB. */
        if (!got || got>0x10000u || after<=before || after-before>count ||
            got!=((after-before+0x7ffu)&~0x7ffu) ||
            (got<count && after<length) || got>g_mem_size-destination-done) {
            archive_failure(object,destination,requested,done,got); return;
        }
        done+=got;
    }
    /* The retained implementation zero-extends the caller's whole request at
       EOF, even though its return count only includes the 2 KiB padded data. */
    if (done<requested && R32(object+0x10u)>=length)
        memset(mb+destination+done,0,requested-done);
    c->eax=done;c->esp=sp+12u;
}
