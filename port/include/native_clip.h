/* SH3 F_0048c290: ordered finite, nonoverlapping native path.
 * Keep double intermediates and every native float-store rounding. The
 * retained translation handles exceptional data and overlapping arguments. */
#pragma once
#include "x86rt.h"

static inline int clip_overlap(uint32_t a, unsigned an, uint32_t b, unsigned bn) {
    /* native_clip checks the 256MiB arena before these bounded additions. */
    return a < b+bn && b < a+an;
}
static inline int native_clip(CPU *c) {
    uint8_t *const mb=g_mem;
    uint32_t sp=c->esp, out=R32(sp+4), a=R32(sp+8), b=R32(sp+12);
    if(sp<12u || (sp|out|a|b)>0x0ffffff0u)return 0;
    /* Keep output and scratch wholly outside the constants' enclosing range;
       unusual destinations inside that range use the original translation. */
    if(!((out<=0x68a748u || out>=0x72c270u) &&
         (sp<=0x68a744u || sp>=0x72c27cu)))return 0;
    if (clip_overlap(out,12,a,12) || clip_overlap(out,12,b,12) ||
        clip_overlap(a,12,sp-12,28) || clip_overlap(b,12,sp-12,28) ||
        clip_overlap(out,12,sp-12,28)) return 0;
    static const uint32_t addresses[]={0x72c26c,0x6909f4,0x68ebac,0x68a868,0x68a754,0x690010};
    double k[6];
    for(unsigned i=0;i<6;i++) {
        uint32_t bits=R32(addresses[i]);
        if((bits&0x7f800000u)==0x7f800000u)return 0;
        k[i]=(double)rt_f32(bits);
    }
    double av[3],bv[3];
    for(unsigned i=0;i<3;i++) {
        uint32_t x=R32(a+4*i),y=R32(b+4*i);
        if((x&0x7f800000u)==0x7f800000u || (y&0x7f800000u)==0x7f800000u)return 0;
        av[i]=(double)rt_f32(x);bv[i]=(double)rt_f32(y);
    }
    double plane=k[0]*k[1], dz=bv[2]-av[2];
    float dx=(float)(bv[0]-av[0]),dy=(float)(bv[1]-av[1]);
    float bound=(float)(k[2]*dz);
    if(!isfinite(dx) || !isfinite(dy) || !isfinite(bound))return 0;
    W32(sp-12,rt_u32f(dx));W32(sp-8,rt_u32f(dy));W32(sp+12,rt_u32f(bound));
    if(dz<k[3] && bv[2]>plane && (fabs((double)dx)<bound || fabs((double)dy)<bound)) {
        double inverse=k[4]/dz;
        W32(out,rt_u32f((float)(((plane-av[2])*bv[0]+(bv[2]-plane)*av[0])*inverse)));
        W32(out+4,rt_u32f((float)(((bv[2]-plane)*av[1]+(plane-av[2])*bv[1])*inverse)));
        W32(out+8,rt_u32f((float)plane));
        rt_fcom(c,fabs((double)dx)<bound?fabs((double)dx):fabs((double)dy),(double)bound);
        c->eax=1;
    } else {
        double length=sqrt((double)dy*dy+(double)dx*dx+dz*dz);
        double scale=length==k[3]?k[4]:k[5]/length;
        W32(out,rt_u32f((float)((double)dx*scale+bv[0])));
        W32(out+4,rt_u32f((float)((double)dy*scale+bv[1])));
        W32(out+8,rt_u32f((float)(scale*dz+bv[2])));
        rt_fcom(c,plane,bv[2]);c->eax=plane<bv[2]?0:2;
    }
    c->ecx=b;c->edx=a;c->esp=sp+4;
    return 1;
}
