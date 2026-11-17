#!/usr/bin/env python3
"""Compare cached and retained apply_states as GL state machines, not GPU pixels."""
from pathlib import Path
import re
import subprocess
import tempfile


def function(source, signature):
    start = source.index(signature)
    return source[start:source.index('\n}', start) + 2]


def main():
    source = (Path(__file__).parent / 'sh4base/d3d8/draw.c').read_text()
    cache = source[source.index('/* Exact state memoization'):source.index("// OpenGL's missing-location")]
    helpers = '\n'.join(function(source, 'static GLenum ' + name + '(')
                        for name in ('blend_of', 'cmp_of', 'sop_of'))
    candidate = function(source, 'static void apply_states(')
    gate = '    if (cache && state_cache_matches(d,vsh)) return;'
    assert candidate.count(gate) == 1
    retained = candidate.replace(gate, '').replace('apply_states(', 'apply_retained(', 1)
    constants = set(re.findall(r'\bGL_[A-Z_0-9]+\b', helpers + candidate)) - {'GL_TRUE', 'GL_FALSE'}
    definitions = '\n'.join(f'#define {name} {index}' for index, name in enumerate(sorted(constants), 2))
    prefix = r'''
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
typedef unsigned GLenum;
typedef int GLint;
typedef int GLsizei;
typedef unsigned char GLboolean;
#define GL_FALSE 0
#define GL_TRUE 1
typedef struct { unsigned shift; } Surface;
typedef struct {
    uint32_t rs[256];
    struct { uint32_t x,y,w,h; float minz,maxz; } vp;
    Surface *rt,*ds;
} Device;
static unsigned rt_shift(Surface *s) { return s->shift; }
static unsigned g_diag_perf=1048576u;
static int fx_active;
static int g_glstatecache_ab, g_glstatecache_phase; /* A/B switch of the real function, off here */
typedef struct { unsigned enabled[128]; uint64_t args[32]; } GlState;
static GlState *current;
static unsigned calls;
static uint64_t number(double d) { uint64_t u; memcpy(&u,&d,8);return u; }
static void glEnable(GLenum e) { current->enabled[e]=1;++calls; }
static void glDisable(GLenum e) { current->enabled[e]=0;++calls; }
static void glViewport(GLint x,GLint y,GLsizei w,GLsizei h) {
    current->args[0]=(uint32_t)x;current->args[1]=(uint32_t)y;
    current->args[2]=(uint32_t)w;current->args[3]=(uint32_t)h;++calls;
}
static void glDepthRange(double x,double y) { current->args[4]=number(x);current->args[5]=number(y);++calls; }
static void glDepthFunc(GLenum x) { current->args[6]=x;++calls; }
static void glDepthMask(GLboolean x) { current->args[7]=x;++calls; }
static void glBlendFunc(GLenum x,GLenum y) { current->args[8]=x;current->args[9]=y;++calls; }
static void glBlendEquation(GLenum x) { current->args[10]=x;++calls; }
static void glFrontFace(GLenum x) { current->args[11]=x;++calls; }
static void glCullFace(GLenum x) { current->args[12]=x;++calls; }
static void glStencilFunc(GLenum x,GLint y,unsigned z) {
    current->args[13]=x;current->args[14]=(uint32_t)y;current->args[15]=z;++calls;
}
static void glStencilOp(GLenum x,GLenum y,GLenum z) {
    current->args[16]=x;current->args[17]=y;current->args[18]=z;++calls;
}
static void glStencilMask(unsigned x) { current->args[19]=x;++calls; }
static void glColorMask(GLboolean x,GLboolean y,GLboolean z,GLboolean w) {
    current->args[20]=x;current->args[21]=y;current->args[22]=z;current->args[23]=w;++calls;
}
static void glPolygonOffset(float x,float y) { current->args[24]=number(x);current->args[25]=number(y);++calls; }
'''
    driver = r'''
static uint32_t rng=0x1a72e369;
static unsigned random_word(void) { rng^=rng<<13;rng^=rng>>17;rng^=rng<<5;return rng; }
int main(void) {
    Surface rt={0},depth={0};
    Device d={.rt=&rt,.ds=&depth,.vp={0,0,960,544,0,1}};
    GlState expected={0},actual={0};
    unsigned reference_calls=0,candidate_calls=0;
    for (unsigned iteration=0;iteration<32768;iteration++) {
        if (!(iteration%8)) {
            unsigned index=(iteration/8)%256;
            d.rs[index]=random_word();
            if (!(iteration%256)) {
                rt.shift=random_word()%3;fx_active=random_word()%2;
                d.ds=(random_word()&1)?&depth:NULL;
                d.vp.x=random_word()%100;d.vp.y=random_word()%100;
                d.vp.w=1+random_word()%1024;d.vp.h=1+random_word()%1024;
                d.vp.minz=(float)(random_word()%1024)/1024;
                d.vp.maxz=(float)(random_word()%1024)/1024;
            }
        }
        if (!(iteration%37)) {
            /* Model an external GL callback, then its required invalidation. */
            unsigned byte=random_word()&255;
            memset(&expected,byte,sizeof expected);memset(&actual,byte,sizeof actual);
            gfx_state_invalidate();
        }
        if (iteration%521==0) d.vp.minz=NAN; /* invalid input must not be memoized */
        else if (iteration%521==2) d.vp.minz=0;
        current=&expected;calls=0;apply_retained(&d);reference_calls+=calls;
        current=&actual;calls=0;apply_states(&d);candidate_calls+=calls;
        if (memcmp(&expected,&actual,sizeof expected)) {
            fprintf(stderr,"GL state mismatch at %u\n",iteration);return 1;
        }
    }
    if (!state_hits || candidate_calls>=reference_calls/2) return 2;
    printf("32768 state transitions PASS; GL calls %u -> %u, hits=%u misses=%u\n",
           reference_calls,candidate_calls,state_hits,state_misses);
    return 0;
}
'''
    with tempfile.TemporaryDirectory(prefix='sh3-gl-state-') as directory:
        cfile = Path(directory) / 'states.c'
        cfile.write_text(prefix + definitions + '\n' + cache + helpers + retained + candidate + driver)
        for flags in (['-Os'], ['-O2', '-fsanitize=address,undefined', '-fno-omit-frame-pointer']):
            binary = Path(directory) / 'states'
            subprocess.run(['cc', '-std=c11', '-D__vita__', *flags, str(cfile), '-lm', '-o', str(binary)], check=True)
            subprocess.run([str(binary)], check=True)


if __name__ == '__main__':
    main()
