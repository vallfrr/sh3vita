/* Actual device.c COM entry points with a CPU-only resource/queue fixture.
   No GL context, game files or copied commercial code needed. */
#include <assert.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include "stateblock-device.inc"
uint8_t *g_mem;uint32_t g_mem_size=65536;
static Device device;
static Texture ta,tb;static ComObj ca;
void rt_fatal(const char *fmt,...){(void)fmt;abort();}
void rt_log(const char *fmt,...){(void)fmt;}
void d3d_log_once(const char *s){(void)s;}
ComObj *com_get(uint32_t a){if(a==0x100)return &device.com;if(a==0x200)return &ta.com;if(a==0x300)return &tb.com;if(a==0x400)return &ca;return NULL;}
void *com_get_kind(uint32_t a,int k){ComObj *p=com_get(a);return p && p->kind==k?p:NULL;}
void d3d_addref(void *v){if(v)((ComObj *)v)->refs++;}
void d3d_release(ComObj *p){if(p){assert(p->refs>1);p->refs--;}}
void diag_cube_release_obj(ComObj *p){assert(p && p->refs>1);p->refs--;}
int prep_set(void *field,const void *src,size_t n){int changed=memcmp(field,src,n)!=0;memcpy(field,src,n);return changed;}
static CPU cpu;
static void call(gfn fn,unsigned n,...){
 cpu.esp=0x8000;uint32_t args[8]={0};args[0]=0x100;va_list ap;va_start(ap,n);for(unsigned i=1;i<n;i++)args[i]=va_arg(ap,unsigned);va_end(ap);
 memcpy(g_mem+cpu.esp+4,args,4*n);fn(&cpu);
}
static void bind(unsigned stage,unsigned object){call(dev_SetTexture,3,stage,object);assert(cpu.eax==HR_OK);}
static unsigned end(void){call(dev_EndStateBlock,2,0x7000u);assert(cpu.eax==HR_OK);uint32_t id;memcpy(&id,g_mem+0x7000,4);return id;}
int main(void){
 g_mem=calloc(1,g_mem_size);assert(g_mem);g_dev=&device;device.com=(ComObj){0x100,1,K_DEVICE};ta.com=(ComObj){0x200,1,K_TEXTURE};tb.com=(ComObj){0x300,1,K_TEXTURE};ca=(ComObj){0x400,1,K_CUBE};
 bind(0,0x200);assert(ta.com.refs==2);
 device.tss[1][7]=0x3f800000;
 call(dev_BeginStateBlock,1);assert(cpu.eax==HR_OK);
 call(dev_SetTextureStageState,4,1u,1u,4u);call(dev_SetTextureStageState,4,1u,1u,5u);
 assert(device.tss[1][1]==0 && device.tss[1][7]==0x3f800000);
 bind(0,0);bind(1,0x300);bind(3,0x400);
 assert(device.tex[0]==&ta && !device.tex[1] && !device.cube[3]);
 call(dev_BeginStateBlock,1);assert(cpu.eax==D3DERR_INVALIDCALL);
 unsigned id=end();assert(blocks[id]->texmask==11 && tb.com.refs==2 && ca.refs==2);
 bind(2,0x200);
 for(unsigned i=0;i<1000;i++)call(dev_ApplyStateBlock,2,id);
 assert(device.tss[1][1]==5 && device.tss[1][7]==0x3f800000);
 assert(!device.tex[0] && device.tex[1]==&tb && device.tex[2]==&ta && device.cube[3]==&ca);
 assert(ta.com.refs==2 && tb.com.refs==3 && ca.refs==3);
 bind(0,0x200);bind(1,0);bind(3,0);
 device.tss[1][1]=7;device.tss[1][7]=0x40000000;
 call(dev_CaptureStateBlock,2,id);assert(cpu.eax==HR_OK);
 assert(tb.com.refs==1 && ca.refs==1 && ta.com.refs==4);
 device.tss[1][1]=9;device.tss[1][7]=0x40400000;
 bind(0,0);call(dev_ApplyStateBlock,2,id);assert(device.tss[1][1]==7 && device.tss[1][7]==0x40400000);assert(device.tex[0]==&ta && device.tex[2]==&ta && !device.tex[1]);
 call(dev_DeleteStateBlock,2,id);assert(ta.com.refs==3);
 bind(0,0);bind(2,0);assert(ta.com.refs==1);
 call(dev_EndStateBlock,2,0x7000u);assert(cpu.eax==D3DERR_INVALIDCALL);
 for(unsigned i=1;i<256;i++){call(dev_BeginStateBlock,1);bind(1,0x300);assert(end()==i);}
 assert(tb.com.refs==256);call(dev_BeginStateBlock,1);bind(1,0x300);call(dev_EndStateBlock,2,0x7000u);assert(cpu.eax==0x8007000eu && tb.com.refs==256);
 for(unsigned i=1;i<256;i++)call(dev_DeleteStateBlock,2,i);
 assert(tb.com.refs==1 && !recording && !record_texmask);
 free(g_mem);puts("PASS: recorded/NULL/cube masks, no recording side effects on texture bindings, capture/apply refs, block exhaustion cleanup");return 0;
}
