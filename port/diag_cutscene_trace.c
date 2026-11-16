/* Windows diagnostic only: locate the first NULL cutscene header, before the
   retained script reader can copy guest address 16 over address zero. */
#include "x86rt.h"
#include <stdio.h>
#include <stdlib.h>
extern uint32_t g_mem_size;
static uint32_t word(uint32_t a) { uint32_t v=0;if(a<=g_mem_size-4)memcpy(&v,g_mem+a,4);return v; }
static int selected(void) { return g_mem[0x70e66d9]>=19 && g_mem[0x70e66d9]<=20; }
static void state(const char *what,CPU *c) {
    fprintf(stderr,"[cutscene] %s caller=%08x stage=%u request=%x state=%u header=%x count=%x rows=%x index=%x arena=%x+%x head=%x\n",
            what,word(c->esp),g_mem[0x70e66d9],word(0xbb2f5c),word(0xbb2f60),word(0xbb2f64),word(0xbb2f68),word(0xbb2f6c),word(0xbb2f70),word(0xb6bc74),word(0xb6bc78),word(0xb6bc7c));
}
static void invalid(CPU *c) {
    state("NULL_HEADER_BEFORE_CORRUPTION",c);
    const char *out=getenv("SH3_BENCH_OUT");char path[512];
    snprintf(path,sizeof path,"%s/guest-before-corruption.bin",out?out:"build/pc-win");
    FILE *f=fopen(path,"wb");if(f){size_t n=fwrite(g_mem,1,g_mem_size,f);fclose(f);fprintf(stderr,"[cutscene] snapshot=%s bytes=%llu\n",path,(unsigned long long)n);}
    fflush(stderr);exit(36);
}
extern void __real_F_004dc250(CPU *);
void __wrap_F_004dc250(CPU *c) {
    if(selected())state("begin",c);
    __real_F_004dc250(c);
    if(selected())state("begin-return",c);
}
extern void __real_F_004dc2b0(CPU *);
void __wrap_F_004dc2b0(CPU *c) {
    if(selected())state("queue-header",c);
    __real_F_004dc2b0(c);
}
extern void __real_F_004dc2d0(CPU *);
void __wrap_F_004dc2d0(CPU *c) {
    static uint32_t previous[3]={~0u,~0u,~0u};
    uint32_t now[3]={word(0xbb2f5c),word(0xbb2f60),word(0xbb2f64)};
    if(selected() && memcmp(previous,now,sizeof now)){state("tick",c);memcpy(previous,now,sizeof now);}
    if(now[0]!=~0u && now[1]<=1 && !now[2])invalid(c);
    __real_F_004dc2d0(c);
}
extern void __real_F_0049b480(CPU *);
void __wrap_F_0049b480(CPU *c) {
    uint32_t caller=word(c->esp),align=word(c->esp+4),size=word(c->esp+8);
    __real_F_0049b480(c);
    if(selected() || !c->eax)fprintf(stderr,"[cutscene-alloc] caller=%x size=%x align_log2=%u result=%x\n",caller,size,align,c->eax);
}
extern void __real_F_0049b2e0(CPU *);
void __wrap_F_0049b2e0(CPU *c) {
    if(selected())state("arena-reset",c);
    __real_F_0049b2e0(c);
}
