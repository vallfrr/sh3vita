#!/usr/bin/env python3
"""Compare bounded reads to the actual retained decoder on mixed zlib/raw blocks.

The fixture replaces only allocation, file I/O and the zlib ABI bridge. It also
reproduces the fragmented-heap failure with a fixed scratch-allocation ceiling.
"""
from pathlib import Path
import subprocess
import tempfile


def main():
    root = Path(__file__).resolve().parents[1]
    source = (root / 'port/gen/x86_f146.c').read_text()
    start = source.index('void F_00608f80(CPU *c) {')
    body = source[start:source.index('\n}', start) + 2]
    body = body.replace('void F_00608f80(', 'void diag_archive_read_retained(', 1)
    fixture = r'''
#include "x86rt.h"
#include <stdio.h>
#include <stdlib.h>
#include <zlib.h>
uint8_t *g_mem;
uint32_t g_mem_size=40u*1024u*1024u;
static uint8_t *physical,*plain;
static size_t physical_size,physical_pos,scratch_limit,scratch_peak;
static unsigned malloc_failures;
enum { OBJ=0x100000, TABLE=0x110000, DEST=0x200000, SCRATCH=0x1800000, SP=0x30000 };
void F_00663014(CPU *c) {
    uint8_t *mb=g_mem;unsigned size=R32(c->esp+4);
    if(size>scratch_peak)scratch_peak=size;
    c->eax=size>scratch_limit?0:SCRATCH;
    if(!c->eax)malloc_failures++;
    c->esp+=4;
}
void F_00662fad(CPU *c) { c->esp+=4; }
void F_0041b6a0(CPU *c) { c->eax=0;c->esp+=4; }
void imp_KERNEL32_Sleep(CPU *c) { c->eax=0;c->esp+=8; }
void imp_KERNEL32_SetFilePointer(CPU *c) {
    uint8_t *mb=g_mem;unsigned offset=R32(c->esp+8);
    physical_pos=offset;c->eax=offset;c->esp+=20;
}
void imp_KERNEL32_ReadFile(CPU *c) {
    uint8_t *mb=g_mem;unsigned dst=R32(c->esp+8),n=R32(c->esp+12),count=R32(c->esp+16);
    if(physical_pos>physical_size || n>physical_size-physical_pos)abort();
    memcpy(mb+dst,physical+physical_pos,n);physical_pos+=n;W32(count,n);c->eax=1;c->esp+=24;
}
void F_0067b6b0(CPU *c) {
    uint8_t *mb=g_mem;uint32_t dst=R32(c->esp+4),lenptr=R32(c->esp+8),src=R32(c->esp+12),n=R32(c->esp+16);
    uLongf length=R32(lenptr);
    c->eax=uncompress(mb+dst,&length,mb+src,n);W32(lenptr,(uint32_t)length);c->esp+=4;
}
'''
    driver = r'''
static unsigned file_length;
static void setup_file(unsigned length,unsigned mode) {
    uint8_t *mb=g_mem;file_length=length;physical_size=0;
    for(unsigned i=0;i<length;i++)plain[i]=(uint8_t)((i*17u+(i>>8)*3u)^(i>>16));
    for(unsigned b=0;b<(length+65535u)/65536u;b++) {
        unsigned size=length-b*65536u;if(size>65536)size=65536;
        if(mode==1 || (mode==2 && (b&1))) {
            uLongf compressed=compressBound(size);
            if(compress2(physical+physical_size,&compressed,plain+b*65536u,size,1))abort();
            W32(TABLE+4*b,(unsigned)compressed);physical_size+=compressed;
        } else { W32(TABLE+4*b,0);memcpy(physical+physical_size,plain+b*65536u,size);physical_size+=size; }
    }
}
static CPU initial(unsigned pos,unsigned request) {
    uint8_t *mb=g_mem;memset(mb+OBJ,0,64);W32(OBJ+0xc,0x1234);W32(OBJ+0x10,pos);
    W32(OBJ+0x14,file_length);W32(OBJ+0x18,0);W32(OBJ+0x1c,TABLE);
    W32(SP,0x601ce1);W32(SP+4,DEST);W32(SP+8,request);
    CPU c={.eax=0x12345678,.ecx=OBJ,.edx=0x76543210,.ebx=0x11223344,
           .esp=SP,.ebp=0x55667788,.esi=0xabcdef01,.edi=0x87654321,.fcw=0x37f};return c;
}
int main(void) {
    g_mem=calloc(1,g_mem_size);physical=malloc(16u*1024*1024);plain=malloc(12u*1024*1024);
    uint8_t *reference=malloc(12u*1024*1024);unsigned cases=0;
    unsigned sizes[]={1,2047,2048,65535,65536,65537,131071,131072,0x100100,0x914000};
    for(unsigned mode=0;mode<3;mode++)for(unsigned si=0;si<sizeof sizes/sizeof *sizes;si++) {
        unsigned length=sizes[si];setup_file(length,mode);
        unsigned positions[]={0,65536,length>65536?length-65536:0};
        unsigned requests[]={0,1,2048,65535,65536,65537,131072,length,length+0x10000};
        for(unsigned pi=0;pi<3;pi++)for(unsigned ri=0;ri<sizeof requests/sizeof *requests;ri++) {
            unsigned pos=positions[pi],request=requests[ri],span=(request>length?request:length)+65536;
            memset(g_mem+DEST,0xa5,span);CPU a=initial(pos,request);
            physical_pos=0;scratch_limit=16u*1024*1024;scratch_peak=0;
            diag_archive_read_retained(&a);unsigned refpos=*(uint32_t *)(g_mem+OBJ+0x10);
            memcpy(reference,g_mem+DEST,span);
            memset(g_mem+DEST,0xa5,span);CPU b=initial(pos,request);
            physical_pos=0;scratch_peak=0;F_00608f80(&b);
            if(a.eax!=b.eax || a.esp!=b.esp || a.ebx!=b.ebx || a.ebp!=b.ebp ||
               a.esi!=b.esi || a.edi!=b.edi || refpos!=*(uint32_t *)(g_mem+OBJ+0x10) ||
               memcmp(reference,g_mem+DEST,span)) {
                fprintf(stderr,"Mismatch mode=%u length=%x pos=%x request=%x return=%x/%x\n",mode,length,pos,request,a.eax,b.eax);return 1;
            }
            ++cases;
        }
    }
    /* Exact root-failure shape: plenty of total storage but no contiguous
       scratch allocation above 128 KiB. Original full-file allocation fails. */
    setup_file(0x914000,0);scratch_limit=0x20000;scratch_peak=0;malloc_failures=0;
    CPU old=initial(0,0x920000);diag_archive_read_retained(&old);
    if(old.eax || !malloc_failures)return 2;
    memset(g_mem+DEST,0xa5,0x930000);CPU bounded=initial(0,0x920000);scratch_peak=0;physical_pos=0;
    F_00608f80(&bounded);
    if(bounded.eax!=0x914000 || scratch_peak>0x20000 || memcmp(g_mem+DEST,plain,0x914000))return 3;
    for(unsigned i=0x914000;i<0x920000;i++)if(g_mem[DEST+i])return 4;
    printf("%u retained-decoder comparisons PASS; original OOM reproduced, bounded scratch peak=%zu\n",cases,scratch_peak);
    free(reference);free(plain);free(physical);free(g_mem);return 0;
}
'''
    with tempfile.TemporaryDirectory(prefix='sh3-archive-stream-') as directory:
        cfile = Path(directory) / 'archive.c'
        cfile.write_text(fixture + body + '\n' + (root / 'port/diag_archive_stream.c').read_text() + driver)
        for flags in (['-O1'], ['-O2', '-fsanitize=address,undefined', '-fno-omit-frame-pointer']):
            binary = Path(directory) / 'archive'
            subprocess.run(['cc', '-std=gnu11', *flags, '-I' + str(root / 'port/include'), str(cfile), '-lz', '-lm', '-o', str(binary)], check=True)
            subprocess.run([str(binary)], check=True)


if __name__ == '__main__':
    main()
