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
/* Registry of the current frame's world-projected vertices: open addressing on the compacted key, an entry is valid only for the
   frame that wrote it (no clearing). Full probe = not recorded = interface squeeze kept (fail closed). */
#define WORLD_SLOTS 16384u
static struct { uint32_t frame,xy,z; } world[WORLD_SLOTS];
static unsigned world_hash(uint32_t xy,uint32_t z) { return (unsigned)((xy*2654435761u)^(z*40503u))&(WORLD_SLOTS-1u); }
static void world_add(uint32_t xy,uint32_t z) {
    unsigned h=world_hash(xy,z);
    for(unsigned i=0;i<32;i++,h=(h+1)&(WORLD_SLOTS-1u)) {
        if(world[h].frame!=g_frame+1u){world[h].frame=g_frame+1u;world[h].xy=xy;world[h].z=z;return;}   /* +1: frame 0 is a real frame */
        if(world[h].xy==xy && world[h].z==z)return;
    }
}
static int world_has(uint32_t xy,uint32_t z) {
    unsigned h=world_hash(xy,z);
    for(unsigned i=0;i<32;i++,h=(h+1)&(WORLD_SLOTS-1u)) {
        if(world[h].frame!=g_frame+1u)return 0;
        if(world[h].xy==xy && world[h].z==z)return 1;
    }
    return 0;
}
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
    for(unsigned i=0;i<4;i++) {
        uint32_t xyz[3];memcpy(xyz,g_mem+p+48+32*i,12);
        world_add((xyz[0]&65535u)|(xyz[1]<<16),xyz[2]>>4);
    }
}
/* Every world-to-screen projection of the game's effect system (F_0059ef90: blood, footprints and the other floor/air effects of
   F_004f0xxx and F_005b5xxx..F_005caxxx) writes its integer GS X, Y, Z to out[0..2]; record them all, not only the blood producers
   (Valentin, 2026-11-17: footprints slid like the blood and the lamp halos did). */
