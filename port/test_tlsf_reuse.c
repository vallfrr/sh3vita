#include "diag_heap.h"
#include "third_party/tlsf/tlsf.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Reproduce a false OOM: the containing bucket has an exact fitting block,
   but the normal constant-time lookup rounds past it. */
int main(void) {
    const size_t bytes=4*1024*1024, texture=1024*1024, guarded=texture+16;
    unsigned char *arena=calloc(1,bytes);assert(arena);
    tlsf_t t=tlsf_create_with_pool(arena,bytes);assert(t);
    void *p=tlsf_malloc(t,guarded),*held[2048];unsigned n=0;assert(p);
    while(n<2048 && (held[n]=tlsf_malloc(t,4096))) {memset(held[n],0x6b,4096);++n;}
    assert(n>0 && n<2048);tlsf_free(t,p);
    assert(tlsf_malloc(t,guarded)==NULL);
    assert(tlsf_malloc_reuse(t,guarded)==p);
    memset(p,0xa5,guarded);
    assert(!tlsf_check(t));
    for(unsigned i=0;i<n;i++){for(unsigned j=0;j<4096;j++)assert(((unsigned char *)held[i])[j]==0x6b);tlsf_free(t,held[i]);}
    tlsf_free(t,p);assert(!tlsf_check(t));tlsf_destroy(t);free(arena);

    /* Real guest heap + its guard checks: reuse the same large hole without
       dropping guards, moving live allocations, or enlarging the arena. */
    assert(!setenv("SH3_DIAG_HEAPGUARD","1",1));
    arena=calloc(1,bytes);assert(arena);
    DiagHeap *h=diag_heap_create(arena,bytes,64,(uint32_t)bytes-64);assert(h);
    uint32_t large=diag_heap_alloc(h,(uint32_t)texture,0),a[2048];assert(large);n=0;
    while(n<2048 && (a[n]=diag_heap_alloc(h,4096,0)))++n;
    assert(n>0 && n<2048);assert(!diag_heap_free(h,large));
    assert(diag_heap_alloc(h,(uint32_t)texture,0)==large);
    memset(arena+large,0x3c,texture);diag_heap_report(h);
    assert(!diag_heap_free(h,large));
    for(unsigned i=0;i<n;i++)assert(!diag_heap_free(h,a[i]));
    assert(!diag_heap_destroy(h));free(arena);
    puts("TLSF skipped-bucket recovery and guarded guest-heap regression passed");
}
