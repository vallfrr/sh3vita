#!/usr/bin/env python3
"""Compare actual COM state method and generated shortcut including CPU/stack/prep records."""
import argparse,re,subprocess,sys
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT/'tools'))
from renderstate_shortcut_unit import CALL,transform

def body(path,token):
 s=path.read_text();a=s.index(token);b=s.index('{',a);i=b+1;depth=1
 while depth:depth+=(s[i]=='{')-(s[i]=='}');i+=1
 return s[b:i]

TEMPLATE = r'''
#include "x86rt.h"
#include <stdio.h>
#include <stdlib.h>
uint8_t *g_mem;
uint32_t g_diag_renderstate_target=0xf1000010u;
typedef struct {uint32_t rs[256];} Device;
static Device device;
static uint32_t records[16];static unsigned nr;static int allow;
#define K_DEVICE 2
static void *com_get_kind(uint32_t object,int kind){return object==0x900u && kind==K_DEVICE?&device:NULL;}
static int prep_set(void *dst,const void *src,size_t n){if(!memcmp(dst,src,n))return 0;records[nr++]=(uint32_t)((char*)dst-(char*)&device);memcpy(&records[nr++],src,4);memcpy(dst,src,n);return 1;}
#define HOSTFN uint8_t *const mb=g_mem
#define ARG(n) R32(c->esp+4u*(n))
#define DEV Device *d=com_get_kind(ARG(1),K_DEVICE);if(!d)abort()
#define STDRET(n,v) do{c->eax=(v);c->esp+=4u+4u*(n);return;}while(0)
#define HR_OK 0
static void native_com(CPU *c) @METHOD@
int diag_d3d_renderstate(uint32_t object,uint32_t state,uint32_t value) @HELPER@
static int diag_renderstate_shortcut(CPU*c,uint32_t target,uint32_t object,uint32_t state,uint32_t value){(void)c;return allow && target==g_diag_renderstate_target && diag_d3d_renderstate(object,state,value);}
void rt_call(CPU*c,uint32_t t){if(t==g_diag_renderstate_target){native_com(c);return;}c->eax^=0xdeadbeefu;c->ecx+=17;c->edx^=3;c->esp+=16;c->ftop=(c->ftop+1)&7;c->fsw^=0x4500u;}

@REFERENCE@
@CANDIDATE@

static uint32_t seed=0x12345678;
static uint32_t next(void){seed^=seed<<13;seed^=seed>>17;seed^=seed<<5;return seed;}
int main(void){static uint8_t arena[65536],initial_mem[65536],expected_mem[65536];static Device before,expected;CPU initial,a,b;uint32_t expected_records[16];
 for(unsigned i=0;i<12000;i++){
  for(unsigned k=0;k<sizeof initial;k++)((uint8_t*)&initial)[k]=(uint8_t)next();initial.esp=0x9000u;initial.edx=0x2000;initial.ftop&=7;
  for(unsigned k=0;k<256;k++)before.rs[k]=next();memset(initial_mem,0x5a,sizeof initial_mem);g_mem=initial_mem;uint8_t *const mb=g_mem;
  uint32_t target=i%3?g_diag_renderstate_target:0xf1000088u;W32(0x20c8,target);W32(initial.esp,0x900u);W32(initial.esp+4,i%4?next()%256:next());W32(initial.esp+8,next());
  if(i%5==0){uint32_t state=R32(initial.esp+4);if(state<256)W32(initial.esp+8,before.rs[state]);}
  a=initial;device=before;nr=0;memset(records,0,sizeof records);memcpy(arena,initial_mem,sizeof arena);g_mem=arena;reference(&a);expected=device;unsigned expected_nr=nr;memcpy(expected_records,records,sizeof records);memcpy(expected_mem,arena,sizeof arena);
  b=initial;device=before;nr=0;memset(records,0,sizeof records);memcpy(arena,initial_mem,sizeof arena);allow=i%2;candidate(&b);
  if(memcmp(&a,&b,sizeof a)||memcmp(&expected,&device,sizeof device)||memcmp(expected_mem,arena,sizeof arena)||nr!=expected_nr||memcmp(records,expected_records,sizeof records)){fprintf(stderr,"Mismatch %u\n",i);return 1;}
 }
 puts("PASS 12000 CPU/64KiB-arena/device/prep-record comparisons, shortcut/fallback/unchanged/out-of-range states");return 0;}
'''

def main():
 ap=argparse.ArgumentParser();ap.add_argument('--cc',default='gcc');ap.add_argument('--out',type=Path,default=ROOT/'build/perf30/renderstate/differential');a=ap.parse_args()
 source=(ROOT/'port/gen/x86_f046.c').read_text();m=next(m for m in CALL.finditer(source) if '+0xc8u' in m['head']);original=m[0]
 fake='#include "x86rt.h"\nvoid F_00000000(CPU *c) {\n'+original+'\n}'
 changed=transform(fake)[0];replacement=changed[changed.index(original[:30]):changed.rindex('\n}')]
 start='uint8_t *const mb=g_mem;uint32_t eax=c->eax,ecx=c->ecx,edx=c->edx,ebx=c->ebx,esp=c->esp,ebp=c->ebp,esi=c->esi,edi=c->edi,ft0=c->ftop;'
 end='c->eax=eax;c->ecx=ecx;c->edx=edx;c->ebx=ebx;c->esp=esp;c->ebp=ebp;c->esi=esi;c->edi=edi;c->ftop=ft0;'
 output=TEMPLATE.replace('@METHOD@',body(ROOT/'port/sh4base/d3d8/device.c','M(SetRenderState)')).replace('@HELPER@',body(ROOT/'port/sh4base/d3d8/device.c','int diag_d3d_renderstate('))
 output=output.replace('@REFERENCE@','static void reference(CPU*c){'+start+original+end+'}').replace('@CANDIDATE@','static void candidate(CPU*c){'+start+replacement+end+'}')
 a.out.mkdir(parents=True,exist_ok=True);c=a.out/'test.c';exe=a.out/'test';c.write_text(output)
 subprocess.run([a.cc,'-O2','-fsanitize=address,undefined','-I'+str(ROOT/'port/include'),str(c),'-o',str(exe),'-lm'],check=True)
 subprocess.run([str(exe)],check=True)
if __name__=='__main__':main()
