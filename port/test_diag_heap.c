#include "diag_heap.h"
#include <assert.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef struct { DiagHeap *h; uint8_t *mem; unsigned seed; } Ctx;
static void *work(void *opaque) {
    Ctx *c=opaque; unsigned seed=c->seed;
    for (unsigned i=0;i<1500;i++) {
        uint32_t n=(rand_r(&seed)%2048)+1, a=diag_heap_alloc(c->h,n,1), actual=0; assert(a);
        for(uint32_t j=0;j<n;j++) assert(c->mem[a+j]==0);
        memset(c->mem+a,(int)c->seed,n); assert(!diag_heap_size(c->h,a,&actual) && actual==n);
        assert(!diag_heap_free(c->h,a));
    }
    return NULL;
}
int main(void) {
    const uint32_t n=4u*1024*1024; uint8_t *mem=calloc(1,n); assert(mem);
    assert(!diag_heap_create(mem,n,n-1,100));
    DiagHeap *h=diag_heap_create(mem,n,65536,n-65536); assert(h);
    uint32_t a=diag_heap_alloc(h,17,1), size=0; assert(a && !diag_heap_size(h,a,&size) && size==17);
    assert(diag_heap_destroy(h)==-1); assert(!diag_heap_free(h,a)); assert(diag_heap_free(h,a)==-1);
    /* Removing interior entries must preserve lookup and freeing of entries
       moved into those positions, even when allocations are interleaved. */
    uint32_t addresses[128];
    for (unsigned i=0;i<128;i++) { addresses[i]=diag_heap_alloc(h,i+1,0); assert(addresses[i]); }
    for (unsigned i=0;i<128;i+=2) assert(!diag_heap_free(h,addresses[i]));
    for (unsigned i=1;i<128;i+=2) {
        assert(!diag_heap_size(h,addresses[i],&size) && size==i+1);
        assert(!diag_heap_free(h,addresses[i]));
    }
    pthread_t th[4]; Ctx c[4];
    for(unsigned i=0;i<4;i++){ c[i]=(Ctx){h,mem,i+1}; assert(!pthread_create(&th[i],NULL,work,&c[i])); }
    for(unsigned i=0;i<4;i++) assert(!pthread_join(th[i],NULL));
    assert(!diag_heap_destroy(h)); free(mem); puts("bounded heap concurrency checks passed");
}
