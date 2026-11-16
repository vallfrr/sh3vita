#!/usr/bin/env python3
"""Exercise the movie laboratory allocator's real pool-locking body."""
import os
from pathlib import Path
import subprocess
import tempfile

source=(Path(__file__).resolve().parent/'sh4base/dx/movie.c').read_text(encoding='utf-8')
a=source.index('M(al, SetProperties) {')
b=source.index('\nM(al, GetProperties)',a)
shim=r'''
#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#define SH3_MOVIE_LAB 1
#define NSAMP 3
#define HR_FAIL 0x80004005u
#define HR_OK 0u
#define MK_SAMP 1
typedef struct Movie Movie;
typedef struct { int refs; uint32_t guest; } ComObj;
typedef struct Samp { ComObj com; Movie *m; uint32_t buf,size; int busy; } Samp;
struct Movie { pthread_mutex_t *lock; Samp *samp[NSAMP]; uint32_t buf_size; };
typedef struct { Movie *movie; uint32_t args[3], result; } CPU;
#define M(p,n) static void p##_##n(CPU *c)
#define HOSTFN ((void)0)
#define MV Movie *m=c->movie
#define ARG(n) (c->args[(n)-1])
#define STDRET(n,v) do { c->result=(v); return; } while(0)
static uint32_t mem[64];
#define R32(p) (mem[(p)/4])
#define W32(p,v) (mem[(p)/4]=(v))
#define SDL_LockMutex(p) assert(!pthread_mutex_lock(p))
#define SDL_UnlockMutex(p) assert(!pthread_mutex_unlock(p))
static int sm_vt[1];
static void com_wrap(ComObj *o,int k,void *v,int n,const char *name) { (void)k;(void)v;(void)n;(void)name;o->guest=100; }
static _Atomic int block_reserve, entered_reserve, release_reserve, reserve_calls, fail_reserve;
static int movie_samples_reserve(uint32_t *buf,uint32_t *cap,unsigned n,uint32_t size) {
    atomic_fetch_add(&reserve_calls,1);
    if (atomic_load(&block_reserve)) { atomic_store(&entered_reserve,1); while(!atomic_load(&release_reserve)) sched_yield(); }
    if (atomic_load(&fail_reserve) || !size) return 0;
    for(unsigned i=0;i<n;i++){buf[i]=1000+i;cap[i]=size;} return 1;
}
'''
test=r'''
static void *run(void *p) { al_SetProperties((CPU*)p); return NULL; }
int main(void) {
    pthread_mutex_t ma=PTHREAD_MUTEX_INITIALIZER,mb=PTHREAD_MUTEX_INITIALIZER;
    Movie a={.lock=&ma},b={.lock=&mb}; CPU ca={.movie=&a,.args={0,4,0}},cb={.movie=&b,.args={0,4,0}};
    mem[2]=256;
    al_SetProperties(&ca); assert(ca.result==HR_OK && a.samp[0] && a.samp[0]->buf==1000);
    a.samp[0]->busy=1; int before=reserve_calls;
    al_SetProperties(&cb); assert(cb.result==HR_FAIL && reserve_calls==before && !b.samp[0]);
    assert(!pthread_mutex_trylock(&ma)); assert(!pthread_mutex_unlock(&ma));
    assert(!pthread_mutex_trylock(&mb)); assert(!pthread_mutex_unlock(&mb));
    a.samp[0]->busy=0; atomic_store(&block_reserve,1); pthread_t t;
    assert(!pthread_create(&t,NULL,run,&cb)); while(!atomic_load(&entered_reserve)) sched_yield();
    assert(pthread_mutex_trylock(&ma)==EBUSY && pthread_mutex_trylock(&mb)==EBUSY);
    atomic_store(&release_reserve,1); assert(!pthread_join(t,NULL)); assert(cb.result==HR_OK && b.samp[0]);
    atomic_store(&block_reserve,0); atomic_store(&fail_reserve,1); cb.result=0;
    al_SetProperties(&cb); assert(cb.result==0x8007000eu);
    assert(!pthread_mutex_trylock(&mb)); assert(!pthread_mutex_unlock(&mb));
    for(int i=0;i<NSAMP;i++){free(a.samp[i]);free(b.samp[i]);}
    puts("movie shared sample pool locking passed"); return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='sh3-movie-pool-') as tmp:
    path=Path(tmp);(path/'test.c').write_text(shim+source[a:b]+test,encoding='utf-8')
    subprocess.run([os.environ.get('CC','cc'),'-std=c11','-Wall','-Wextra','-Werror','-pthread',str(path/'test.c'),'-o',str(path/'test')],check=True)
    subprocess.run([str(path/'test')],check=True)
