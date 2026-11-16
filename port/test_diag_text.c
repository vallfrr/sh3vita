/* Host-only tests of the diagnostic bridge, not a Windows compatibility proof. */
#include "x86rt.h"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
extern void imp_KERNEL32_MultiByteToWideChar(CPU *);
extern void imp_KERNEL32_WideCharToMultiByte(CPU *);
extern void imp_KERNEL32_GetStringTypeW(CPU *);
static void call(CPU *c, void (*f)(CPU *), const uint32_t *args, unsigned n) {
    c->esp=0x1000; memset(g_mem+c->esp,0,4); memcpy(g_mem+c->esp+4,args,4*n);
    f(c); assert(c->esp==0x1000+4*(n+1));
}
int main(void) {
    g_mem=calloc(1,200u*1024*1024); assert(g_mem);
    CPU c={0}; const uint32_t src=0x2000, wide=0x3000, result=0x4000, types=0x5000;
    for (unsigned i=0;i<256;i++) g_mem[src+i]=(uint8_t)i;
    uint32_t towide[]={1252,1,src,256,wide,256}; call(&c,imp_KERNEL32_MultiByteToWideChar,towide,6); assert(c.eax==256);
    uint16_t v; memcpy(&v,g_mem+wide+0x80*2,2); assert(v==0x20ac);
    memcpy(&v,g_mem+wide+0xe9*2,2); assert(v==0x00e9);
    uint32_t tonarrow[]={1252,0,wide,256,result,256,0,0}; call(&c,imp_KERNEL32_WideCharToMultiByte,tonarrow,8);
    assert(c.eax==256 && !memcmp(g_mem+src,g_mem+result,256));
    uint32_t query[]={1252,0,wide,256,0,0,0,0}; call(&c,imp_KERNEL32_WideCharToMultiByte,query,8); assert(c.eax==256);
    uint16_t sample[]={'A','a','0',' ','\t','!'}; memcpy(g_mem+wide,sample,sizeof sample);
    uint32_t classify[]={1,wide,6,types}; call(&c,imp_KERNEL32_GetStringTypeW,classify,4); assert(c.eax==1);
    uint16_t f[6]; memcpy(f,g_mem+types,sizeof f);
    assert(f[0]==(0x001|0x100|0x080)); assert(f[1]==(0x002|0x100|0x080));
    assert(f[2]==(0x004|0x080)); assert(f[3]==(0x008|0x040));
    assert(f[4]==(0x008|0x020|0x040)); assert(f[5]==0x010);
    free(g_mem); puts("diagnostic text checks passed"); return 0;
}
