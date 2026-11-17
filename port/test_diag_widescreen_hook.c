#include "x86rt.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
unsigned g_diag_perf,g_frame;
static uint8_t memory[4096];
uint8_t *g_mem=memory;
uint32_t g_mem_size=sizeof memory,g_diag_res_w=960,g_diag_res_h=544;
int diag_cursor_screen_active(void) { return 0; }
void diag_wide_scale_x(CPU *c);
void diag_wide_render_setter(CPU *c);
void diag_wide_render_reset(CPU *c);
void diag_wide_interface_x(CPU *c);
void F_0043ab80_orig(CPU *c) { c->ftop=(c->ftop-1)&7; c->st[c->ftop]=2.0; c->esp+=4; }
void F_00402b60_orig(CPU *c) { c->eax=11; c->esp+=4; }
void F_00402e00_orig(CPU *c) { c->eax=12; c->esp+=4; }
void F_00682ba0_orig(CPU *c) { c->ftop=(c->ftop-1)&7; c->st[c->ftop]=0.0; c->esp+=4; }
int main(void) {
    CPU c={0}; c.ftop=3; c.esp=100;
    unsetenv("SH3_DIAG_WIDE_FOV"); diag_wide_scale_x(&c);
    assert(c.ftop==2 && c.esp==104 && c.st[2]==2.0);
    c.ftop=3; c.esp=100; setenv("SH3_DIAG_WIDE_FOV","1",1); diag_wide_scale_x(&c);
    assert(c.ftop==2 && c.esp==104 && fabs(c.st[2]-2.0*(4.0/3.0)/(960.0/544.0))<1e-6);
    unsetenv("SH3_DIAG_WIDE_NATIVE"); c.esp=100; c.eax=0; diag_wide_render_setter(&c);
    assert(c.esp==104 && c.eax==11);
    c.esp=100; diag_wide_render_reset(&c); assert(c.esp==104 && c.eax==12);
    setenv("SH3_DIAG_WIDE_NATIVE","1",1); c.esp=100; c.eax=17; diag_wide_render_setter(&c);
    assert(c.esp==104 && c.eax==17);
    c.esp=100; diag_wide_render_reset(&c); assert(c.esp==104 && c.eax==17);
    c.ftop=3; c.esp=100; setenv("SH3_DIAG_WIDE_HUD","1",1); diag_wide_interface_x(&c);
    assert(c.ftop==2 && c.esp==104 && fabs(c.st[2]-(480.0-544.0*2.0/3.0))<1e-6);
    g_diag_perf=1024;
    c.ftop=3;c.esp=100;diag_wide_interface_x(&c);
    assert(c.ftop==2 && c.esp==104 && fabs(c.st[2]-(480.0-544.0*2.0/3.0))<1e-6);
    /* Runtime options are immutable after startup; cached path retains them. */
    unsetenv("SH3_DIAG_WIDE_NATIVE");unsetenv("SH3_DIAG_WIDE_HUD");
    c.ftop=3;c.esp=100;diag_wide_interface_x(&c);
    assert(fabs(c.st[2]-(480.0-544.0*2.0/3.0))<1e-6);
    g_diag_perf=0;c.ftop=3;c.esp=100;diag_wide_interface_x(&c);assert(c.st[2]==0.0);
    puts("widescreen wrappers: dynamic and cached options passed");
}
