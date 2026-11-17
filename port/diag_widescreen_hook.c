/* Experimental, opt-in projection hook. See diag_widescreen.c for attribution. */
#include "x86rt.h"
#include "diag_widescreen.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
extern unsigned g_frame;
extern uint32_t g_mem_size;
#ifndef SH3_BLOOD_WORLD_DEFAULT
#define SH3_BLOOD_WORLD_DEFAULT 0
#endif
/* The native blood producers emit unpacked GS XYZ2 packets. The generic GS
   decoder copies each vertex to its stack, packs XY into 16-bit halves and
   shifts Z right by four. Its caller is shared with UI, so a caller-only
   exception would incorrectly stretch text. Track exact compacted XYZ in the
   current frame instead. No shader/texture heuristic, matrix rewrite or
   generated game source edit is involved. */
static unsigned blood_frame,blood_count;
static uint32_t blood_xyz[4096][2];
static int blood_enabled(void) {
    static int value=-1;
    if(value<0){const char *v=getenv("SH3_BLOOD_PACKET_PASS");value=v?(*v=='1'):SH3_BLOOD_WORLD_DEFAULT;}
    return value;
}
static int blood_audit(void) {
    static int value=-1;
    if(value<0)value=getenv("SH3_BLOOD_AUDIT")!=NULL;
    return value;
}
void diag_blood_note_packet(uint32_t p) {
    if(!blood_enabled() && !blood_audit())return;
    if(g_mem_size<160 || p>g_mem_size-160)return;
    if(blood_frame!=g_frame){blood_frame=g_frame;blood_count=0;}
    if(blood_count>4096-4)return; /* fail closed: no unmatched geometry is widened */
    for(unsigned i=0;i<4;i++) {
        uint32_t xyz[3];memcpy(xyz,g_mem+p+48+32*i,12);
        blood_xyz[blood_count][0]=(xyz[0]&65535u)|(xyz[1]<<16);
        blood_xyz[blood_count][1]=xyz[2]>>4;
        blood_count++;
    }
}
static int blood_vertex(uint32_t address) {
    if(blood_frame!=g_frame || g_mem_size<8 || address>g_mem_size-8)return 0;
    uint32_t packed[2];memcpy(packed,g_mem+address,8);
    for(unsigned i=0;i<blood_count;i++)
        if(blood_xyz[i][0]==packed[0] && blood_xyz[i][1]==packed[1])return 1;
    return 0;
}

extern void F_0043ab80_orig(CPU *c);
extern void F_00402b60_orig(CPU *c);
extern void F_00402e00_orig(CPU *c);
extern void F_00682ba0_orig(CPU *c);
/* These three options are fixed during runtime startup, before guest code.
   The lab bit preserves the old getenv path for a same-binary comparison. */
extern unsigned g_diag_perf;
static int wide_flag(unsigned index) {
    static const char *names[]={"SH3_DIAG_WIDE_FOV","SH3_DIAG_WIDE_NATIVE","SH3_DIAG_WIDE_HUD"};
    static int cached[]={-1,-1,-1};
    if (!(g_diag_perf&1024u)) return getenv(names[index])!=NULL;
    if (cached[index]<0) cached[index]=getenv(names[index])!=NULL;
    return cached[index];
}
void diag_wide_scale_x(CPU *c) {
    F_0043ab80_orig(c);
    /* The original leaves its native projection scale on x87 ST(0).
       The target aspect must also match the render surface: the hook alone
       does not make the diagnostic render widescreen. */
    if (wide_flag(0))
        c->st[c->ftop & 7u]=diag_wide_projection_x((float)c->st[c->ftop & 7u],960.0f,544.0f);
}
/* FusionFix disables the game's render-size setters while its resolution
   override is active. Keep this isolated until UI/graphics are validated. */
void diag_wide_render_setter(CPU *c) {
    if (wide_flag(1)) c->esp+=4;
    else F_00402b60_orig(c);
}
void diag_wide_render_reset(CPU *c) {
    if (wide_flag(1)) c->esp+=4;
    else F_00402e00_orig(c);
}
/* Interface elements use a centered 4:3 region. This preliminary wrapper
   applies to all callers; full-screen effects need separate classification. */
/* The native particle draw (particle_draw_native.c) inlines F_00682ba0 and refuses to run while this hook would alter it. */
int diag_wide_interface_active(void) { return wide_flag(1) && wide_flag(2); }
void diag_wide_interface_x(CPU *c) {
    uint32_t ret=0,arg=0;
    memcpy(&ret,g_mem+c->esp,4);memcpy(&arg,g_mem+c->esp+4,4);
    F_00682ba0_orig(c);
    if(ret==0x5f6d27u && wide_flag(1) && wide_flag(2) &&
       (blood_enabled() || blood_audit()) && blood_vertex(c->esi)) {
        double native=c->st[c->ftop&7u];
        if(blood_audit()) {
            static unsigned lines;
            if(lines++<20000)fprintf(stderr,"[blood-world] frame=%u x=%u native=%.12g actual=%.12g enabled=%d\n",
                g_frame,arg,native,blood_enabled()?native:480.0+(native-480.0)*((4.0/3.0)/(960.0/544.0)),blood_enabled());
        }
        if(blood_enabled())return;
    }
    /* The 4:3 squeeze below is for interface elements only. Sprites anchored in the 3D world (lamp halos, particles incl.
       blood drops, lens flare) already use the widened projection (diag_wide_scale_x): squeezing them again slid them
       sideways against the scene as the camera moved (Valentin, 2026-11-16: blood on the floor "moves by itself").
       Callers (return address after the call): F_00468b40/F_00468df0 (halos), F_004749b0..F_00474d70 (light sprites),
       F_004794b0/F_00479e30 (particles), F_0049fac0 (lens), F_004ddae0 and F_00599b70 (sprites placed with the 3D projection F_0043ab80, e.g. blood). */
    if ((ret>=0x468b40u && ret<0x468fa0u) || (ret>=0x4749b0u && ret<0x474d70u) || (ret>=0x4794b0u && ret<0x47a8b0u) || (ret>=0x49fac0u && ret<0x4a0001u) ||
        (ret>=0x4ddae0u && ret<0x4dde20u) || (ret>=0x599b70u && ret<0x599ea0u)) return;
    if (wide_flag(1) && wide_flag(2)) {
        double x=c->st[c->ftop&7u];
        c->st[c->ftop&7u]=480.0+(x-480.0)*((4.0/3.0)/(960.0/544.0));
    }
}

/* Fixed console mode must survive the PC Options aspect-ratio filter.
   Otherwise its current-mode lookup returns -1 and reads before the mode table. */
int diag_console_display_mode(uint32_t w,uint32_t h) {
    extern uint32_t g_diag_res_w,g_diag_res_h;
    return wide_flag(1) && w==g_diag_res_w && h==g_diag_res_h;
}
