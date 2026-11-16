#!/usr/bin/env python3
"""Exercise real DirectSound staging methods with guarded guest heap/pthreads."""
from pathlib import Path
import subprocess
import tempfile

root=Path(__file__).resolve().parents[1]
source=(root/'port/sh4base/dx/dsound.c').read_text()


def function(signature):
    a=source.index(signature)
    return source[a:source.index('\n}',a)+2]


a=source.index('typedef struct DSBuf {')
struct=source[a:source.index('} DSBuf;',a)+8]
code=r'''
#include "x86rt.h"
#include "diag_heap.h"
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#define rt_log(...) ((void)0)
uint8_t *g_mem;
uint64_t g_mem_ds;
typedef struct {uint32_t guest;int32_t refs;int kind;} ComObj;
'''+struct+r'''
#define M(p,name) static void p##_##name(CPU *c)
#define HOSTFN uint8_t *const mb=g_mem;(void)mb
#define ARG(n) R32(c->esp+4u*(n))
#define STDRET(n,v) do{c->eax=(uint32_t)(v);c->esp+=4u+4u*(n);return;}while(0)
#define HR_OK 0u
#define HR_FAIL 0x80004005u
#define D3DERR_INVALIDCALL 0x8876086cu
#define K_DSBUF 1
#define BUF DSBuf *b=com_get_kind(ARG(1),K_DSBUF);if(!b)STDRET(1,HR_FAIL)
static pthread_mutex_t mutex=PTHREAD_MUTEX_INITIALIZER,*mx=&mutex;
#define SDL_LockMutex pthread_mutex_lock
#define SDL_UnlockMutex pthread_mutex_unlock
static DiagHeap *heap;
static unsigned allocations,frees,fail_next;
static uint32_t gheap_alloc(uint32_t n){allocations++;if(fail_next){fail_next=0;return 0;}return diag_heap_alloc(heap,n,0);}
static void gheap_free(uint32_t p){assert(!diag_heap_free(heap,p));frees++;}
static void rt_fatal(const char *s){fprintf(stderr,"%s\n",s);abort();}
static DSBuf *map[16],*bufs;
static unsigned next_id=1;
static const gfn buf_vt[24]={0};
static void *com_get_kind(uint32_t g,int kind){(void)kind;return g<16?map[g]:NULL;}
static uint32_t com_wrap(ComObj *o,int k,const gfn *v,int n,const char *s){
 (void)v;(void)n;(void)s;assert(next_id<16);o->guest=next_id;o->refs=1;o->kind=k;
 map[next_id]=(DSBuf *)o;return next_id++;
}
static void com_free(ComObj *o){map[o->guest]=NULL;}
'''+function('static DSBuf *buf_new(')+'\n'+function('static void buf_link(')+'\n'+function('static uint32_t buf_release_ref(DSBuf *b) {')+'\n'+function('M(buf, Lock) {')+'\n'+function('M(buf, Unlock) {')+'\n'+function('M(ds, DuplicateSoundBuffer) {')+r'''
static DSBuf *make_buffer(uint32_t n) {
 DSBuf *b=buf_new();assert(b);b->size=n;b->mem=gheap_alloc(n);assert(b->mem);
 memset(g_mem+b->mem,0,n);b->shared_refs=malloc(sizeof(int));assert(b->shared_refs);*b->shared_refs=1;
 com_wrap(&b->com,K_DSBUF,buf_vt,24,"test");buf_link(b);g_mem_ds+=n;return b;
}
static uint32_t lock_buffer(DSBuf *b,uint32_t off,uint32_t n) {
 CPU c={.esp=0x20000};uint8_t *const mb=g_mem;
 uint32_t a[9]={0,b->com.guest,off,n,0x21000,0x21004,0x21008,0x2100c,0};
 memcpy(g_mem+c.esp,a,sizeof a);buf_Lock(&c);assert(c.esp==0x20024);return c.eax;
}
static uint32_t unlock_buffer(DSBuf *b,int wrong) {
 CPU c={.esp=0x20000};
 uint32_t n=b->lock_size,n1=b->lock_first,p=b->lock_copy;
 uint32_t a[6]={0,b->com.guest,p+(unsigned)wrong,n1,n>n1?p+n1:0,n-n1};
 memcpy(g_mem+c.esp,a,sizeof a);buf_Unlock(&c);assert(c.esp==0x20018);return c.eax;
}
static unsigned done;
static DSBuf *stress_buffer;
static void *mixer(void *arg) {
 (void)arg;
 do {
  SDL_LockMutex(mx);uint8_t *p=g_mem+stress_buffer->mem;
  for(unsigned i=1;i<stress_buffer->size;i++)assert(p[i]==p[0]);
  SDL_UnlockMutex(mx);
 }while(!__atomic_load_n(&done,__ATOMIC_ACQUIRE));return NULL;
}
int main(int argc,char **argv) {
 assert(argc==2);int cached=atoi(argv[1]);
 if(cached)setenv("SH3_DIAG_AUDIO_LOCK_CACHE","1",1);
 setenv("SH3_DIAG_HEAPGUARD","1",1);
 g_mem=calloc(1,4u*1024*1024);assert(g_mem);
 uint8_t *const mb=g_mem;
 heap=diag_heap_create(g_mem,4u*1024*1024,0x100000,2u*1024*1024);assert(heap);
 DSBuf *b=make_buffer(32768);
 assert(!lock_buffer(b,32760,32));assert(b->lock_first==8);
 assert(R32(0x21000)==b->lock_copy && R32(0x21008)==b->lock_copy+8);
 memset(g_mem+b->lock_copy,0xa7,32);
 assert(g_mem[b->mem+32760]==0 && g_mem[b->mem]==0);
 assert(lock_buffer(b,0,32)==D3DERR_INVALIDCALL);
 assert(unlock_buffer(b,1)==D3DERR_INVALIDCALL && b->lock_copy);
 assert(!unlock_buffer(b,0));
 for(unsigned i=0;i<8;i++)assert(g_mem[b->mem+32760+i]==0xa7);
 for(unsigned i=0;i<24;i++)assert(g_mem[b->mem+i]==0xa7);
 assert(!b->lock_copy && (cached?b->lock_capacity==32:!b->lock_cache));
 assert(!lock_buffer(b,0,1024));assert(!unlock_buffer(b,0));
 uint32_t saved_cache=b->lock_cache;
 fail_next=1;assert(lock_buffer(b,0,2048)==0x8007000eu);
 assert(!b->lock_copy && b->lock_cache==saved_cache);
 assert(!lock_buffer(b,0,2048));assert(!unlock_buffer(b,0));
 uint32_t retained=b->lock_cache;
 assert(!lock_buffer(b,0,20000));assert(b->lock_copy!=retained);assert(!unlock_buffer(b,0));
 assert(b->lock_cache==retained && b->lock_capacity<=16384);
 CPU c={.esp=0x20000};uint32_t a[4]={0,0,b->com.guest,0x21020};
 memcpy(g_mem+c.esp,a,sizeof a);ds_DuplicateSoundBuffer(&c);assert(!c.eax);
 DSBuf *dup=com_get_kind(R32(0x21020),K_DSBUF);assert(dup && !dup->lock_cache && !dup->lock_capacity);
 assert(!lock_buffer(dup,0,1024));assert(dup->lock_copy!=b->lock_cache);assert(!unlock_buffer(dup,0));
 assert(!buf_release_ref(dup));
 stress_buffer=make_buffer(1024);pthread_t thread;assert(!pthread_create(&thread,NULL,mixer,NULL));
 unsigned before=allocations;
 for(unsigned i=0;i<2000;i++) {
  assert(!lock_buffer(stress_buffer,0,1024));
  memset(g_mem+stress_buffer->lock_copy,(int)(i&255),1024);
  assert(!unlock_buffer(stress_buffer,0));
 }
 __atomic_store_n(&done,1,__ATOMIC_RELEASE);assert(!pthread_join(thread,NULL));
 assert(allocations-before==(cached?1u:2000u));
 assert(!buf_release_ref(stress_buffer));assert(!buf_release_ref(b));
 assert(!bufs && !g_mem_ds && !diag_heap_destroy(heap));free(g_mem);
 printf("DirectSound staging %s: wrap/ownership/OOM/mixer/2000 updates passed; allocations=%u frees=%u\n",cached?"cached":"original",allocations,frees);
}
'''
with tempfile.TemporaryDirectory(prefix='sh3-dsound-') as temp:
    c=Path(temp)/'test.c';exe=Path(temp)/'test';c.write_text(code,encoding='utf-8')
    subprocess.run(['gcc','-std=gnu11','-O2','-Wall','-Wextra','-Werror','-Wno-unused-variable','-fsanitize=address,undefined','-pthread',
                    '-I'+str(root/'port/include'),'-I'+str(root/'port'),str(c),str(root/'port/diag_heap.c'),
                    str(root/'port/third_party/tlsf/tlsf.c'),'-o',str(exe)],check=True)
    for mode in ('0','1'):
        subprocess.run([str(exe),mode],check=True)
