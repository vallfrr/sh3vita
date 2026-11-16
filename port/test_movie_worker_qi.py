#!/usr/bin/env python3
"""Exercise the actual stateless movie QI stub concurrently on guest CPUs."""
import os
from pathlib import Path
import subprocess
import tempfile

source = (Path(__file__).resolve().parent / 'sh4base/dx/movie.c').read_text(encoding='utf-8')
start = source.index('M(any, QI)')
end = source.index('M(any, AddRef)', start)
sample_start = source.index('M(sm, False1)')
sample_end = source.index('M(sm, Ok2)', sample_start)
shim = r'''
#include <stdint.h>
#define SH3_MOVIE_LAB 1
typedef struct { uint32_t eax, esp; } CPU;
typedef void (*gfn)(CPU *);
#define M(a,b) static void a##_##b(CPU *c)
#define STDRET(n,v) do { c->eax=(uint32_t)(v); c->esp += 4u+4u*(n); return; } while(0)
#define E_NOINTERFACE_ 0x80004002u
#define S_FALSE_ 1u
'''
test = r'''
#include <assert.h>
#include <pthread.h>
#include <stdio.h>
static void other(CPU *c) { c->eax=1; }
static void *run(void *arg) {
    (void)arg;
    for (unsigned i=0; i<100000; ++i) {
        CPU c={0x12345678u,0x1000u};
        any_QI(&c);
        assert(c.eax==0x80004002u && c.esp==0x1010u);
        c.esp=0x1000u; sm_False1(&c);
        assert(c.eax==1u && c.esp==0x1008u);
    }
    return NULL;
}
int main(void) {
    assert(movie_lab_is_pure_worker_stub(any_QI));
    assert(movie_lab_is_pure_worker_stub(sm_False1));
    assert(!movie_lab_is_pure_worker_stub(other));
    assert(!movie_lab_is_pure_worker_stub(NULL));
    pthread_t t[4];
    for (unsigned i=0;i<4;i++) assert(!pthread_create(&t[i],NULL,run,NULL));
    for (unsigned i=0;i<4;i++) assert(!pthread_join(t[i],NULL));
    puts("movie worker QI stub test passed");
}
'''
with tempfile.TemporaryDirectory(prefix='sh3-movie-qi-') as tmp:
    unit = Path(tmp) / 'test.c'
    binary = Path(tmp) / 'test'
    unit.write_text(shim + source[start:end] + source[sample_start:sample_end] + test, encoding='utf-8')
    subprocess.run([os.environ.get('CC', 'cc'), '-std=c11', '-Wall', '-Wextra',
                    '-Werror', '-pthread', str(unit), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
