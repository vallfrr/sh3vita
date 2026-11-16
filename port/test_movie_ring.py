#!/usr/bin/env python3
"""Exercise the actual SH3 movie audio ring with pthread-backed SDL mutexes.

This host stress test does not emulate SDL/Vita scheduling or movie playback.
"""
import os
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
source = (root / "port/sh4base/dx/movie.c").read_text(encoding="utf-8")
start = source.index("static float g_ring[")
end = source.index("// ---------- calls into the guest code ----------", start)
shim = r'''
#include <stdint.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <pthread.h>
typedef pthread_mutex_t SDL_mutex;
static SDL_mutex *SDL_CreateMutex(void) {
    SDL_mutex *m=malloc(sizeof *m);
    if (m && pthread_mutex_init(m,0)) { free(m); return NULL; }
    return m;
}
static int SDL_LockMutex(SDL_mutex *m) { return pthread_mutex_lock(m); }
static int SDL_UnlockMutex(SDL_mutex *m) { return pthread_mutex_unlock(m); }
static void SDL_DestroyMutex(SDL_mutex *m) { pthread_mutex_destroy(m); free(m); }
#define rt_fatal(...) abort()
'''
test = r'''
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
void movie_audio_push(const float *,int,int);
void movie_audio_mix(float *,int);
void movie_audio_flush(void);
static void *producer(void *p) {
    (void)p; float samples[256];
    for (int i=0;i<256;i++) samples[i]=0.25f;
    for (int i=0;i<30000;i++) movie_audio_push(samples,128,48000);
    return NULL;
}
static void *consumer(void *p) {
    (void)p; float out[256];
    for (int i=0;i<30000;i++) {
        memset(out,0,sizeof out); movie_audio_mix(out,128);
        for (int j=0;j<256;j++) if (!isfinite(out[j]) || out[j]<0 || out[j]>0.251f) abort();
    }
    return NULL;
}
static void *flusher(void *p) {
    (void)p; for (int i=0;i<20000;i++) movie_audio_flush(); return NULL;
}
int main(void) {
    /* Fill the ring, then keep pushing: discarded input must not leave a
       negative resampling phase and cause an out-of-bounds read next time. */
    float samples[256]; for (int i=0;i<256;i++) samples[i]=0.25f;
    for (int i=0;i<2000;i++) movie_audio_push(samples,128,48000);
    movie_audio_flush();
    pthread_t threads[4];
    if (pthread_create(&threads[0],0,producer,0) || pthread_create(&threads[1],0,consumer,0) ||
        pthread_create(&threads[2],0,flusher,0) || pthread_create(&threads[3],0,flusher,0)) return 1;
    for (int i=0;i<4;i++) if (pthread_join(threads[i],0)) return 1;
    puts("concurrent movie ring stress passed"); return 0;
}
'''
with tempfile.TemporaryDirectory(prefix="sh3-movie-ring-") as tmp:
    tmp = Path(tmp)
    (tmp / "ring.c").write_text(shim + source[start:end], encoding="utf-8")
    (tmp / "test.c").write_text(test, encoding="utf-8")
    exe = tmp / "test"
    subprocess.run([os.environ.get("CC", "cc"), "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror",
                    "-pthread", str(tmp / "ring.c"), str(tmp / "test.c"), "-lm", "-o", str(exe)], check=True)
    subprocess.run([str(exe)], check=True, timeout=30)
