/* FXAM classification of represented double values; empty x87 tags are not modelled. */
#include "x86rt.h"
#include <assert.h>
#include <float.h>
#include <stdio.h>
int main(void) {
    const struct { double value; uint32_t bits; } cases[]={
        {0.0,0x4000},{-0.0,0x4200},{1.0,0x0400},{-1.0,0x0600},
        {INFINITY,0x0500},{-INFINITY,0x0700},{NAN,0x0100},
        {DBL_TRUE_MIN,0x0400},{-DBL_TRUE_MIN,0x0600}
    };
    CPU c={0};
    for (unsigned i=0;i<sizeof cases/sizeof cases[0];i++) {
        c.fsw=0xffffu; rt_fxam(&c,cases[i].value);
        assert((c.fsw&0x4700u)==cases[i].bits);
        assert((c.fsw&~0x4700u)==(0xffffu&~0x4700u));
    }
    puts("FXAM represented-value classification checks passed");
}
