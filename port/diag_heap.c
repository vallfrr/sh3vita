/* Thread-safe bounded guest heap, using sh4vita's BSD-licensed TLSF. GPL-3.0. */
#include "diag_heap.h"
#include "third_party/tlsf/tlsf.h"
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdint.h>
#include <time.h>
#define MAX_BLOCKS 65536u
#define GUARD_BYTES 16u
typedef struct __attribute__((packed,may_alias)) { uint32_t w[4]; } PackedGuard;
struct DiagHeap {
    uint8_t *arena; tlsf_t tlsf; pthread_mutex_t lock;
    struct { uint32_t address,size; } blocks[MAX_BLOCKS];
    unsigned live; int guard, stats;
    uint64_t scan_ns, scan_live; unsigned scans, scan_max_live;
    unsigned failed_allocations;
    unsigned reused_bucket;
};
typedef struct { size_t free_bytes,largest_free,used_bytes; } HeapCapacity;
static void heap_capacity(void *pointer,size_t size,int used,void *opaque) {
    (void)pointer;
    HeapCapacity *capacity=opaque;
    if (used) capacity->used_bytes+=size;
    else { capacity->free_bytes+=size; if(size>capacity->largest_free)capacity->largest_free=size; }
}
void diag_heap_report(DiagHeap *h) {
    if (!h || pthread_mutex_lock(&h->lock)) return;
    HeapCapacity capacity={0};
    tlsf_walk_pool(tlsf_get_pool(h->tlsf),heap_capacity,&capacity);
    fprintf(stderr,"[heap-live] live=%u used=%llu free=%llu largest_free=%llu failures=%u reused_bucket=%u\n",
            h->live,(unsigned long long)capacity.used_bytes,(unsigned long long)capacity.free_bytes,
            (unsigned long long)capacity.largest_free,h->failed_allocations,h->reused_bucket);
    pthread_mutex_unlock(&h->lock);
}
static unsigned char guard_byte(uint32_t addr,unsigned i) { return (unsigned char)(0xa5u ^ (addr>>(8*(i%4))) ^ i); }
static void check_guards(DiagHeap *h,const char *op) {
    if (!h->guard) return;
    struct timespec start, end;
    int timed=h->stats && !clock_gettime(CLOCK_MONOTONIC,&start);
    /* Entries are kept dense under the heap lock. Check every live guard,
       without repeatedly walking holes left by freed allocations. */
    for (unsigned i=0;i<h->live;i++) {
        uint32_t a=h->blocks[i].address,n=h->blocks[i].size;
        uint32_t x=a^0xa6a7a4a5u;
#if __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
        x=__builtin_bswap32(x);
#endif
        const PackedGuard *p=(const PackedGuard *)(h->arena+a+n);
        int bad=(p->w[0]^x)|(p->w[1]^(x^0x04040404u))|(p->w[2]^(x^0x08080808u))|(p->w[3]^(x^0x0c0c0c0cu));
        if (bad) {
            for (unsigned j=0;j<GUARD_BYTES;j++) if (h->arena[a+n+j]!=guard_byte(a,j)) {
                fprintf(stderr,"[diag] heap guard corrupted before %s: block=%08x size=%u guard_offset=%u value=%02x\n",op,a,n,j,h->arena[a+n+j]); fflush(stderr); abort();
            }
        }
    }
    if (timed && !clock_gettime(CLOCK_MONOTONIC,&end)) {
        int64_t ns=(int64_t)(end.tv_sec-start.tv_sec)*1000000000LL+end.tv_nsec-start.tv_nsec;
        if (ns>=0) h->scan_ns+=(uint64_t)ns;
        h->scan_live+=h->live;
        if (h->live>h->scan_max_live) h->scan_max_live=h->live;
        if (++h->scans==1024) {
            fprintf(stderr,"[heap-stats] scans=%u total_ms=%.2f avg_live=%.1f max_live=%u\n",
                    h->scans,(double)h->scan_ns/1000000.0,(double)h->scan_live/h->scans,h->scan_max_live);
            h->scan_ns=h->scan_live=0; h->scans=h->scan_max_live=0;
        }
    }
}
DiagHeap *diag_heap_create(uint8_t *arena,size_t arena_size,uint32_t base,uint32_t length) {
    if (!arena || !base || base>arena_size || length>arena_size-base ||
        ((uintptr_t)(arena+base)%tlsf_align_size()) || length<tlsf_size()+tlsf_pool_overhead()+tlsf_block_size_min()) return NULL;
    DiagHeap *h=calloc(1,sizeof *h); if (!h) return NULL;
    if (pthread_mutex_init(&h->lock,NULL)) { free(h); return NULL; }
    h->arena=arena; h->guard=getenv("SH3_DIAG_HEAPGUARD")!=NULL;
    h->stats=getenv("SH3_DIAG_HEAPSTATS")!=NULL; h->tlsf=tlsf_create_with_pool(arena+base,length);
    if (!h->tlsf) { pthread_mutex_destroy(&h->lock); free(h); return NULL; }
    return h;
}
uint32_t diag_heap_alloc(DiagHeap *h,uint32_t size,int zero) {
    if (!h || pthread_mutex_lock(&h->lock)) return 0;
    check_guards(h,"allocation");
    unsigned i=h->live;
    size_t requested=size?size:1;
    if (h->guard) {
        if (requested>SIZE_MAX-GUARD_BYTES) { pthread_mutex_unlock(&h->lock); return 0; }
        requested+=GUARD_BYTES;
    }
    void *p=i<MAX_BLOCKS?tlsf_malloc(h->tlsf,requested):NULL;
    if (!p && i<MAX_BLOCKS && (p=tlsf_malloc_reuse(h->tlsf,requested))) ++h->reused_bucket;
    uint32_t addr=0;
    if (p) { if(zero) memset(p,0,size); addr=(uint32_t)((uint8_t *)p-h->arena); h->blocks[i].address=addr; h->blocks[i].size=size; h->live++;
        if (h->guard) for (unsigned j=0;j<GUARD_BYTES;j++) h->arena[addr+size+j]=guard_byte(addr,j);
        if (h->guard && size<=4 && getenv("SH3_DIAG_TINYLOG")) fprintf(stderr,"[diag] tiny allocation addr=%08x size=%u zero=%d host_caller=%p\n",addr,size,zero,__builtin_return_address(0));
    }
    else if (++h->failed_allocations<=32) {
        HeapCapacity capacity={0};
        tlsf_walk_pool(tlsf_get_pool(h->tlsf),heap_capacity,&capacity);
        fprintf(stderr,"[heap-failure] request=%u actual=%llu live=%u used=%llu free=%llu largest_free=%llu count=%u\n",
                size,(unsigned long long)requested,h->live,(unsigned long long)capacity.used_bytes,
                (unsigned long long)capacity.free_bytes,(unsigned long long)capacity.largest_free,h->failed_allocations);
        fflush(stderr);
    }
    pthread_mutex_unlock(&h->lock); return addr;
}
int diag_heap_free(DiagHeap *h,uint32_t address) {
    if (!h || !address || pthread_mutex_lock(&h->lock)) return -1;
    check_guards(h,"free");
    unsigned i=0; while(i<h->live && h->blocks[i].address!=address) i++;
    if(i==h->live) { pthread_mutex_unlock(&h->lock); return -1; }
    h->live--; h->blocks[i]=h->blocks[h->live];
    h->blocks[h->live].address=0; h->blocks[h->live].size=0;
    tlsf_free(h->tlsf,h->arena+address);
    pthread_mutex_unlock(&h->lock); return 0;
}
int diag_heap_size(DiagHeap *h,uint32_t address,uint32_t *size) {
    if (!h || !address || !size || pthread_mutex_lock(&h->lock)) return -1;
    unsigned i=0; while(i<h->live && h->blocks[i].address!=address) i++;
    if(i==h->live) { pthread_mutex_unlock(&h->lock); return -1; }
    *size=h->blocks[i].size; pthread_mutex_unlock(&h->lock); return 0;
}
int diag_heap_destroy(DiagHeap *h) {
    if (!h || pthread_mutex_lock(&h->lock)) return -1;
    if(h->live) { pthread_mutex_unlock(&h->lock); return -1; }
    pthread_mutex_unlock(&h->lock); pthread_mutex_destroy(&h->lock); free(h); return 0;
}
