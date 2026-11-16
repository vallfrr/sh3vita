/* Host-only tests for the experimental DirectSound staging lock. */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "sh4base/dx/dsound.c"

uint8_t *g_mem;
uint32_t g_mem_size=65536;
static DSBuf test_buf;
static int freed;
void *com_get_kind(uint32_t guest,int kind) { return guest==0x100 && kind==K_DSBUF ? &test_buf : NULL; }
uint32_t gheap_alloc(uint32_t n) { return n<=256 ? 0x4000 : 0; }
void gheap_free(uint32_t p) { assert(p==0x4000); freed++; }
_Noreturn void rt_fatal(const char *fmt, ...) { (void)fmt; abort(); }
static void call(CPU *c,void (*fn)(CPU *),const uint32_t *args,unsigned n) {
    c->esp=0x1000; memcpy(g_mem+c->esp+4,args,4*n); fn(c); assert(c->esp==0x1004+4*n);
}
static uint32_t rd(uint32_t p) { uint32_t v; memcpy(&v,g_mem+p,4); return v; }
int main(void) {
    g_mem=calloc(1,g_mem_size); assert(g_mem);
    mx=SDL_CreateMutex(); assert(mx); test_buf.mem=0x2000; test_buf.size=16;
    for (unsigned i=0;i<16;i++) g_mem[test_buf.mem+i]=(uint8_t)i;
    CPU c={0}; uint32_t lock[]={0x100,12,8,0x3000,0x3004,0x3008,0x300c,0};
    call(&c,buf_Lock,lock,8); assert(c.eax==0);
    uint32_t p1=rd(0x3000),n1=rd(0x3004),p2=rd(0x3008),n2=rd(0x300c);
    assert(p1==0x4000 && n1==4 && p2==0x4004 && n2==4);
    assert(!memcmp(g_mem+p1,g_mem+0x200c,4) && !memcmp(g_mem+p2,g_mem+0x2000,4));
    memset(g_mem+p1,0xa1,4); memset(g_mem+p2,0xb2,4);
    assert(g_mem[0x200c]==12 && g_mem[0x2000]==0);
    uint32_t bad[]={0x100,p1,n1,p2,n2+1}; call(&c,buf_Unlock,bad,5); assert(c.eax==D3DERR_INVALIDCALL && !freed);
    uint32_t unlock[]={0x100,p1,n1,p2,n2}; call(&c,buf_Unlock,unlock,5); assert(c.eax==0 && freed==1);
    for (unsigned i=0;i<4;i++) { assert(g_mem[0x200c+i]==0xa1); assert(g_mem[0x2000+i]==0xb2); }
    assert(!test_buf.lock_copy); SDL_DestroyMutex(mx); free(g_mem); puts("DirectSound staged wraparound lock checks passed");
}
