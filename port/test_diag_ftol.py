#!/usr/bin/env python3
"""Differential CRT conversion: retained generated code versus full CPU/scratch contract."""
from pathlib import Path
import sys
import tempfile
import subprocess

root=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(root/'tools'))
from numeric_profile_units import ftol_unit,function
unit=(root/'port/gen/x86_f180.c').read_text(encoding='utf-8')
retained=function(ftol_unit(unit),'diag_ftol_retained')
runtime=(root/'port/diag_runtime.c').read_text(encoding='utf-8')
start=runtime.index('double rt_fround(');end=runtime.index('\ndouble rt_f80_load(',start)
helpers=runtime[start:end]
native=(root/'port/diag_ftol.c').read_text(encoding='utf-8')
source='''#include "x86rt.h"
#include "diag_fist_bits.h"
#include <fenv.h>
#include <stdio.h>
#include <stdlib.h>
uint8_t *g_mem;unsigned g_diag_perf=4096;
'''+helpers+retained+native+r'''
static uint64_t seed=12345678;
static uint64_t rnd(void) { seed^=seed<<13;seed^=seed>>7;seed^=seed<<17;return seed; }
int main(void) {
 uint8_t a[4096],b[4096];
 double edge[]={0.,-0.,0.25,-0.25,0.5,-0.5,1.5,-1.5,2.5,-2.5,1.,-1.,
  2147483647.,-2147483648.,4294967296.,-4294967296.,0x1p63,-0x1p63,
  0x1.fffffffffffffp62,-0x1.fffffffffffffp62,INFINITY,-INFINITY,NAN,0x1p-1074,-0x1p-1074};
 for(unsigned k=0;k<32768;k++) {
  CPU initial;for(unsigned i=0;i<sizeof initial;i++)((uint8_t*)&initial)[i]=(uint8_t)rnd();
  initial.esp=2048+(k%16);initial.ftop=(k/16)%8;initial.fcw=(0x37fu&~0xc00u)|(((k/128)%4)<<10);
  uint64_t bits=rnd();double x;memcpy(&x,&bits,8);
  if(k%3==0)x=edge[(k/3)%(sizeof edge/sizeof edge[0])];
  if(k%3==1)x=(int32_t)rnd()/128.0;
  initial.st[initial.ftop]=x;
  for(unsigned i=0;i<sizeof a;i++)a[i]=(uint8_t)rnd();memcpy(b,a,sizeof a);
  CPU ca=initial,cb=initial;g_mem=a;g_diag_perf=0;diag_ftol_retained(&ca);g_mem=b;
  if(getenv("SH3_TEST_FISTBITS")) { g_diag_perf=131072;diag_ftol_retained(&cb); }
  else { g_diag_perf=4096;F_00662c40(&cb); }
  if(memcmp(&ca,&cb,sizeof ca)||memcmp(a,b,sizeof a)) {
   fprintf(stderr,"FTOL mismatch case%u x=%a fcw=%x\n",k,x,initial.fcw);return 1;
  }
 }
 puts("FTOL: 32768 full CPU/4KB arena cases, rounding/alignment/IEEE edges passed");
}
'''
with tempfile.TemporaryDirectory() as d:
    c=Path(d)/'ftol.c';c.write_text(source,encoding='utf-8')
    for opt in ['-Os','-O2']:
        exe=Path(d)/'ftol'
        subprocess.run(['gcc',opt,'-ffp-contract=off','-fsanitize=address,undefined','-I'+str(root/'port/include'),str(c),'-lm','-o',str(exe)],check=True)
        subprocess.run([str(exe)],check=True)
