#!/usr/bin/env python3
"""Compare the native rand warm path with actual translated SH3 CRT code."""
from pathlib import Path
import subprocess
import sys
import tempfile

root=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(root/'recomp'))
from pe import PE


def function(source,signature):
    start=source.index(signature)
    return source[start:source.index('\n}',start)+2]


runtime=(root/'port/diag_runtime.c').read_text()
rand=function((root/'port/gen/x86_f181.c').read_text(),'void F_0066405e(').replace('void F_0066405e(', 'void retained_rand(')
ptd=function((root/'port/gen/x86_f183.c').read_text(),'void F_00667a7f(')
imports='\n'.join(function(runtime,'void '+name+'(') for name in ['imp_KERNEL32_GetLastError','imp_KERNEL32_SetLastError','imp_KERNEL32_TlsGetValue'])
helper=function(runtime,'uint32_t diag_rand_thread_data(')
native=(root/'port/diag_rand.c').read_text()
pe=PE(root/'game/sh3.exe')
fixtures='\n'.join('static const unsigned char code_'+str(i)+'[]={'+','.join(str(x) for x in pe.read(a,n))+'};'
                   for i,(a,n) in enumerate([(0x66405e,0x22),(0x667a7f,0x71)]))
code=r'''
#include "x86rt.h"
#include "x86_funcs.h"
#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#define DIAG_MEM_SIZE (8u*1024*1024)
uint8_t *g_mem;
uint32_t g_mem_size=DIAG_MEM_SIZE;
static uint32_t diag_tls_used;
static int fallback_count,cold_stub;
void F_00663daf(CPU *c){(void)c;abort();}
void F_006629cf(CPU *c){(void)c;abort();}
void imp_KERNEL32_TlsSetValue(CPU *c){(void)c;abort();}
void imp_KERNEL32_GetCurrentThreadId(CPU *c){(void)c;abort();}
'''+imports+'\n'+helper+'\n'+ptd+'\n'+rand+r'''
void __real_F_0066405e(CPU *c) {
    fallback_count++;
    if(cold_stub) {c->eax=0x1234;c->esp+=4;return;}
    retained_rand(c);
}
'''+native+'\n'+fixtures+r'''
static uint32_t rng=0x13579bdf;
static uint32_t random_word(void){rng=rng*1664525u+1013904223u;return rng;}
int main(void) {
    g_mem=calloc(1,DIAG_MEM_SIZE);
    uint8_t *before=malloc(DIAG_MEM_SIZE),*oracle=malloc(DIAG_MEM_SIZE);
    assert(g_mem && before && oracle);
    memcpy(g_mem+0x66405e,code_0,sizeof code_0);
    memcpy(g_mem+0x667a7f,code_1,sizeof code_1);
    assert(diag_rand_layout_valid());
    g_mem[0x66405e]^=1; assert(!diag_rand_layout_valid()); g_mem[0x66405e]^=1;
    for(unsigned k=0;k<128;k++) {
        CPU input,a,b;
        for(unsigned i=0;i<sizeof input;i++) ((uint8_t *)&input)[i]=(uint8_t)random_word();
        input.esp=0x40000+64*(k%8);input.fs_base=0x20000+0x2000*(k%4);
        uint32_t index=k%32,p=0x100000+0x100*(k%4),seed=random_word(),error=random_word();
        diag_tls_used=1u<<index;
        memcpy(g_mem+0x7296b8,&index,4);
        memcpy(g_mem+input.fs_base+0xe10+4*index,&p,4);
        memcpy(g_mem+input.fs_base+0x34,&error,4);
        memcpy(g_mem+p+0x14,&seed,4);
        for(unsigned i=0;i<64;i++)g_mem[input.esp-32+i]=(uint8_t)random_word();
        memcpy(before,g_mem,DIAG_MEM_SIZE);
        a=input;retained_rand(&a);memcpy(oracle,g_mem,DIAG_MEM_SIZE);
        memcpy(g_mem,before,DIAG_MEM_SIZE);
        b=input;diag_rand_enabled=1;fallback_count=0;__wrap_F_0066405e(&b);
        assert(!fallback_count && !memcmp(&a,&b,sizeof a) && !memcmp(g_mem,oracle,DIAG_MEM_SIZE));
        memcpy(g_mem,before,DIAG_MEM_SIZE);
        b=input;diag_rand_enabled=0;fallback_count=0;__wrap_F_0066405e(&b);
        assert(fallback_count==1 && !memcmp(&a,&b,sizeof a) && !memcmp(g_mem,oracle,DIAG_MEM_SIZE));
    }
    CPU c={.esp=0x50000,.fs_base=0x20000}; uint32_t zero=0,index=0;
    memcpy(g_mem+0x7296b8,&index,4);diag_tls_used=1;
    memcpy(g_mem+c.fs_base+0xe10,&zero,4);
    cold_stub=1;fallback_count=0;diag_rand_enabled=1;
    __wrap_F_0066405e(&c);assert(fallback_count==1 && c.eax==0x1234 && c.esp==0x50004);
    puts("native rand: 128 full CPU/arena comparisons and cold fallback passed");
    free(oracle);free(before);free(g_mem);
}
'''
with tempfile.TemporaryDirectory(prefix='sh3-rand-') as temp:
    c=Path(temp)/'test.c'; exe=Path(temp)/'test'
    c.write_text(code)
    subprocess.run(['gcc','-std=gnu11','-O2','-Wall','-Wextra','-Wno-unused-variable','-Wno-unused-but-set-variable',
                    '-fsanitize=address,undefined','-I'+str(root/'port/include'),'-I'+str(root/'port/gen'),str(c),'-lm','-o',str(exe)],check=True)
    subprocess.run([str(exe)],check=True)
