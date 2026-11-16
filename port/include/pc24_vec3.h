/* Isolated D3DXVec3Transform path for the native D3D8 PC24 contract.
 * Every operation rounds to binary32. The input bounds keep every nonzero
 * intermediate normal (no x87 extended-exponent versus float discrepancy,
 * nor NEON flush-to-zero discrepancy). Other domains retain the old port.
 */
#pragma once
#include "x86rt.h"
#include <fenv.h>
extern uint32_t g_mem_size;
typedef float DiagPc24Vector __attribute__((vector_size(16)));

static inline __attribute__((always_inline)) int diag_pc24_vec3(CPU *c) {
    if ((c->fcw&0x0f3fu)!=0x003fu) return 0; /* PC24, nearest, masked exceptions. */
#if defined(__arm__) && defined(__ARM_FP)
    unsigned fpscr;
    __asm__ __volatile__("vmrs %0, fpscr" : "=r"(fpscr));
    if (fpscr&(3u<<22)) return 0;
#else
    if (fegetround()!=FE_TONEAREST) return 0;
#endif
    uint8_t *const mb=g_mem;
    uint32_t sp=c->esp;
    if ((uint64_t)sp+16u>g_mem_size) return 0;
    uint32_t o=R32(sp+4u),v=R32(sp+8u),m=R32(sp+12u);
    if ((uint64_t)o+16u>g_mem_size || (uint64_t)v+12u>g_mem_size ||
        (uint64_t)m+64u>g_mem_size) return 0;
    uint32_t vb[3],matrix[16];
    memcpy(vb,mb+v,sizeof vb);memcpy(matrix,mb+m,sizeof matrix);
    for (unsigned i=0;i<19;i++) {
        uint32_t a=(i<3?vb[i]:matrix[i-3])&0x7fffffffu;
        /* Zero, or 2^-30 <= magnitude <= 2^30. Products lie in [2^-60,2^60].
         * Cancellation of 24-bit values is zero or >=2^-83, still normal. */
        if (a && (a<0x30800000u || a>0x4e800000u)) return 0;
    }
    float x=rt_f32(vb[0]),y=rt_f32(vb[1]),z=rt_f32(vb[2]);
    DiagPc24Vector a,b,d,t;
    memcpy(&a,matrix,16);memcpy(&b,matrix+4,16);
    memcpy(&d,matrix+8,16);memcpy(&t,matrix+12,16);
    DiagPc24Vector vx={x,x,x,x},vy={y,y,y,y},vz={z,z,z,z};
    DiagPc24Vector r=((vx*a+vy*b)+vz*d)+t;
    memcpy(mb+o,&r,16); /* All inputs have been read: output may alias them. */
    c->ecx=o;c->edx=m;c->eax=R32(sp+4u);c->esp=sp+16u;c->ftop&=7u;
    return 1;
}
