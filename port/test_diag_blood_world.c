#include "x86rt.h"
#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
uint8_t *g_mem;
uint32_t g_mem_size=8192;
unsigned g_frame=1,g_diag_perf;
uint32_t g_diag_res_w=960,g_diag_res_h=544;
void diag_blood_note_packet(uint32_t);
void diag_wide_interface_x(CPU *);
void F_0043ab80_orig(CPU *c) {(void)c;}
void F_00402b60_orig(CPU *c) {(void)c;}
void F_00402e00_orig(CPU *c) {(void)c;}
void F_00682ba0_orig(CPU *c) {
    uint32_t x;memcpy(&x,g_mem+c->esp+4,4);
    c->ftop=(c->ftop-1)&7;c->st[c->ftop]=480.0+((double)x/16.0-2048.0)*960.0/512.0;c->esp+=4;
}
static double convert(uint32_t x,uint32_t y,uint32_t z,uint32_t caller) {
    uint32_t stack[2]={caller,x},packed[2]={(x&65535)|(y<<16),z};
    memcpy(g_mem+128,stack,sizeof stack);memcpy(g_mem+2048,packed,sizeof packed);
    CPU cpu={0};cpu.esp=128;cpu.esi=2048;cpu.ftop=3;
    diag_wide_interface_x(&cpu);assert(cpu.esp==132 && cpu.ftop==2);
    return cpu.st[2];
}
int main(int argc,char **argv) {
    assert(argc==2);int enabled=argv[1][0]!='0';
    if(argv[1][0]=='d') {
#ifdef _WIN32
        _putenv_s("SH3_BLOOD_PACKET_PASS","");
#else
        unsetenv("SH3_BLOOD_PACKET_PASS");
#endif
    }
    else setenv("SH3_BLOOD_PACKET_PASS",enabled?"1":"0",1);
    setenv("SH3_BLOOD_AUDIT","1",1);
    setenv("SH3_DIAG_WIDE_NATIVE","1",1);setenv("SH3_DIAG_WIDE_HUD","1",1);
    g_mem=calloc(1,8192);assert(g_mem);
    for(unsigned i=0;i<4;i++) {uint32_t xyz[3]={33289+i*100,34000,0x123450+i*16};memcpy(g_mem+1024+48+32*i,xyz,12);}
    diag_blood_note_packet(1024);
    double native=480+(33289/16.0-2048)*960/512;
    double legacy=480+(native-480)*(4.0/3.0)/(960.0/544.0);
    assert(fabs(convert(33289,34000,0x12345,0x5f6d27)-(enabled?native:legacy))<1e-9);
    /* UI with identical X/Y but another Z, and other converter callsites,
       must retain the original interface layout. No X-only classification. */
    assert(fabs(convert(33289,34000,0x12346,0x5f6d27)-legacy)<1e-9);
    assert(fabs(convert(33289,34000,0x12345,0x5f6d28)-legacy)<1e-9);
    g_frame++;
    assert(fabs(convert(33289,34000,0x12345,0x5f6d27)-legacy)<1e-9);
    diag_blood_note_packet(0x10000000u); /* out of bounds rejected before read */
    free(g_mem);printf("blood world enabled=%d: packed XYZ, UI exclusion and frame expiry PASS\n",enabled);
}
