#include "diag_taskmem.h"
#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
typedef struct { DiagTaskMem *m; } Ctx;
static void *worker(void *p) { Ctx *c=p;for(unsigned i=0;i<1000;i++){uint32_t a=diag_taskmem_alloc(c->m,i%128);assert(a);assert(!diag_taskmem_free(c->m,a));}return NULL; }
int main(void) {
    unsigned n=4u*1024*1024;uint8_t *mem=calloc(1,n);assert(mem);
    DiagHeap *h=diag_heap_create(mem,n,65536,n-65536);assert(h);
    DiagTaskMem *m=diag_taskmem_create(h);assert(m);
    uint32_t p=diag_taskmem_alloc(m,19),other=diag_heap_alloc(h,10,0);assert(p && other);
    assert(diag_taskmem_destroy(m)==-1);assert(diag_taskmem_free(m,other)==-1);
    assert(!diag_taskmem_free(m,p));assert(diag_taskmem_free(m,p)==-1);assert(!diag_taskmem_free(m,0));assert(!diag_heap_free(h,other));
    pthread_t t[4];Ctx c={m};for(unsigned i=0;i<4;i++)assert(!pthread_create(&t[i],NULL,worker,&c));
    for(unsigned i=0;i<4;i++)assert(!pthread_join(t[i],NULL));
    assert(!diag_taskmem_destroy(m));assert(!diag_heap_destroy(h));free(mem);puts("COM task allocation ownership and concurrency passed");
}
