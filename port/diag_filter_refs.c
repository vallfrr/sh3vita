#include "diag_filter_refs.h"
int diag_filter_insert(DiagFilterRefs *r,uint32_t f) {
    if(!r || !f || r->count>DIAG_FILTER_LIMIT)return -1;
    for(unsigned i=0;i<r->count;i++)if(r->items[i]==f)return 1;
    if(r->count==DIAG_FILTER_LIMIT)return 2;
    r->items[r->count++]=f;return 0;
}
int diag_filter_remove(DiagFilterRefs *r,uint32_t f) {
    if(!r || !f || r->count>DIAG_FILTER_LIMIT)return -1;
    for(unsigned i=0;i<r->count;i++)if(r->items[i]==f) {
        r->items[i]=r->items[--r->count];r->items[r->count]=0;return 1;
    }
    return 0;
}
void diag_filter_drain(DiagFilterRefs *r,DiagFilterRelease release,void *ctx) {
    if(!r || !release || r->count>DIAG_FILTER_LIMIT)return;
    while(r->count) { uint32_t f=r->items[--r->count];r->items[r->count]=0;release(ctx,f); }
}
