#!/usr/bin/env python3
"""Check host-only opt-in capture from the actual movie audio ring."""
import os,struct,subprocess,tempfile
from pathlib import Path
source=(Path(__file__).resolve().parent/'sh4base/dx/movie.c').read_text(encoding='utf-8')
a=source.index('static float g_ring[');b=source.index('// ---------- calls into the guest code ----------',a)
shim=r'''
#include <stdint.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <stdio.h>
#include <pthread.h>
#define SH3_MOVIE_LAB 1
typedef pthread_mutex_t SDL_mutex;
static SDL_mutex *SDL_CreateMutex(void) { SDL_mutex *m=malloc(sizeof *m); if(m && pthread_mutex_init(m,0)) { free(m);return NULL; } return m; }
static int SDL_LockMutex(SDL_mutex *m) { return pthread_mutex_lock(m); }
static int SDL_UnlockMutex(SDL_mutex *m) { return pthread_mutex_unlock(m); }
static void SDL_DestroyMutex(SDL_mutex *m) { pthread_mutex_destroy(m); free(m); }
#define rt_fatal(...) abort()
#define rt_log(...) ((void)0)
'''
test=r'''
int main(int argc,char **argv) {
 if(argc!=2 || setenv("SH3_DIAG_MOVIE_AUDIO_CAPTURE",argv[1],1)) return 1;
 float samples[]={0.25f,-0.5f,0.5f,-0.25f,0.0f,0.75f,0.125f,-0.125f},out[8]={0};
 movie_audio_push(samples,4,48000); movie_audio_mix(out,4);
 for(int i=0;i<8;i++) if(out[i]!=samples[i]) return 2;
 return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='sh3-audio-capture-') as t:
 p=Path(t);(p/'test.c').write_text(shim+source[a:b]+test,encoding='utf-8')
 subprocess.run([os.environ.get('CC','cc'),'-std=gnu11','-Wall','-Wextra','-Werror','-pthread',str(p/'test.c'),'-o',str(p/'test')],check=True)
 out=p/'audio.raw';subprocess.run([str(p/'test'),str(out)],check=True)
 assert struct.unpack('<8f',out.read_bytes())==(0.25,-0.5,0.5,-0.25,0.0,0.75,0.125,-0.125)
 out.write_bytes(b'preserve');subprocess.run([str(p/'test'),str(out)],check=True)
 assert out.read_bytes()==b'preserve'
 print('movie audio capture output and no-overwrite passed')
