/* Experimental, opt-in projection hook. See diag_widescreen.c for attribution. */
#include "x86rt.h"
#include "diag_widescreen.h"
#include <stdlib.h>

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
    F_00682ba0_orig(c);
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
