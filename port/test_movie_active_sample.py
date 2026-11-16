#!/usr/bin/env python3
"""Exercise the movie lab's worker-owned sample guard, including rejection."""
import os
from pathlib import Path
import subprocess
import tempfile

source = (Path(__file__).resolve().parent / 'sh4base/dx/movie.c').read_text(encoding='utf-8')
start = source.index('int movie_lab_try_active_sample(')
end = source.index('\n#endif', start)
shim = r'''
#include <stdint.h>
#include <stdatomic.h>
#include <string.h>
typedef struct { uint32_t eax, esp; } CPU;
typedef void (*gfn)(CPU *);
typedef struct { void *lock; } Movie;
typedef struct { struct { uint32_t guest; int refs; } com; Movie *m; uint32_t buf,size,len; int busy; int64_t t0,t1; int have_time; _Atomic int sync; } Samp;
static void SDL_LockMutex(void *p) { (void)p; }
static void SDL_UnlockMutex(void *p) { (void)p; }
static __thread Samp *movie_active_sample;
static __thread CPU *movie_active_sample_cpu;
static uint8_t memory[64];
uint8_t *g_mem=memory;
uint32_t g_mem_size=sizeof memory;
#define HR_OK 0u
#define S_FALSE_ 1u
static void sm_IsSync(CPU *c) { (void)c; }
static void sm_GetTime(CPU *c) { (void)c; }
static void sm_GetMT(CPU *c) { (void)c; }
static void sm_GetPointer(CPU *c) { (void)c; }
static void sm_GetLen(CPU *c) { (void)c; }
static void sm_GetSize(CPU *c) { (void)c; }
static void sm_AddRef(CPU *c) { (void)c; }
static void sm_Release(CPU *c) { (void)c; }
static void other(CPU *c) { (void)c; }
'''
test = r'''
#include <assert.h>
#include <pthread.h>
#include <stdio.h>
static void *run(void *arg) {
    (void)arg;
    CPU c={123,8}, other_cpu={123,8}; Movie movie={0}; Samp sample={.com={42,1},.m=&movie,.buf=40,.size=8,.busy=1,.t0=123456789,.t1=987654321,.have_time=1,.sync=1}; uint32_t object=42;
    memcpy(memory+12,&object,4);
    assert(!movie_lab_try_active_sample(sm_IsSync,&c));
    movie_active_sample=&sample; movie_active_sample_cpu=&c;
    assert(!movie_lab_try_active_sample(other,&c));
    assert(!movie_lab_try_active_sample(sm_IsSync,&other_cpu));
    object=43; memcpy(memory+12,&object,4);
    assert(!movie_lab_try_active_sample(sm_IsSync,&c) && c.esp==8 && c.eax==123);
    object=42; memcpy(memory+12,&object,4);
    c.esp=60; assert(!movie_lab_try_active_sample(sm_IsSync,&c)); c.esp=8;
    assert(movie_lab_try_active_sample(sm_IsSync,&c) && c.eax==0 && c.esp==16);
    c.esp=8; atomic_store(&sample.sync,0);
    assert(movie_lab_try_active_sample(sm_IsSync,&c) && c.eax==1 && c.esp==16);
    uint32_t p1=32,p2=48; memcpy(memory+16,&p1,4); memcpy(memory+20,&p2,4);
    c.esp=8; assert(movie_lab_try_active_sample(sm_GetTime,&c)==2 && c.eax==0 && c.esp==24);
    int64_t t0,t1; memcpy(&t0,memory+32,8); memcpy(&t1,memory+48,8);
    assert(t0==123456789 && t1==987654321);
    p2=60; memcpy(memory+20,&p2,4); c.esp=8; c.eax=123;
    assert(!movie_lab_try_active_sample(sm_GetTime,&c) && c.esp==8 && c.eax==123);
    p2=48; memcpy(memory+20,&p2,4); sample.have_time=0; c.esp=8;
    assert(movie_lab_try_active_sample(sm_GetTime,&c)==2 && c.eax==0x80040249u && c.esp==24);
    uint32_t nonzero=77; memcpy(memory+32,&nonzero,4); p1=32; memcpy(memory+16,&p1,4);
    c.esp=8; assert(movie_lab_try_active_sample(sm_GetMT,&c)==3 && c.eax==1 && c.esp==20);
    memcpy(&nonzero,memory+32,4); assert(nonzero==0);
    p1=61; memcpy(memory+16,&p1,4); c.esp=8; c.eax=123;
    assert(!movie_lab_try_active_sample(sm_GetMT,&c) && c.esp==8 && c.eax==123);
    p1=32; memcpy(memory+16,&p1,4); c.esp=8;
    assert(movie_lab_try_active_sample(sm_GetPointer,&c)==4 && c.eax==0 && c.esp==20);
    memcpy(&nonzero,memory+32,4); assert(nonzero==40);
    sample.busy=0; c.esp=8; c.eax=123;
    assert(!movie_lab_try_active_sample(sm_GetPointer,&c) && c.esp==8 && c.eax==123);
    sample.busy=1; sample.buf=60; c.esp=8;
    assert(!movie_lab_try_active_sample(sm_GetPointer,&c) && c.esp==8);
    sample.buf=40; sample.len=7; sample.busy=1; c.esp=8;
    assert(movie_lab_try_active_sample(sm_GetLen,&c)==5 && c.eax==7 && c.esp==16);
    sample.busy=0; c.esp=8; c.eax=123;
    assert(!movie_lab_try_active_sample(sm_GetLen,&c) && c.eax==123 && c.esp==8);
    sample.busy=1; sample.size=8; c.esp=8;
    assert(movie_lab_try_active_sample(sm_GetSize,&c)==6 && c.eax==8 && c.esp==16);
    sample.busy=0; c.esp=8; c.eax=123;
    assert(!movie_lab_try_active_sample(sm_GetSize,&c) && c.eax==123 && c.esp==8);
    sample.busy=1; sample.com.refs=1; c.esp=8;
    assert(movie_lab_try_active_sample(sm_AddRef,&c)==7 && c.eax==2 && c.esp==16);
    c.esp=8; assert(movie_lab_try_active_sample(sm_Release,&c)==8 && c.eax==1 && c.esp==16);
    c.esp=8; c.eax=123;
    assert(!movie_lab_try_active_sample(sm_Release,&c) && c.eax==123 && c.esp==8 && sample.com.refs==1);
    movie_active_sample=NULL; movie_active_sample_cpu=NULL;
    c.esp=8; assert(!movie_lab_try_active_sample(sm_IsSync,&c));
    return NULL;
}
int main(void) { pthread_t t; assert(!pthread_create(&t,NULL,run,NULL));
    assert(!pthread_join(t,NULL)); puts("movie active sample guard test passed"); }
'''
with tempfile.TemporaryDirectory(prefix='sh3-movie-active-') as tmp:
    unit = Path(tmp) / 'test.c'
    binary = Path(tmp) / 'test'
    unit.write_text(shim + source[start:end] + test, encoding='utf-8')
    subprocess.run([os.environ.get('CC', 'cc'), '-std=c11', '-Wall', '-Wextra',
                    '-Werror', '-pthread', str(unit), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
