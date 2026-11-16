/* Read-only observer of SH3 binding labels. Built only by the isolated host lab.
 * Addresses/call bytes independently checked against PE CRC32 372ae792.
 * No third-party patch implementation or assets are included. */
#include "x86rt.h"
#include <stdint.h>
#include <string.h>
extern uint32_t g_mem_size;
extern void rt_log(const char *format,...);
extern void F_005ee710_orig(CPU *c);
void F_005ee710(CPU *c) {
    const uint8_t *mb=g_mem;
    if ((uint64_t)c->esp+32<=g_mem_size) {
        uint32_t caller;memcpy(&caller,mb+c->esp,4);
        if(caller==0x5ed654u || caller==0x5ee18au || caller==0x5ee28cu || caller==0x5ee363u) {
            uint32_t p,x,y;memcpy(&p,mb+c->esp+16,4);memcpy(&x,mb+c->esp+4,4);memcpy(&y,mb+c->esp+8,4);
            if(p<g_mem_size) {
                char text[97];unsigned n=0;
                while(n<96 && (uint64_t)p+n<g_mem_size && mb[p+n]) {text[n]=(char)mb[p+n];n++;}
                text[n]=0;
                uint32_t h=2166136261u;for(unsigned i=0;i<n;i++)h=(h^(unsigned char)text[i])*16777619u;
                h^=caller;h=h*16777619u^x;h=h*16777619u^y;
                static uint32_t seen[128];static unsigned count;
                unsigned i=0;while(i<count && seen[i]!=h)i++;
                if(i==count && count<128) {
                    seen[count++]=h;
                    rt_log("[ui-label] caller=%08x x=%u y=%u ptr=%08x length=%u text=%s\n",caller,x,y,p,n,text);
                }
            }
        }
    }
    F_005ee710_orig(c);
}