static struct { uint32_t xy,z,f; } note_ring[8]; static unsigned note_n;   /* diagnostic: last recorded world vertices */
void diag_world_note_xyz(uint32_t p) {
    if(!blood_enabled() && !blood_audit())return;
    if(g_mem_size<16 || p>g_mem_size-16)return;
    uint32_t xyz[4];memcpy(xyz,g_mem+p,16);
    if(blood_audit()) { static unsigned ok,bad; if(xyz[3]&~0x8000u)bad++; else ok++;
        if((ok+bad)%2000u==0 && ok+bad<=40000u)fprintf(stderr,"[world-note] frame=%u accepted=%u rejected=%u\n",g_frame,ok,bad); }
    if(xyz[3]&~0x8000u)return;   /* not the X, Y, Z, flag block the function writes: record nothing */
    world_add((xyz[0]&65535u)|(xyz[1]<<16),xyz[2]>>4);
    note_ring[note_n&7].xy=(xyz[0]&65535u)|(xyz[1]<<16); note_ring[note_n&7].z=xyz[2]>>4; note_ring[note_n&7].f=g_frame; note_n++;
}
static int blood_vertex(uint32_t address) {
    if(g_mem_size<8 || address>g_mem_size-8)return 0;
    uint32_t packed[2];memcpy(packed,g_mem+address,8);
    return world_has(packed[0],packed[1]);
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
/* Picture viewers (puzzle boards, documents, close-ups): the modules F_0056a4d0..F_0058c110 draw their full-screen images with the
   game's 2D picture call F_004dc770 (cursor_ui_unit.py tags its entry). While one is on screen the whole screen keeps the game's
   16:9 layout for that frame and the next two, text and markers drawn around the picture included, like the map: squeezed, the
   picture sat in a 4:3 box with bars (Valentin 2026-11-17: every interactive screen must fill the screen; inventory excluded). */
/* Diagnostic only (instrumented unit copies, SH3_WIN_UNIT_PATCH): last frame each candidate effect producer ran. */
static struct { unsigned fn,frame,previous; } prod[16]; static unsigned nprod;
void diag_prod_mark(unsigned fn) {
    static int trace=-1;if(trace<0)trace=getenv("SH3_DIAG_SQUEEZE_TRACE")!=NULL;
    if(!trace)return;
    if (fn==0x5c7a00u) { static unsigned last=~0u, frames; if (last!=g_frame) { last=g_frame; if ((++frames%30u)==1u && frames<=30000u) fprintf(stderr,"[footprint-frames] %u (frame %u)\n",frames,g_frame); } }
    unsigned i=0; for(;i<nprod;i++) if(prod[i].fn==fn) break;
    if(i==nprod){if(nprod==16)return;prod[i].fn=fn;prod[i].frame=prod[i].previous=~0u;nprod++;}
    if(prod[i].frame!=g_frame){prod[i].previous=prod[i].frame;prod[i].frame=g_frame;} }
/* Diagnostic (SH3_DIAG_SQUEEZE_TRACE): footprint quads (F_005c7a00) emitted per presented frame. */
void diag_fp_count(uint32_t p) {
    static int trace=-1;if(trace<0)trace=getenv("SH3_DIAG_SQUEEZE_TRACE")!=NULL;
    if(!trace)return;
    static unsigned cur=~0u,n,flagged,lines;
    if(cur!=g_frame){ if(cur!=~0u && n && lines<4000){lines++;fprintf(stderr,"[fp-draw] frame=%u quads=%u flagged=%u\n",cur,n,flagged);} cur=g_frame;n=0;flagged=0; }
    n++;
    if(g_mem_size>=16 && p<=g_mem_size-16){ uint32_t f; memcpy(&f,g_mem+p+12,4); if(f)flagged++; }
}
/* Footprint depth bias added by F_005c7a00 after projection. The game uses 0xa0, which only just beats the floor in depth: on PC the
   footprints disappear below about 0x8c, and on the Vita the slightly different depth rounding made them blink with the camera
   position (issue #3, 2026-11-18). 0x400 keeps a wide margin and nothing else near the floor shows through. SH3_FOOT_BIAS overrides it. */
unsigned diag_fp_bias(void) {
    static int ready, v;
    if(!ready){ const char *e=getenv("SH3_FOOT_BIAS"); v=e?(int)strtol(e,NULL,0):0x400; if(e)fprintf(stderr,"[foot-bias] %d\n",v); ready=1; }
    return (unsigned)v;
}
static unsigned viewer_frame=~0u;
void diag_picture_note(uint32_t esp) {
    uint32_t caller;
    if (g_mem_size<8 || esp>g_mem_size-4) return;
    memcpy(&caller,g_mem+esp,4);
    if (caller<0x56a4d0u || caller>=0x58c300u) return;
    if (viewer_frame!=g_frame) { static unsigned n,last; if (n<60 && caller!=last) { n++; last=caller; uint32_t ui=0; if (g_mem_size>0xbcc664u) memcpy(&ui,g_mem+0xbcc660u,4);
        fprintf(stderr,"[wide-viewer] caller=%08x ui=%u frame=%u\n",caller,ui,g_frame); } }
    viewer_frame=g_frame;
}
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
    /* Full-screen interactive screens (map = ui screen 9, cursor puzzles): no 4:3 squeeze. The game's own layout then spans the
       whole 960 x 544 surface and matches the pointer (x*512/960), which the squeeze did not (Valentin, 2026-11-17). */
    { extern int diag_cursor_screen_active(void); uint32_t ui=0; if (g_mem_size>0xbcc664u) memcpy(&ui,g_mem+0xbcc660u,4);
      if (ui==9 || ui==11 || diag_cursor_screen_active()) return;
      if (viewer_frame!=~0u && g_frame-viewer_frame<=2u) return; }
    {   /* Diagnostic (SH3_DIAG_SQUEEZE_TRACE): in gameplay, who sends GS-decoder vertices that get the 4:3 squeeze without
           being registered as world-projected. Logs each new stack signature (code addresses near the top of the stack). */
        static int trace=-1; if (trace<0) trace=getenv("SH3_DIAG_SQUEEZE_TRACE")!=NULL;
        uint32_t ui=0; if (trace && g_mem_size>0xbcc664u) memcpy(&ui,g_mem+0xbcc660u,4);
        if (trace && ui==5) {
            static unsigned cf=~0u, cn, lines;   /* per frame: how many squeezed unregistered vertices, and which producers ran */
            if (cf!=g_frame) {
                if (cn && lines<3000) { lines++; fprintf(stderr,"[squeeze-count] frame=%u vertices=%u producers:",cf,cn);
                    for (unsigned j=0;j<nprod;j++) if (cf==prod[j].frame || cf==prod[j].previous) fprintf(stderr," %08x",prod[j].fn); fprintf(stderr,"\n"); }
                cf=g_frame; cn=0;
            }
            cn++;
            uint32_t sig[6]={0}; unsigned k=0;
            sig[k++]=ret;
            for (unsigned i=1;i<96 && k<6 && c->esp+4*i<=g_mem_size-4;i++) { uint32_t w; memcpy(&w,g_mem+c->esp+4*i,4); if (w>=0x401000u && w<0x680000u) sig[k++]=w; }
            static uint32_t seen[96][6]; static unsigned ns; unsigned i=0;
            for (;i<ns;i++) if (!memcmp(seen[i],sig,sizeof sig)) break;
            if (i==ns && ns<96) { memcpy(seen[ns++],sig,sizeof sig);
                fprintf(stderr,"[squeeze-trace] frame=%u x=%.1f world=%d chain=%08x %08x %08x %08x %08x %08x\n",g_frame,c->st[c->ftop&7u],
                        (blood_enabled()||blood_audit()) && blood_vertex(c->esi),sig[0],sig[1],sig[2],sig[3],sig[4],sig[5]);
                for (unsigned j=0;j<nprod;j++) if (g_frame-prod[j].frame<=1u) fprintf(stderr,"[squeeze-trace]   producer this frame: F_%08x\n",prod[j].fn);
                { uint32_t pk[2]={0,0}; if (c->esi<=g_mem_size-8) memcpy(pk,g_mem+c->esi,8); fprintf(stderr,"[squeeze-trace]   vertex xy=%08x z=%08x; last notes:",pk[0],pk[1]);
                  for (unsigned j=0;j<8;j++) fprintf(stderr," %08x/%08x@%u",note_ring[j].xy,note_ring[j].z,note_ring[j].f);
                  fprintf(stderr,"\n"); } }
        }
    }
    if (wide_flag(1) && wide_flag(2)) {
        double x=c->st[c->ftop&7u];
        if (x<=8.0 || x>=952.0) {   /* test build log: a squeezed element reaching a screen edge is a full-screen 2D layer (bars at the sides) */
            static uint32_t seen[256]; static unsigned n; uint32_t ui=0,key; if (g_mem_size>0xbcc664u) memcpy(&ui,g_mem+0xbcc660u,4);
            key=(ui<<24)^ret; unsigned i; for(i=0;i<n;i++) if(seen[i]==key) break;
            if(i==n && n<256){ seen[n++]=key; fprintf(stderr,"[wide-edge] ui=%u caller=%08x x=%.1f frame=%u\n",ui,ret,x,g_frame); }
        }
        c->st[c->ftop&7u]=480.0+(x-480.0)*((4.0/3.0)/(960.0/544.0));
    }
}

/* Fixed console mode must survive the PC Options aspect-ratio filter.
   Otherwise its current-mode lookup returns -1 and reads before the mode table. */
int diag_console_display_mode(uint32_t w,uint32_t h) {
    extern uint32_t g_diag_res_w,g_diag_res_h;
    return wide_flag(1) && w==g_diag_res_w && h==g_diag_res_h;
}
