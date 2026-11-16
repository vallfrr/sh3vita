#!/usr/bin/env python3
"""Full CPU and guest-arena differential test against the retained generated matrix function."""
from pathlib import Path
import sys
import subprocess
import tempfile

root=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(root/'tools'))
from matrix_profile_unit import extract,optimize

original=extract((root/'port/gen/x86_f171.c').read_text(encoding='utf-8'))
try:
    optimize(original+' /* changed */')
    raise AssertionError('signature check failed')
except ValueError:pass
candidate=optimize(original)
source='''#include "x86rt.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
uint8_t *g_mem;
'''+original+candidate+r'''
#define N 65536
static uint8_t arena[N],expected[N];
static uint32_t seed=12345;
static uint32_t rnd(void) { seed^=seed<<13;seed^=seed>>17;seed^=seed<<5;return seed; }
int main(void) {
 for (unsigned k=0;k<8192;k++) {
  for (unsigned i=0;i<N;i+=4) { uint32_t x=rnd();memcpy(arena+i,&x,4); }
  CPU initial; for (unsigned i=0;i<sizeof initial;i++) ((uint8_t*)&initial)[i]=(uint8_t)rnd();
  initial.esp=0xe000;initial.ftop=k%8;initial.df=(k/8)%2;
  uint32_t args[4]={0x12345678,0x2000,0x2100,0x2200};
  // Exact aliases, partial overlaps, stack scratch aliases, and disjoint operands.
  switch ((k/16)%8) {
   case 1:args[1]=args[2];break;
   case 2:args[1]=args[3];break;
   case 3:args[1]=args[2]=args[3];break;
   case 4:args[2]=args[3];break;
   case 5:args[1]=args[2]+4;args[3]=args[2]+8;break;
   case 6:args[1]=0xdfa0;break;
   case 7:args[2]=0xdfa0;break;
  }
  // Normal geometry, arbitrary finite IEEE extremes, or arbitrary bits (fallback).
  if (k%3==0) for(unsigned i=0x1f00;i<0x2300;i+=4) {
   float v=(int32_t)rnd()/65536.0f;memcpy(arena+i,&v,4);
  }
  if (k%3==1) for(unsigned i=0x1f00;i<0x2300;i+=4) {
   uint32_t bits=rnd();if((bits&0x7f800000u)==0x7f800000u)bits^=0x00800000u;
   memcpy(arena+i,&bits,4);
  }
  memcpy(arena+initial.esp,args,sizeof args);memcpy(expected,arena,N);
  CPU a=initial,b=initial;g_mem=expected;F_00648639(&a);g_mem=arena;matrix_scalar(&b);
  if(memcmp(&a,&b,sizeof a)||memcmp(expected,arena,N)) {
   fprintf(stderr,"matrix mismatch case %u alias %u ftop%u df%u\n",k,(k/16)%8,initial.ftop,initial.df);
   for(unsigned i=0;i<sizeof a;i++) if(((uint8_t*)&a)[i]!=((uint8_t*)&b)[i]) fprintf(stderr,"CPU byte %u: %02x/%02x\n",i,((uint8_t*)&a)[i],((uint8_t*)&b)[i]);
   for(unsigned i=0;i<N;i+=4) if(memcmp(expected+i,arena+i,4)) {uint32_t x,y;memcpy(&x,expected+i,4);memcpy(&y,arena+i,4);fprintf(stderr,"MEM %x: %08x/%08x\n",i,x,y);}
   return 1;
  }
 }
 puts("matrix scalar: 8192 full CPU/64KB arena cases, all ftop/df/aliases, passed");
}
'''
with tempfile.TemporaryDirectory() as d:
    c=Path(d)/'matrix.c';c.write_text(source,encoding='utf-8')
    for level in ['-Os','-O2']:
        exe=Path(d)/'matrix'
        subprocess.run(['gcc',level,'-ffp-contract=off','-fsanitize=address,undefined','-fno-omit-frame-pointer',
                        '-I'+str(root/'port/include'),str(c),'-o',str(exe),'-lm'],check=True)
        subprocess.run([str(exe)],check=True)
