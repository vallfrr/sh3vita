#!/usr/bin/env python3
"""Differential native clipping: exact retained translation, full CPU and 8MiB arena."""
from pathlib import Path
import subprocess
import sys
import tempfile

root=Path(__file__).resolve().parents[1]
sys.path[:0]=[str(root/'tools'),str(root/'recomp')]
from numeric_profile_units import function
from pe import PE
unit=(root/'port/gen/x86_f045.c').read_text(encoding='utf-8')
retained=function(unit,'F_0048c290').replace('void F_0048c290(', 'void clip_retained(')
candidate='static unsigned native_hits, fallback_hits, kinds[3]; void clip_candidate(CPU *c) { if(native_clip(c)){native_hits++;kinds[c->eax]++;}else{fallback_hits++;clip_retained(c);} }'
pe=PE(root/'game/sh3.exe')
def constant(addr):
    data=pe.read(addr,4)
    if data is None:
        assert any(va+rawsize<=addr and addr+4<=va+vsize for _,va,vsize,_,rawsize,_ in pe.sections),hex(addr)
        return bytes(4)  # Verified virtual-only tail: loader zero-initializes BSS.
    assert len(data)==4
    return data
fixtures='\n'.join('memcpy(a+0x%x,(unsigned char[]){%s},4);'%(addr,','.join(map(str,constant(addr))))
                   for addr in [0x72c26c,0x6909f4,0x68ebac,0x68a868,0x68a754,0x690010])
source='''#include "x86rt.h"
#include "native_clip.h"
#include <stdio.h>
#include <stdlib.h>
uint8_t *g_mem;
'''+retained+candidate+r'''
#define N (8u<<20)
static uint32_t seed=321;
static uint32_t rnd(void) { seed^=seed<<13;seed^=seed>>17;seed^=seed<<5;return seed; }
int main(void) {
 unsigned char *a=calloc(1,N),*b=malloc(N);
 FIXTURES
 for(unsigned k=0;k<12000;k++) {
  CPU initial;for(unsigned i=0;i<sizeof initial;i++)((unsigned char*)&initial)[i]=(unsigned char)rnd();
  initial.esp=0x4000;initial.ftop=k%8;
  uint32_t args[4]={0,0x2000,0x2100,0x2200};
  if(k%4==1)args[1]=args[2];if(k%4==2)args[1]=args[3];if(k%4==3)args[3]=args[2];
  for(unsigned j=0;j<3;j++) {
   float x=(int32_t)rnd()/4096.0f,y=(int32_t)rnd()/4096.0f;
   if(k%7==0) { uint32_t edges[]={0,0x80000000u,0x7f800000u,0xff800000u,0x7fc01234u,0x7f7fffffu,1,0x80000001u};x=rt_f32(edges[rnd()%8]);y=rt_f32(edges[rnd()%8]); }
   if(k%5==0)x=j?0.f:1.f;
   memcpy(a+0x2100+4*j,&x,4);memcpy(a+0x2200+4*j,&y,4);
  }
  float plane=(k%3)?(float)(int32_t)rnd()/65536.f:0;
  memcpy(a+0x72c26c,&plane,4);
  memcpy(a+initial.esp,args,sizeof args);memcpy(b,a,N);
  CPU ca=initial,cb=initial;g_mem=a;clip_retained(&ca);g_mem=b;clip_candidate(&cb);
  if(memcmp(&ca,&cb,sizeof ca)||memcmp(a,b,N)) {
   fprintf(stderr,"native clipping mismatch case%u ret=%u/%u fsw=%x/%x\n",k,ca.eax,cb.eax,ca.fsw,cb.fsw);
   for(unsigned i=0;i<N;i++)if(a[i]!=b[i]){fprintf(stderr,"memory first difference @%x %02x/%02x\n",i,a[i],b[i]);break;}
   return 1;
  }
 }
 if(!native_hits || !fallback_hits || !kinds[0] || !kinds[1] || !kinds[2])return 2;
 printf("native=%u fallback=%u return0/1/2=%u/%u/%u\n",native_hits,fallback_hits,kinds[0],kinds[1],kinds[2]);
 free(a);free(b);puts("Native clipping: 12000 full CPU/8MiB arena cases passed (native clipping, F48e140 not covered)");
}
'''.replace('FIXTURES',fixtures)
with tempfile.TemporaryDirectory() as d:
    c=Path(d)/'geo.c';c.write_text(source,encoding='utf-8');exe=Path(d)/'geo'
    subprocess.run(['gcc','-Os','-ffp-contract=off','-fsanitize=address,undefined','-I'+str(root/'port/include'),str(c),'-lm','-o',str(exe)],check=True)
    subprocess.run([str(exe)],check=True)
