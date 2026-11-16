/* Mac-only laboratory bridge for the SH4-derived DirectShow implementation.
 * Not linked into the normal diagnostic or Vita application. */
#include "rt.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

typedef struct { CPU cpu; uint32_t raw_stack, tib; } MovieCPU;
CPU *rt_new_cpu(uint32_t size) {
    if (size<16 || size>8u*1024u*1024u) rt_fatal("[movie-lab] invalid guest stack size");
    MovieCPU *o=calloc(1,sizeof *o);
    if (!o) rt_fatal("[movie-lab] CPU allocation failed");
    o->raw_stack=gheap_alloc(size+16); o->tib=gheap_calloc(0x1000);
    if (!o->raw_stack || !o->tib) rt_fatal("[movie-lab] guest stack/TIB allocation failed");
    uint32_t stack=(o->raw_stack+15u)&~15u, empty=0xffffffffu, top=stack+size;
    o->cpu.fs_base=o->tib; o->cpu.esp=top-16; o->cpu.fcw=0x37f;
    memcpy(g_mem+o->tib,&empty,4); memcpy(g_mem+o->tib+4,&top,4);
    memcpy(g_mem+o->tib+8,&stack,4); memcpy(g_mem+o->tib+0x18,&o->tib,4);
    return &o->cpu;
}
void rt_free_cpu(CPU *cpu) {
    if (!cpu) return;
    MovieCPU *o=(MovieCPU *)cpu;
    gheap_free(o->raw_stack); gheap_free(o->tib); free(o);
}
void w32_host_path(const char *guest,char *out,size_t n) {
    if (!n) return;
    out[0]=0;
    if (!guest || strstr(guest,"..")) return;
    const char *base=guest;
    for (const char *p=guest;*p;p++) if (*p=='/' || *p=='\\') base=p+1;
    /* The path is selected only by an explicit laboratory switch and a
       fixed allowlist. No guest string can escape the local build directory. */
    static const char *const names[]={"op","ed_b","ed_n","ed_u","henshin","huxx","muxx"};
    if (getenv("SH3_DIAG_MOVIE_PC_SHORT") && !strcasecmp(base,"op.000")) {
#ifdef __vita__
        const char *path=getenv("SH3_DIAG_MOVIE_PC_SHORT_384") ? "ux0:data/sh3/movie-short-384/op.mpg" : getenv("SH3_DIAG_MOVIE_PC_SHORT_320") ? "ux0:data/sh3/movie-short-320/op.mpg" : "ux0:data/sh3/movie-short/op.mpg";
#else
        const char *path=getenv("SH3_DIAG_MOVIE_PC_SHORT_384") ? "build/pc-movie-lab/converted/sh3-pc-op-10s-384.mpg" : getenv("SH3_DIAG_MOVIE_PC_SHORT_320") ? "build/pc-movie-lab/converted/sh3-pc-op-10s-320.mpg" : "build/pc-movie-lab/converted/sh3-pc-op-10s.mpg";
#endif
        int len=snprintf(out,n,"%s",path);
        if (len<0 || (size_t)len>=n) out[0]=0;
    } else if (getenv("SH3_DIAG_MOVIE_PC")) for (unsigned i=0;i<sizeof names/sizeof names[0];i++) {
        char expected[32]; snprintf(expected,sizeof expected,"%s.000",names[i]);
        /* Lab (marker movieas=<name>): the title/attract op.000 plays another allowlisted film, so every film
           can be checked from the title screen, twice in a row through the attract loop. */
        const char *as=getenv("SH3_DIAG_MOVIE_AS");
        if (as && !strcasecmp(base,"op.000") && !strcasecmp(as,names[i])) snprintf(expected,sizeof expected,"op.000");
        else if (as && !strcasecmp(base,"op.000")) continue;
        if (!strcasecmp(base,expected)) {
#ifdef __vita__
            int len=snprintf(out,n,"ux0:data/sh3/movie/%s.mpg",names[i]);
#else
            int len=snprintf(out,n,"build/pc-movie-lab/%s.mpg",names[i]);
#endif
            if (len<0 || (size_t)len>=n) out[0]=0;
            break;
        }
    }
    else if (!strcasecmp(base,"op.000") && getenv("SH3_DIAG_MOVIE_SAMPLE"))
        snprintf(out,n,"build/pc-movie-lab/converted/op-10s.mpg");
    fprintf(stderr,"[movie-lab] path %.200s -> %.200s\n",guest,out);
}
