#!/usr/bin/env python3
"""Compare integer IEEE rounding with retained runtime under all host/guest modes."""
from pathlib import Path
import subprocess
import tempfile

root=Path(__file__).resolve().parents[1]
runtime=(root/'port/diag_runtime.c').read_text(encoding='utf-8')
start=runtime.index('double rt_fround(')
end=runtime.index('\ndouble rt_f80_load(',start)
helpers=runtime[start:end]
source=r'''
#include "x86rt.h"
#include "diag_fist_bits.h"
#include <fenv.h>
#include <stdio.h>
unsigned g_diag_perf=131072;
'''+helpers+r'''
static uint64_t state=0x73825ef013b;
static uint64_t random64(void) {state^=state<<13;state^=state>>7;state^=state<<17;return state;}
int main(void) {
 int host_modes[]={FE_TONEAREST,FE_DOWNWARD,FE_UPWARD,FE_TOWARDZERO};
 unsigned cases=0;
 for(unsigned h=0;h<4;h++) {
  if(fesetround(host_modes[h]))return 2;
  for(unsigned k=0;k<262144;k++) {
   CPU c={0};c.fcw=0x37fu|((k%4)<<10);
   uint64_t bits=random64();
   if(k%3==0) {
    /* Concentrate around all binary exponents, including halfway ties. */
    unsigned e=(k/12)%2048;
    uint64_t fractions[]={0,1,0x7ffffffffffff,0x8000000000000,0xfffffffffffff};
    bits=((uint64_t)(k/4%2)<<63)|((uint64_t)e<<52)|fractions[(k/24)%5];
   }
   if(k%3==1) {
    double v=(int32_t)random64()/4.0;memcpy(&bits,&v,8);
   }
   double value;memcpy(&value,&bits,8);
   int64_t expected=diag_fist64_retained(&c,value);
   int64_t actual=rt_fist64(&c,value);
   if(actual!=expected) {
    fprintf(stderr,"mismatch bits=%016llx host=%d fcw=%x expected=%lld actual=%lld\n",
     (unsigned long long)bits,host_modes[h],c.fcw,(long long)expected,(long long)actual);return 1;
   }
   ++cases;
  }
 }
 fesetround(FE_TONEAREST);
 printf("fistbits: %u cases, all4 host/guest rounding modes, IEEE extremes passed\n",cases);
}
'''
with tempfile.TemporaryDirectory() as d:
    c=Path(d)/'fist.c';c.write_text(source,encoding='utf-8')
    for opt in ['-Os','-O2']:
        exe=Path(d)/'fist'
        subprocess.run(['gcc',opt,'-frounding-math','-ffp-contract=off','-fsanitize=address,undefined',
                        '-I'+str(root/'port/include'),str(c),'-lm','-o',str(exe)],check=True)
        subprocess.run([str(exe)],check=True)
