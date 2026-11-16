#!/usr/bin/env python3
"""Compare whole CPU and scratch memory for exact-body CRT inlining."""
from pathlib import Path
import subprocess
import sys
import tempfile
r = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(r/'tools'))
from floor_inline_unit import function, transform
base = (r/'port/gen/x86_f180.c').read_text()
control = (r/'port/gen/x86_f186.c').read_text()
rounding = (r/'port/gen/x86_f188.c').read_text()
expanded = transform(base, control, rounding)
a = expanded.index('static __attribute__((always_inline)) inline void floor_control_inline(')
b = expanded.index('\nvoid F_', expanded.index('void F_006634a0(CPU *c)', a) + 8)
code = expanded[a:b]
# Remap only the two fixed CRT globals, identically in retained and candidate.
code = code.replace('0x7179b44u', '0x100u').replace('0x729978u', '0x108u')
leaf1 = function(control, 'F_00670c35').replace('void F_00670c35(', '__attribute__((noinline)) void F_00670c35(')
leaf2 = function(rounding, 'F_006741ad').replace('void F_006741ad(', '__attribute__((noinline)) void F_006741ad(')
source = r'''#include "x86rt.h"
#include <fenv.h>
#include <stdio.h>
#include <stdlib.h>
uint8_t *g_mem; unsigned g_diag_perf;
_Noreturn void rt_unimpl(CPU *c, uint32_t pc, const char *what) { (void)c; fprintf(stderr,"unexpected %s %x\n",what,pc); exit(2); }
void F_0066b3dc(CPU *c) { (void)c; abort(); }
void F_006702ec(CPU *c) { (void)c; abort(); }
void F_006709db(CPU *c) { (void)c; abort(); }
void F_00670a8d(CPU *c) { (void)c; abort(); }
double rt_fround(CPU *c, double v) {
 switch ((c->fcw>>10)&3) {case 0:return nearbyint(v);case 1:return floor(v);case 2:return ceil(v);default:return trunc(v);}
}
''' + leaf1 + leaf2 + code + r'''
static uint64_t seed=0x94e37a;
static uint64_t rnd(void) {seed^=seed<<13;seed^=seed>>7;seed^=seed<<17;return seed;}
int main(void) {
 uint8_t a[4096],b[4096];
 double edge[]={0.,-0.,0.5,-0.5,1.5,-1.5,2147483647.,-2147483648.,0x1p52,-0x1p52,0x1p-1074,-0x1p-1074};
 for(unsigned k=0;k<32768;k++) {
  const int host_modes[]={FE_TONEAREST,FE_DOWNWARD,FE_UPWARD,FE_TOWARDZERO};
  if(fesetround(host_modes[(k/8192)%4]))return 3;
  CPU initial;for(unsigned i=0;i<sizeof initial;i++)((uint8_t*)&initial)[i]=(uint8_t)rnd();
  initial.esp=2048+(k%16);initial.ftop=(k/16)%8;initial.fcw=0x37fu|(((k/128)%4)<<10);
  uint64_t bits=rnd();bits&=~(UINT64_C(0x7ff)<<52);bits|=(rnd()%2047)<<52;double value;memcpy(&value,&bits,8);
  if(k%3==0)value=edge[(k/3)%(sizeof edge/sizeof edge[0])];
  if(k%3==1)value=(int32_t)rnd()/128.;
  for(unsigned i=0;i<sizeof a;i++)a[i]=(uint8_t)rnd();
  uint32_t flag=0,fcw=0x37fu|(((k/32)%4)<<10);memcpy(a+0x100,&flag,4);memcpy(a+0x108,&fcw,4);memcpy(a+initial.esp+4,&value,8);memcpy(b,a,sizeof a);
  CPU ca=initial,cb=initial;g_mem=a;g_diag_perf=0;F_006634a0(&ca);g_mem=b;g_diag_perf=524288;F_006634a0(&cb);
  if(memcmp(&ca,&cb,sizeof ca)||memcmp(a,b,sizeof a)) {fprintf(stderr,"floor mismatch %u x=%a\n",k,value);return 1;}
 }
 puts("floor-inline: 32768 finite full CPU/4KB scratch/host and guest rounding cases passed; SSE and exception paths unchanged/uncovered");
}
'''
with tempfile.TemporaryDirectory() as d:
 c=Path(d)/'test.c';c.write_text(source)
 for opt in ['-Os','-O2']:
  exe=Path(d)/'floor'
  subprocess.run(['gcc',opt,'-ffp-contract=off','-fsanitize=address,undefined','-I'+str(r/'port/include'),str(c),'-lm','-o',str(exe)],check=True)
  subprocess.run([str(exe)],check=True)
