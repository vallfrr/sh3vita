/* IEEE binary64 -> signed64 with the virtual x87 rounding mode.
 * Mode0 requires host FE_TONEAREST, checked by the runtime wrapper.
 * Invalid values return the same integer-indefinite sentinel as rt_fist64.
 * No guest state or native floating-point control is modified. */
#ifndef SH3_DIAG_FIST_BITS_H
#define SH3_DIAG_FIST_BITS_H
#include <stdint.h>
#include <string.h>
#include <limits.h>
static inline int64_t diag_fist_bits(double value,unsigned mode) {
    uint64_t bits;memcpy(&bits,&value,8);
    unsigned negative=(unsigned)(bits>>63);
    int exponent=(int)((bits>>52)&2047u)-1023;
    uint64_t fraction=bits&UINT64_C(0xfffffffffffff);
    if (exponent>=63) return INT64_MIN; /* also NaN/Inf */
    uint64_t magnitude=0,remainder=0,half=0;
    if (exponent<0) {
        remainder=bits&UINT64_C(0x7fffffffffffffff);
        if (mode==0 && exponent==-1 && fraction) magnitude=1;
        else if ((mode==1 && negative) || (mode==2 && !negative)) magnitude=remainder!=0;
    } else {
        uint64_t mantissa=fraction|UINT64_C(0x10000000000000);
        if (exponent>=52) magnitude=mantissa<<(exponent-52);
        else {
            unsigned shift=52u-(unsigned)exponent;
            magnitude=mantissa>>shift;
            remainder=mantissa&((UINT64_C(1)<<shift)-1);
            half=UINT64_C(1)<<(shift-1);
            if ((mode==0 && (remainder>half || (remainder==half && (magnitude&1)))) ||
                (mode==1 && negative && remainder) || (mode==2 && !negative && remainder)) ++magnitude;
        }
    }
    return negative?-(int64_t)magnitude:(int64_t)magnitude;
}
#endif
