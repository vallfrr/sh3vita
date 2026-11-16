/* Regression test for complete guard checking, including fragmented heaps. */
#include "diag_heap.h"
#include <assert.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <unistd.h>

static void corrupt(unsigned offset, int fragmented, unsigned victim) {
    size_t bytes=4u*1024u*1024u;
    uint8_t *memory=calloc(1,bytes);
    assert(memory);
    DiagHeap *heap=diag_heap_create(memory,bytes,65536,(uint32_t)bytes-65536);
    assert(heap);
    uint32_t addresses[4000];
    unsigned count=fragmented?4000:1;
    for (unsigned i=0;i<count;i++) {
        addresses[i]=diag_heap_alloc(heap,i%31+1,0);
        assert(addresses[i]);
    }
    if (fragmented) for (unsigned i=0;i<count;i+=3) assert(!diag_heap_free(heap,addresses[i]));
    assert(victim<count && (!fragmented || victim%3));
    memory[addresses[victim]+victim%31+1+offset]^=1;
    (void)diag_heap_alloc(heap,8,0); /* Scanning must abort before allocation. */
    _exit(2);
}
static void expect_abort(unsigned offset,int fragmented,unsigned victim) {
    pid_t child=fork(); assert(child>=0);
    if (!child) {
        FILE *f=freopen("/dev/null","w",stderr); (void)f;
        corrupt(offset,fragmented,victim);
    }
    int status; assert(waitpid(child,&status,0)==child);
    assert(WIFSIGNALED(status) && WTERMSIG(status)==SIGABRT);
}
int main(void) {
    assert(!setenv("SH3_DIAG_HEAPGUARD","1",1));
    for (unsigned offset=0;offset<16;offset++) expect_abort(offset,0,0);
    expect_abort(15,1,1); expect_abort(15,1,1999); expect_abort(15,1,3998);
    puts("guard corruption checks passed across offsets and fragmented heap");
    return 0;
}
