#include "x86rt.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
uint8_t *g_mem;
static unsigned crt_seen[30];
void rt_call(CPU *c,uint32_t t){(void)c;fprintf(stderr,"Unexpected external jump %08x\n",t);abort();}
#include "crt-copy-function.inc"
static unsigned count;
static void put(uint8_t *m,unsigned a,uint32_t v){memcpy(m+a,&v,4);}
static void test(unsigned src,unsigned dst,unsigned n){
 uint8_t reference[0x4000];for(unsigned i=0;i<sizeof reference;i++)g_mem[i]=(uint8_t)(i*73+i/17+count*19);
 CPU c={0};c.esp=0x3800;c.ebp=0x12345678;c.edi=0xabcdef02;c.esi=0x12345601;c.ebx=0xfedcba98;c.fcw=0x37f;
 put(g_mem,c.esp,0x12345678);put(g_mem,c.esp+4,dst);put(g_mem,c.esp+8,src);put(g_mem,c.esp+12,n);
 memcpy(reference,g_mem,sizeof reference);put(reference,c.esp-4,c.ebp);put(reference,c.esp-8,c.edi);put(reference,c.esp-12,c.esi);
 memmove(reference+dst,reference+src,n);F_00661d20(&c);
 if(memcmp(reference,g_mem,sizeof reference)){fprintf(stderr,"Mismatch src=%u dst=%u n=%u\n",src,dst,n);abort();}
 assert(c.esp==0x3804 && c.eax==dst && c.ebp==0x12345678 && c.edi==0xabcdef02 && c.esi==0x12345601 && c.ebx==0xfedcba98 && !c.df && c.fcw==0x37f);count++;
}
int main(int argc,char **argv){
 assert(argc==2);g_mem=calloc(1,0x800000);assert(g_mem);FILE *f=fopen(argv[1],"rb");assert(f);assert(fread(g_mem+0x661d20,1,0x340,f)==0x340);fclose(f);
 int gaps[]={-65,-33,-17,-9,-7,-3,-2,-1,0,1,2,3,7,9,17,33,65};
 unsigned extra[]={129,255,256,257,511,512,513,1023,1024,4095,4096};
 for(unsigned index=0;index<129+sizeof extra/sizeof extra[0];index++){
  unsigned n=index<129?index:extra[index-129];
  for(unsigned sa=0;sa<8;sa++){
   for(unsigned da=0;da<8;da++){test(512+sa,8192+da,n);test(8192+sa,512+da,n);}
   for(unsigned g=0;g<sizeof gaps/sizeof gaps[0];g++)test(512+sa,(unsigned)(512+(int)sa+gaps[g]),n);
  }
 }
 for(unsigned i=0;i<30;i++){if(!crt_seen[i]){fprintf(stderr,"Uncovered dispatch target %u\n",i);abort();}}
 printf("PASS: %u copies vs host memmove, all 30 internal destinations, cdecl ABI and DF preserved\n",count);free(g_mem);return 0;
}
