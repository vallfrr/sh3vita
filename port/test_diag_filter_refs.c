#include "diag_filter_refs.h"
#include <assert.h>
#include <stdio.h>
struct counts { unsigned release[9]; };
static void release(void *ctx,uint32_t f) { struct counts *c=ctx;assert(f>=1 && f<=8);c->release[f]++; }
int main(void) {
    DiagFilterRefs r={0};struct counts c={0};
    assert(diag_filter_insert(&r,0)==-1);
    for(unsigned i=1;i<=8;i++)assert(diag_filter_insert(&r,i)==0);
    assert(diag_filter_insert(&r,3)==1);assert(diag_filter_insert(&r,9)==2);
    assert(diag_filter_remove(&r,4)==1);release(&c,4);
    assert(diag_filter_remove(&r,4)==0);assert(diag_filter_insert(&r,4)==0);
    diag_filter_drain(&r,release,&c);assert(!r.count);
    for(unsigned i=1;i<=8;i++)assert(c.release[i]==(i==4?2u:1u));
    diag_filter_drain(&r,release,&c);assert(!r.count);
    puts("filter membership and release accounting passed");
}
