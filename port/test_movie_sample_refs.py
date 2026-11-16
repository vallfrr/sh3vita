#!/usr/bin/env python3
"""Exercise the actual movie sample reference helpers with pthread mutexes."""
import os
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
source = (root / "port/sh4base/dx/movie.c").read_text(encoding="utf-8")
start = source.index("static int movie_sample_addref(")
end = source.index("M(sm, AddRef)", start)
shim = r'''
#include <pthread.h>
#include <stdlib.h>
typedef pthread_mutex_t SDL_mutex;
typedef struct { SDL_mutex *lock; } Movie;
typedef struct { struct { int refs; } com; Movie *m; int busy; } Samp;
static int SDL_LockMutex(SDL_mutex *m) { return pthread_mutex_lock(m); }
static int SDL_UnlockMutex(SDL_mutex *m) { return pthread_mutex_unlock(m); }
#define rt_fatal(...) abort()
'''
test = r'''
#include <assert.h>
#include <stdio.h>
static void *worker(void *p) {
    Samp *s=p;
    for (int i=0;i<100000;i++) { movie_sample_addref(s); movie_sample_release(s); }
    return NULL;
}
int main(void) {
    SDL_mutex lock=PTHREAD_MUTEX_INITIALIZER; Movie m={&lock}; Samp s={{1},&m,1};
    assert(movie_sample_addref(&s)==2);
    assert(movie_sample_release(&s)==1 && s.busy==1);
    pthread_t threads[4];
    for (int i=0;i<4;i++) assert(!pthread_create(&threads[i],NULL,worker,&s));
    for (int i=0;i<4;i++) assert(!pthread_join(threads[i],NULL));
    assert(s.com.refs==1 && s.busy==1);
    assert(movie_sample_release(&s)==0 && s.busy==0);
    puts("movie sample ownership test passed"); return 0;
}
'''
with tempfile.TemporaryDirectory(prefix="sh3-movie-sample-") as tmp:
    tmp = Path(tmp)
    unit = tmp / "test.c"
    unit.write_text(shim + source[start:end] + test, encoding="utf-8")
    exe = tmp / "test"
    subprocess.run([os.environ.get("CC", "cc"), "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror",
                    "-pthread", str(unit), "-o", str(exe)], check=True)
    subprocess.run([str(exe)], check=True, timeout=30)
