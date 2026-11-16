#!/usr/bin/env python3
"""Exercise the bounded worker EndOfStream notification helper."""
import os
from pathlib import Path
import subprocess
import tempfile
source=(Path(__file__).resolve().parent/'sh4base/dx/movie.c').read_text(encoding='utf-8')
a=source.index('int movie_lab_try_eos_notify(');b=source.index('\n#endif',a)
shim=r'''
#include <assert.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>
typedef struct { uint32_t esp,eax; } CPU;
typedef void (*gfn)(CPU *);
typedef struct { uint32_t sink_g; } Movie;
static __thread Movie *movie_active_eos;
static __thread CPU *movie_active_eos_cpu;
static Movie *g_mv;
static uint8_t memory[64],*g_mem=memory;
static uint32_t g_mem_size=sizeof memory;
#define HR_OK 0u
static uint32_t events[3],count;
static void sink_Notify(CPU *c) { (void)c; }
static void other(CPU *c) { (void)c; }
static void rpush(CPU *c,uint32_t a,uint32_t b,uint32_t d) { (void)c;events[0]=a;events[1]=b;events[2]=d;count++; }
'''
test=r'''
int main(void) {
 Movie m={42},other_movie={42}; CPU c={8,77},other_cpu={8,77}; uint32_t args[]={42,1,2,3}; memcpy(memory+12,args,sizeof args);
 assert(!movie_lab_try_eos_notify(sink_Notify,&c));
 movie_active_eos=&m;movie_active_eos_cpu=&c;g_mv=&m;
 assert(!movie_lab_try_eos_notify(other,&c));assert(!movie_lab_try_eos_notify(sink_Notify,&other_cpu));
 g_mv=&other_movie;assert(!movie_lab_try_eos_notify(sink_Notify,&c));g_mv=&m;
 args[0]=43;memcpy(memory+12,args,sizeof args);assert(!movie_lab_try_eos_notify(sink_Notify,&c));
 args[0]=42;memcpy(memory+12,args,sizeof args);c.esp=48;assert(!movie_lab_try_eos_notify(sink_Notify,&c));c.esp=8;
 assert(movie_lab_try_eos_notify(sink_Notify,&c)&&c.eax==0&&c.esp==28&&count==1);
 assert(events[0]==1&&events[1]==2&&events[2]==3); puts("movie EOS notify ownership passed");
}
'''
with tempfile.TemporaryDirectory(prefix='sh3-movie-eos-') as tmp:
 p=Path(tmp);(p/'test.c').write_text(shim+source[a:b]+test,encoding='utf-8')
 subprocess.run([os.environ.get('CC','cc'),'-std=c11','-Wall','-Wextra','-Werror',str(p/'test.c'),'-o',str(p/'test')],check=True)
 subprocess.run([str(p/'test')],check=True)
