// IDirect3DDevice8: state, resources and render calls (the GL rendering is in draw.c).
#include "d3d8.h"
#include "prep.h"
#include "audit.h"
#include "plat.h"
#include "diag_widescreen.h"
#include <stdlib.h>

Device *g_dev;
void d3d_fill_caps(uint32_t p);
extern const gfn tex_vt[], surf_vt[], vb_vt[], ib_vt[];
extern int diag_trace_pause(void);

#define M(name) static void dev_##name(CPU *c)
#define DEV Device *d = com_get_kind(ARG(1), K_DEVICE); if (!d) rt_fatal("device invalide %08x [%08x %08x %08x %08x] (g_dev=%p guest %08x) kind=%d obj=%p", ARG(1), R32(ARG(1)), R32(ARG(1) + 4), R32(ARG(1) + 8), R32(ARG(1) + 12), (void *)g_dev, g_dev ? g_dev->com.guest : 0, com_get(ARG(1)) ? com_get(ARG(1))->kind : -1, (void *)com_get(ARG(1)))

#define addref d3d_addref
uint64_t g_t_begin, g_t_body; // profiling: start of the current frame, accumulated body

/* Diagnostic SH3_DIAG_FADE_SEQ (off by default): full operation sequence (targets, copies, draws with source
   texture and target identities) of the first 8 frames that contain the FFP104 MODULATE2X fade pass. */
#include <stdarg.h>
static int fadeseq_on=-1,fadeseq_hit,fadeseq_frames; static unsigned fadeseq_start;   /* value = first frame traced */
static char fadeseq_buf[24576]; static size_t fadeseq_len;
static void fadeseq_init(void) { if(fadeseq_on<0) { const char *v=getenv("SH3_DIAG_FADE_SEQ"); fadeseq_on=v!=NULL; fadeseq_start=v?(unsigned)atoi(v):0; } }
/* Bench command drawlog: full operation/draw list (targets, copies, draws with shaders, textures, blend, cube maps) of the
   next presented frame; used to compare frames with the flashlight on/off (lamp halo investigation). 0 idle, 1 armed, 2 capturing. */
static int drawlog_state; static char *drawlog_buf; static size_t drawlog_len; enum { DRAWLOG_CAP = 1u << 20 };
void diag_drawlog_next(void) { drawlog_state = 1; }
static void fadeseq(const char *fmt,...) {
    fadeseq_init();
    int to_fade = fadeseq_on && fadeseq_frames<8 && fadeseq_len<sizeof fadeseq_buf-256, to_log = drawlog_state==2;
    if(!to_fade && !to_log) return;
    char line[640]; va_list ap; va_start(ap,fmt);
    int n=vsnprintf(line,sizeof line,fmt,ap); va_end(ap);
    if(n<=0) return; if((size_t)n>=sizeof line) n=(int)sizeof line-1;
    if(to_fade) { memcpy(fadeseq_buf+fadeseq_len,line,(size_t)n); fadeseq_len+=(size_t)n; fadeseq_buf[fadeseq_len]=0; }
    if(to_log) {
        if(!drawlog_buf) drawlog_buf=malloc(DRAWLOG_CAP);
        if(drawlog_buf && drawlog_len+(size_t)n+1<DRAWLOG_CAP) { memcpy(drawlog_buf+drawlog_len,line,(size_t)n); drawlog_len+=(size_t)n; drawlog_buf[drawlog_len]=0; }
    }
}
static uint32_t surf_id(const Surface *s) { return s ? s->com.guest : 0; }
static void fadeseq_draw(Device *d,const char *kind,uint32_t prim,uint32_t n) {
    fadeseq_init();
    if(!fadeseq_on && drawlog_state!=2) return;
    Texture *t=d->tex[0];
    if(d->vs==0x104u && d->tss[0][1]==5u) fadeseq_hit=1;
    if(drawlog_state==2) {
        fadeseq("D %s prim=%u n=%u vs=%x ps=%x rt=%08x %ux%u blend=%u/%u/%u z=%u/%u/%u aref=%u/%u/%u stencil=%u cull=%u fog=%u tf=%08x tex=[",
                kind,prim,n,d->vs,d->ps,surf_id(d->rt),d->rt?d->rt->w:0,d->rt?d->rt->h:0,d->rs[27],d->rs[19],d->rs[20],
                d->rs[7],d->rs[14],d->rs[23],d->rs[15],d->rs[24],d->rs[25],d->rs[52],d->rs[22],d->rs[28],d->rs[60]);
        for(unsigned s2=0;s2<4;s2++) {
            Texture *ts=d->tex[s2];
            fadeseq("%s%08x:%ux%u/%u%s",s2?" ":"",ts?ts->com.guest:0,ts?ts->w:0,ts?ts->h:0,ts?ts->fmt:0,d->cube[s2]?"+CUBE":"");
        }
        { uint32_t h=2166136261u, nz=0; Texture *t0=d->tex[0];
          if(t0 && t0->w==128 && t0->fmt==FMT_A8R8G8B8 && t0->lv && t0->lv[0] && t0->lv[0]->mem) {
              const uint8_t *q=g_mem+t0->lv[0]->mem; for(uint32_t i=0;i<128*128*4;i++){ h=(h^q[i])*16777619u; nz+=q[i]!=0; } }
          uint32_t ah[4]={0,0,0,0}; unsigned long long sr=0; uint32_t np=0;
          if(t0 && t0->w==512 && t0->fmt==FMT_A8R8G8B8 && t0->lv && t0->lv[0] && t0->lv[0]->mem) {
              const uint8_t *q=g_mem+t0->lv[0]->mem; np=512*512; for(uint32_t i=0;i<np;i++){ uint8_t a8=q[i*4+3]; ah[a8==0?0:a8<16?1:a8<64?2:3]++; sr+=q[i*4]+q[i*4+1]+q[i*4+2]; } }
          uint32_t sh_[4]={0,0,0,0};
          for(int k=1;k<=2;k++){ Texture *tk=d->tex[k]; uint32_t hh=2166136261u; if(tk && tk->fmt==FMT_A8R8G8B8 && tk->w<=128 && tk->lv && tk->lv[0] && tk->lv[0]->mem){ const uint8_t *q=g_mem+tk->lv[0]->mem; for(uint32_t i=0;i<tk->w*tk->h*4;i++) hh=(hh^q[i])*16777619u; sh_[k]=hh; } }
          fadeseq("] t0h=%08x nz=%u a512=%u/%u/%u/%u avgrgb=%u t1h=%08x t2h=%08x",h,nz,ah[0],ah[1],ah[2],ah[3],np?(unsigned)(sr/np/3):0,sh_[1],sh_[2]); }
        fadeseq(" sten=%u/%u/%u/%x/%u,%u,%u/%x",d->rs[52],d->rs[56],d->rs[57],d->rs[58],d->rs[53],d->rs[54],d->rs[55],d->rs[59]);
        fadeseq(" colorop=%u,%u,%u alphaop=%u,%u vp=%u,%u,%u,%u\n",d->tss[0][1],d->tss[1][1],d->tss[2][1],d->tss[0][4],d->tss[1][4],d->vp.x,d->vp.y,d->vp.w,d->vp.h);
        return;
    }
    fadeseq("  %s prim=%u n=%u vs=%x ps=%x rt=%08x(tex %08x %ux%u) tex0=%08x %ux%u usage=%u lv=%u colorop=%u tf=%08x blend=%u/%u/%u vp=%u,%u %ux%u\n",
            kind,prim,n,d->vs,d->ps,surf_id(d->rt),d->rt&&d->rt->tex?d->rt->tex->com.guest:0,d->rt?d->rt->w:0,d->rt?d->rt->h:0,
            t?t->com.guest:0,t?t->w:0,t?t->h:0,t?t->usage:0,t?t->levels:0,d->tss[0][1],d->rs[60],d->rs[27],d->rs[19],d->rs[20],
            d->vp.x,d->vp.y,d->vp.w,d->vp.h);
}
static void fadeseq_present(void) {
    if(drawlog_state==2) {
        rt_log("[drawlog] frame=%u bytes=%u\n",g_frame,(unsigned)drawlog_len);
        if(drawlog_buf) for(size_t off=0; off<drawlog_len; ) { size_t k=0; while(off+k<drawlog_len && k<900 && drawlog_buf[off+k]!='\n') k++; fprintf(stderr,"%.*s\n",(int)k,drawlog_buf+off); off+=k+1; }
        fprintf(stderr,"[drawlog] end\n"); drawlog_state=0; drawlog_len=0;
    } else if(drawlog_state==1) { drawlog_state=2; drawlog_len=0; }
    if(fadeseq_on>0 && fadeseq_hit && fadeseq_frames<8 && g_frame>=fadeseq_start) { rt_log("[fade-seq] frame=%u\n%s",g_frame,fadeseq_buf); fadeseq_frames++; }
    fadeseq_len=0; fadeseq_hit=0;
}

M(QueryInterface) { STDRET(3, 0x80004002u); }
M(AddRef) { HOSTFN; DEV; STDRET(1, ++d->com.refs); }
M(Release) { HOSTFN; DEV; STDRET(1, d->com.refs > 1 ? --d->com.refs : 1); }
M(TestCooperativeLevel) { STDRET(1, HR_OK); }
M(GetAvailableTextureMem) { STDRET(1, 128u << 20); }
M(ResourceManagerDiscardBytes) { STDRET(2, HR_OK); }
M(GetDirect3D) {
    HOSTFN;
    extern void *g_d3d_obj;
    if (!g_d3d_obj) { W32(ARG(2), 0); STDRET(2, HR_FAIL); }
    d3d_addref(g_d3d_obj);
    W32(ARG(2), ((ComObj *)g_d3d_obj)->guest);
    STDRET(2, HR_OK);
}
M(GetDeviceCaps) { HOSTFN; d3d_fill_caps(ARG(2)); STDRET(2, HR_OK); }
M(GetDisplayMode) {
    HOSTFN; DEV;
    uint32_t p = ARG(2);
    W32(p, d->bb_w); W32(p + 4, d->bb_h); W32(p + 8, 60); W32(p + 12, FMT_X8R8G8B8);
    STDRET(2, HR_OK);
}
M(GetCreationParameters) {
    HOSTFN;
    uint32_t p = ARG(2);
    W32(p, 0); W32(p + 4, 1); W32(p + 8, 0x00010010u); W32(p + 12, 0x40);
    STDRET(2, HR_OK);
}
M(SetCursorProperties) { STDRET(4, HR_OK); }
M(SetCursorPosition) { STDRET(4, HR_OK); }
// bShow != 0: on-screen pointer in use -> dinput.c puts every direction on the left stick (g_cursor_seen)
M(ShowCursor) {
    HOSTFN; extern unsigned g_cursor_seen, g_frame;
    static unsigned last = 2;
    unsigned s = ARG(2) & 1;
    if (s) g_cursor_seen = g_frame + 1;
    if (s != last) { last = s; rt_log("[cursor] show=%u frame %u\n", s, g_frame); }
    STDRET(2, 0);
}
M(CreateAdditionalSwapChain) { d3d_log_once("CreateAdditionalSwapChain"); STDRET(3, D3DERR_INVALIDCALL); }
// ponytail: the backbuffer size does not change; SH4_RES re-imposes the resolution that the game option (0x524320) just overwrote.
M(Reset) { HOSTFN; DEV;
    if (ENVFLAG("SH3_DIAG_WIDE_NATIVE")) {
        extern uint32_t g_diag_res_w, g_diag_res_h;
        uint32_t rw=g_diag_res_w, rh=g_diag_res_h;
        W32(0x72bfd0u,rw); W32(0x72bfd4u,rh);
        W32(0x72c664u,rw); W32(0x72c668u,rh);
        W32(0x72c780u,rw); W32(0x72c784u,rh); W32(0x72c788u,rw); W32(0x72c78cu,rh);
        W32(ARG(2),rw); W32(ARG(2)+4,rh);
    }
    rt_log("[d3d8] Reset %ux%u\n", R32(ARG(2)), R32(ARG(2) + 4)); prep_gfx_init(); STDRET(2, HR_OK); }
M(Present) {
    HOSTFN; DEV;
    { extern void texfree_present(void); texfree_present(); }
    if (g_t_begin) g_t_body += SDL_GetPerformanceCounter() - g_t_begin;
    g_t_begin = 0;
    fadeseq_present();
    {   /* Loading-freeze diagnostic (bounded): a gap > 250 ms between two Presents means the game thread showed
           nothing at all (Valentin's overlay stops updating during loads). */
        static uint64_t last_present; static unsigned gaps;
        uint64_t now = SDL_GetTicks64();
        if (last_present && now - last_present > 250 && gaps < 400) {
            gaps++; rt_log("[present-gap] frame=%u gap=%llu ms at=%llu ms\n", g_frame, (unsigned long long)(now - last_present), (unsigned long long)now);
        }
        last_present = now;
    }
    g_frame++;
    /* SH4-only resolution enforcement, movie test hooks and native UI removed. */
#if 0
    // Laboratory schedule, e.g. "4200:7,4700:8". Advance before calling the
    // original blocking player: it recursively Presents while a movie runs.
    static const char *pm;
    static int movie_test_active;
    if (!pm) pm = getenv("SH4_PLAY_MOVIE") ? getenv("SH4_PLAY_MOVIE") : "";
    unsigned pf = 0, pn = 0; int consumed = 0;
    if (!movie_test_active && *pm && sscanf(pm, "%u:%u%n", &pf, &pn, &consumed) == 2 &&
        pf && pn < 14 && (!pm[consumed] || pm[consumed] == ',') && g_frame >= pf && g_main_cpu) {
        pm += consumed;
        if (*pm == ',') pm++;
        movie_test_active = 1;
        rt_log("[test] playing cutscene %u\n", pn);
        rt_guest_call(g_main_cpu, 0x412ef0, 1, pn);
        rt_log("[test] cutscene finished\n");
        movie_test_active = 0;
    }
    extern void rt_controls_present(void), console_ui_finish_frame(void);
    rt_controls_present();
    console_ui_finish_frame();
#endif
    prep_present();
    prep_gdrain();
#ifdef SH3_MOVIE_LAB
    static int present_probe=-1; if(present_probe<0) present_probe=getenv("SH3_DIAG_MOVIE_PRESENT_PROBE")!=NULL;   /* per Present: read once */
    if(present_probe) {
        static int probing;
        extern void movie_lab_present_probe(CPU *);
        if(!probing) { probing=1; movie_lab_present_probe(c); probing=0; }
    }
#endif
    rt_audit_frame(g_frame);
    (void)d;
    STDRET(5, HR_OK);
}
M(GetBackBuffer) {
    HOSTFN; DEV;
    addref(d->backbuffer);
    W32(ARG(4), d->backbuffer->com.guest);
    STDRET(4, HR_OK);
}
M(GetRasterStatus) { HOSTFN; W32(ARG(2), 1); W32(ARG(2) + 4, 0); STDRET(2, HR_OK); }
M(SetGammaRamp) {
    HOSTFN; DEV;
    // The render consumer owns a separate Device, including the final-blit gamma ramp.
    if (prep_set(d->gamma, mb + ARG(3), sizeof d->gamma) && g_audit_checks)
        rt_log("[audit-gamma] red=%u/%u/%u green=%u/%u/%u blue=%u/%u/%u\n",
               d->gamma[0][0], d->gamma[0][128], d->gamma[0][255],
               d->gamma[1][0], d->gamma[1][128], d->gamma[1][255],
               d->gamma[2][0], d->gamma[2][128], d->gamma[2][255]);
    STDRET(3, 0);
}
M(GetGammaRamp) { HOSTFN; DEV; memcpy(mb + ARG(2), d->gamma, sizeof d->gamma); STDRET(2, 0); }

M(CreateTexture) {
    HOSTFN;
    static int s_first = 1;
    if (s_first) { s_first = 0; rt_log("[d3d8] first CreateTexture %ux%u fmt=%u\n", ARG(2), ARG(3), ARG(6)); }
    Texture *t = tex_create(ARG(2), ARG(3), ARG(4), ARG(5), ARG(6), ARG(7));
#ifdef SH3_BAD_TARGET_LAB
    if (!t && ENVFLAG("SH3_DIAG_BAD_TARGET_DETAIL") && g_mem_size>=36 && c->esp<=g_mem_size-36)
        fprintf(stderr,"[bad-target] CreateTexture failed %ux%u caller=%08x out=%08x\n",ARG(2),ARG(3),R32(c->esp),ARG(8));
#endif
    if (!t) {
        fprintf(stderr,"[d3d8] CreateTexture rejected %ux%u fmt=%u\n",ARG(2),ARG(3),ARG(6));
        STDRET(8, D3DERR_INVALIDCALL);
    }
#ifdef SH3_BAD_TARGET_LAB
    if (ENVFLAG("SH3_DIAG_BAD_TARGET_DETAIL") && g_mem_size>=0x44 && c->esp <= g_mem_size-36 && R32(c->esp)==0x00461dcfu) {
        uint32_t obj=t->com.guest, vt=0, slot=0;
        if (obj && obj<=g_mem_size-4) memcpy(&vt,g_mem+obj,4);
        if (vt && vt<=g_mem_size-0x44) memcpy(&slot,g_mem+vt+0x40,4);
        fprintf(stderr,"[bad-target] CreateTexture %ux%u object=%08x vtable=%08x slot16=%08x out=%08x\n",
                ARG(2),ARG(3),obj,vt,slot,ARG(8));
    }
#endif
    W32(ARG(8), t->com.guest);
    STDRET(8, HR_OK);
}
M(CreateVolumeTexture) { HOSTFN; d3d_log_once("CreateVolumeTexture"); W32(ARG(9), 0); STDRET(9, D3DERR_NOTAVAILABLE); }
M(CreateCubeTexture) {
    HOSTFN;
    rt_log("[diag] CreateCubeTexture edge=%u levels=%u usage=%x format=%u pool=%u output=%08x\n", ARG(2),ARG(3),ARG(4),ARG(5),ARG(6),ARG(7));
    if (ENVFLAG("SH3_DIAG_ASSUME_CUBEMAP")) {
        extern uint32_t diag_cube_create(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t);
        uint32_t cube=diag_cube_create(ARG(2),ARG(3),ARG(4),ARG(5),ARG(6));
        W32(ARG(7),cube); STDRET(7,cube?HR_OK:D3DERR_NOTAVAILABLE);
    }
    W32(ARG(7), 0); STDRET(7, D3DERR_NOTAVAILABLE);
}
M(CreateVertexBuffer) {
    HOSTFN;
    Buffer *b = buf_create(ARG(2), ARG(3), ARG(4), ARG(5), 0);
    W32(ARG(6), b->com.guest);
    STDRET(6, HR_OK);
}
M(CreateIndexBuffer) {
    HOSTFN;
    Buffer *b = buf_create(ARG(2), ARG(3), ARG(4), ARG(5), 1);
    W32(ARG(6), b->com.guest);
    STDRET(6, HR_OK);
}
M(CreateRenderTarget) {
    HOSTFN;
    Surface *s = surf_create(ARG(2), ARG(3), ARG(4), 0);
    s->usage = 1; // D3DUSAGE_RENDERTARGET : stockage GL
    W32(ARG(7), surf_wrap(s));
    STDRET(7, HR_OK);
}
M(CreateDepthStencilSurface) {
    HOSTFN;
    Surface *s = surf_create(ARG(2), ARG(3), ARG(4), 1);
    W32(ARG(6), surf_wrap(s));
    STDRET(6, HR_OK);
}
M(CreateImageSurface) {
    HOSTFN;
    Surface *s = surf_create(ARG(2), ARG(3), ARG(4), 0);
    W32(ARG(5), surf_wrap(s));
    STDRET(5, HR_OK);
}
void surf_copy_rects(Surface *src, uint32_t rects, uint32_t n, Surface *dst, uint32_t points);
M(CopyRects) {
    HOSTFN;
    Surface *src = com_get_kind(ARG(2), K_SURFACE), *dst = com_get_kind(ARG(5), K_SURFACE);
    if (!src || !dst) STDRET(6, D3DERR_INVALIDCALL);
    fadeseq("  CopyRects src=%08x(tex %08x %ux%u) dst=%08x(tex %08x %ux%u) n=%u\n",surf_id(src),src->tex?src->tex->com.guest:0,src->w,src->h,surf_id(dst),dst->tex?dst->tex->com.guest:0,dst->w,dst->h,ARG(4));
#if defined(SH3_MOVIE_LAB) && defined(__vita__)
    static unsigned movie_copy_audit;
    if (g_movie_w && ENVFLAG("SH3_DIAG_MOVIE_TEXTURE_AUDIT") && movie_copy_audit++ < 30)
        rt_log("[movie-copy-audit] %ux%u fmt=%u tex=%d -> %ux%u fmt=%u tex=%d film=%dx%d caller=%08x\n",
               src->w,src->h,src->fmt,src->tex!=NULL,dst->w,dst->h,dst->fmt,dst->tex!=NULL,g_movie_w,g_movie_h,R32(c->esp));
#endif
    if (diag_trace_pause()) rt_log("[pause-trace] copy frame=%u %ux%u gl=%d -> %ux%u gl=%d n=%u\n",g_frame,src->w,src->h,surf_is_gl(src),dst->w,dst->h,surf_is_gl(dst),ARG(4));
    if (diag_trace_pause()) { uint32_t rc=ARG(3),pt=ARG(6);rt_log("[pause-copy] caller=%08x rect=%d,%d,%d,%d point=%d,%d\n",R32(c->esp),rc?(int)R32(rc):0,rc?(int)R32(rc+4):0,rc?(int)R32(rc+8):(int)src->w,rc?(int)R32(rc+12):(int)src->h,pt?(int)R32(pt):0,pt?(int)R32(pt+4):0); }
    surf_copy_rects(src, ARG(3), ARG(4), dst, ARG(6));
    /* SH3 F_005f2fc0 captures the complete scene into a power-of-two texture.
       Its original 512-square render target filled that texture, so the game
       hard-codes UV=(1,1) at bcc728/72c. Native widescreen keeps 960x544 here,
       leaving the remainder of the 1024-square texture empty. Correct only
       this identified native snapshot, using the copied extent, not geometry
       or arbitrary texture sizes. Ordinary D3D CopyRects semantics unchanged. */
    if(__atomic_load_n(&g_diag_capture_uv_fix,__ATOMIC_RELAXED) &&
       R32(c->esp)==0x5f312fu && !ARG(3) && !ARG(4) && !ARG(6) &&
       dst->tex && dst->level==0 && dst->tex->com.guest==R32(0xbcc71cu) &&
       src->w && src->h && src->w<=dst->w && src->h<=dst->h) {
        float u=(float)src->w/dst->w,v=(float)src->h/dst->h;
        memcpy(g_mem+0xbcc728u,&u,4);memcpy(g_mem+0xbcc72cu,&v,4);
        rt_log("[pause-uv] snapshot %ux%u in %ux%u: UV=%.6f,%.6f\n",src->w,src->h,dst->w,dst->h,u,v);
    }

    STDRET(6, HR_OK);
}
M(UpdateTexture) { d3d_log_once("UpdateTexture"); STDRET(3, HR_OK); }
M(GetFrontBuffer) { d3d_log_once("GetFrontBuffer"); STDRET(2, HR_OK); }
M(SetRenderTarget) {
    HOSTFN; DEV;
    Surface *rt = ARG(2) ? com_get_kind(ARG(2), K_SURFACE) : NULL;
    Surface *ds = ARG(3) ? com_get_kind(ARG(3), K_SURFACE) : NULL;
    fadeseq("  SetRenderTarget rt=%08x(tex %08x %ux%u) ds=%08x\n",surf_id(rt),rt&&rt->tex?rt->tex->com.guest:0,rt?rt->w:0,rt?rt->h:0,surf_id(ds));
    if (diag_trace_pause()) rt_log("[pause-trace] target frame=%u rt=%ux%u ds=%ux%u\n",g_frame,rt?rt->w:0,rt?rt->h:0,ds?ds->w:0,ds?ds->h:0);
    if (rt) {
        Surface *o = d->rt; addref(rt); PSET(rt, rt); if (o) d3d_release(&o->com);
        // D3D8: SetRenderTarget resets the viewport to the whole new target (the 4096x4096 pause capture relies on it)
        __typeof__(d->vp) v = {0, 0, rt->w, rt->h, 0.0f, 1.0f};
        prep_set(&d->vp, &v, sizeof v);
    }
    if (ds || ARG(3) == 0) { Surface *o = d->ds; addref(ds); PSET(ds, ds); if (o) d3d_release(&o->com); }
    prep_bind();
    STDRET(3, HR_OK);
}
M(GetRenderTarget) { HOSTFN; DEV; addref(d->rt); W32(ARG(2), d->rt ? d->rt->com.guest : 0); STDRET(2, HR_OK); }
M(GetDepthStencilSurface) {
    HOSTFN; DEV;
    addref(d->ds);
    W32(ARG(2), d->ds ? d->ds->com.guest : 0);
    STDRET(2, d->ds ? HR_OK : 0x8876086cu);
}
M(BeginScene) {
    HOSTFN; DEV;
    uint32_t ui_screen=R32(0xbcc660u);
    { static uint32_t last_ui=~0u; if (ui_screen!=last_ui) { last_ui=ui_screen; rt_log("[ui-screen] frame=%u id=%u\n",g_frame,ui_screen); } }
    prep_set(&d->sh3_ui_screen,&ui_screen,sizeof ui_screen);
#ifdef __vita__
    static int hospital_sharp = -1;
    if (hospital_sharp < 0) {
        const char *mode = getenv("SH3_PROBE_FX");
        hospital_sharp = mode && !strcmp(mode, "hospital");
    }
    if (hospital_sharp) {
        uint32_t active = ui_screen == 5 && g_mem[0x70e66d9u] == 30 &&
                          R32(0x897e48u) == 194;
        prep_set(&d->sh3_hospital_sharp, &active, sizeof active);
    }
#endif
    if (!d->in_scene) g_t_begin = SDL_GetPerformanceCounter();
    d->in_scene = 1;
    STDRET(1, HR_OK);
}
M(EndScene) { HOSTFN; DEV; d->in_scene = 0; STDRET(1, HR_OK); }
M(Clear) {
    HOSTFN; DEV;
    {   /* Test-build log (bounded): clear colours used on the inventory screen (ui 6), whose 4:3 layers leave the sides
           showing the cleared surface (orange bands in the mall's outdoor street). */
        uint32_t ui = R32(0xbcc660u);
        if (ui == 6 && (ARG(4) & 1u)) { static uint32_t seen[32]; static unsigned n; unsigned i = 0;
            for (; i < n; i++) if (seen[i] == ARG(5)) break;
            if (i == n && n < 32) { seen[n++] = ARG(5); rt_log("[inv-clear] color=%08x flags=%x stage=%u room=%u frame=%u\n", ARG(5), ARG(4),
                (unsigned)g_mem[0x70e66d9u], R32(0x897e48u), g_frame); } }
        /* The inventory clears the screen with the room's fog colour and then draws its 4:3 layers: in rooms with a light
           fog the sides showed coloured bands (orange in the mall's outdoor street, tester report). The full-screen target
           is cleared black on that screen, like the rest of the game's 4:3 sides; the small scene view keeps its colour. */
        if (ui == 6 && (ARG(4) & 1u) && d->rt && d->rt->w >= 960 && d->rt->h >= 544) {
            prep_clear(ARG(4), ARG(5) & 0xff000000u, rt_f32(ARG(6)), ARG(7), (int)ARG(2), ARG(3));
            STDRET(7, HR_OK);
        }
    }
    prep_clear(ARG(4), ARG(5), rt_f32(ARG(6)), ARG(7), (int)ARG(2), ARG(3));
    STDRET(7, HR_OK);
}
static float *xform(Device *d, uint32_t st) {
    if (st == 2) return d->view;
    if (st == 3) return d->proj;
    if (st >= 16 && st < 24) return d->texm[st - 16];
    if (st == 256) return d->world;
    return NULL;
}
M(SetTransform) { HOSTFN; DEV; float *m = xform(d, ARG(2)); if (m) prep_set(m, mb + ARG(3), 64); STDRET(3, HR_OK); }
M(GetTransform) { HOSTFN; DEV; float *m = xform(d, ARG(2)); if (m) memcpy(mb + ARG(3), m, 64); STDRET(3, HR_OK); }
#ifdef SH3_SHADOW_NATIVE_LAB
uint32_t g_diag_gettransform_target;
/* Same copy as GetTransform above, for the native shadow builder once it has checked that the guest vtable slot is
 * this method. Returns 0 (nothing written) when the object is not a device or the state has no matrix. */
int diag_get_transform_host(uint32_t device, uint32_t state, uint32_t out) {
    uint8_t *const mb = g_mem; Device *d = com_get_kind(device, K_DEVICE); float *m = d ? xform(d, state) : NULL;
    if (!m) return 0;
    memcpy(mb + out, m, 64); return 1;
}
#endif
M(MultiplyTransform) {
    HOSTFN; DEV;
    float *m = xform(d, ARG(2)), b[16], r[16];
    if (m) {
        memcpy(b, mb + ARG(3), 64);
        for (int i = 0; i < 4; i++)
            for (int j = 0; j < 4; j++)
                r[i * 4 + j] = b[i * 4 + 0] * m[0 + j] + b[i * 4 + 1] * m[4 + j] + b[i * 4 + 2] * m[8 + j] + b[i * 4 + 3] * m[12 + j];
        prep_set(m, r, 64);
    }
    STDRET(3, HR_OK);
}
M(SetViewport) {
    HOSTFN; DEV;
    uint32_t p = ARG(2);
    __typeof__(d->vp) v = {R32(p), R32(p + 4), R32(p + 8), R32(p + 12), rt_f32(R32(p + 16)), rt_f32(R32(p + 20))};
    if (diag_trace_pause()) rt_log("[pause-trace] viewport frame=%u target=%ux%u vp=%u,%u %ux%u\n",g_frame,d->rt?d->rt->w:0,d->rt?d->rt->h:0,v.x,v.y,v.w,v.h);
    prep_set(&d->vp, &v, sizeof v);
    STDRET(2, HR_OK);
}
M(GetViewport) {
    HOSTFN; DEV;
    uint32_t p = ARG(2);
    W32(p, d->vp.x); W32(p + 4, d->vp.y); W32(p + 8, d->vp.w); W32(p + 12, d->vp.h);
    W32(p + 16, rt_u32f(d->vp.minz)); W32(p + 20, rt_u32f(d->vp.maxz));
    STDRET(2, HR_OK);
}
static uint8_t g_material[68], g_lights[8][104];
M(SetMaterial) { HOSTFN; memcpy(g_material, mb + ARG(2), 68); STDRET(2, HR_OK); }
M(GetMaterial) { HOSTFN; memcpy(mb + ARG(2), g_material, 68); STDRET(2, HR_OK); }
M(SetLight) { HOSTFN; if (ARG(2) < 8) memcpy(g_lights[ARG(2)], mb + ARG(3), 104); STDRET(3, HR_OK); }
M(GetLight) { HOSTFN; if (ARG(2) < 8) memcpy(mb + ARG(3), g_lights[ARG(2)], 104); STDRET(3, HR_OK); }
M(LightEnable) { STDRET(3, HR_OK); }
M(GetLightEnable) { HOSTFN; W32(ARG(3), 0); STDRET(3, HR_OK); }
M(SetClipPlane) { STDRET(3, HR_OK); }
M(GetClipPlane) { STDRET(3, HR_OK); }
M(SetRenderState) { HOSTFN; DEV; if (ARG(2) < 256) { uint32_t v = ARG(3); prep_set(&d->rs[ARG(2)], &v, 4); } STDRET(3, HR_OK); }
#ifdef SH3_RENDERSTATE_LAB
uint32_t g_diag_renderstate_target;
/* Same state write and prep recording as the COM method above. The caller
 * has verified the actual registered target; invalid objects retain COM's
 * diagnostic failure path. No state is skipped and no CPU field is touched. */
int diag_d3d_renderstate(uint32_t object,uint32_t state,uint32_t value) {
    Device *d=com_get_kind(object,K_DEVICE);
    if (!d) return 0;
    if (state<256) prep_set(&d->rs[state],&value,4);
    return 1;
}
#endif
M(GetRenderState) { HOSTFN; DEV; W32(ARG(3), ARG(2) < 256 ? d->rs[ARG(2)] : 0); STDRET(3, HR_OK); }

/* Legacy state snapshots plus accurately recorded texture bindings. Textures
   are pixel state, including explicit NULL. A block owns its references.
   Texture-stage state also has an explicit recording mask. Other legacy
   snapshot categories remain unchanged in this focused fix. */
typedef struct {
    uint32_t rs[256],tss[MAX_STAGES][32],vs,ps;
    float vsc[MAX_VSC][4];
    Texture *tex[MAX_STAGES];ComObj *cube[MAX_STAGES];uint32_t texmask;
    uint32_t tssmask[MAX_STAGES];
} StateBlock;
static StateBlock *blocks[256];
static int recording;
static Texture *record_tex[MAX_STAGES];
static ComObj *record_cube[MAX_STAGES];
static uint32_t record_texmask;
static uint32_t record_tss[MAX_STAGES][32],record_tssmask[MAX_STAGES];
static void sb_store_texture(Texture **slot,ComObj **cube_slot,Texture *t,ComObj *cube) {
    if(t)addref(t);if(cube)cube->refs++;
    if(*slot)d3d_release(&(*slot)->com);
    if(*cube_slot)diag_cube_release_obj(*cube_slot);
    *slot=t;*cube_slot=cube;
}
static void device_bind_texture(Device *d,unsigned stage,Texture *t,ComObj *cube) {
    Texture *old=d->tex[stage];ComObj *oldcube=d->cube[stage];
    if(old==t && oldcube==cube)return;
    if(t)addref(t);if(cube)cube->refs++;
    PSET(tex[stage],t);PSET(cube[stage],cube);
    if(old)d3d_release(&old->com);if(oldcube)diag_cube_release_obj(oldcube);
}
static void sb_capture_textures(StateBlock *b,Device *d) {
    for(unsigned i=0;i<MAX_STAGES;i++)if(b->texmask&(1u<<i))
        sb_store_texture(&b->tex[i],&b->cube[i],d->tex[i],d->cube[i]);
}
static void sb_free(StateBlock *b) {
    if(!b)return;
    for(unsigned i=0;i<MAX_STAGES;i++)sb_store_texture(&b->tex[i],&b->cube[i],NULL,NULL);
    free(b);
}
static uint32_t sb_new(Device *d) {
    for(unsigned i=1;i<256;i++)if(!blocks[i]) {
        StateBlock *b=calloc(1,sizeof *b);if(!b)return 0;
        memcpy(b->rs,d->rs,sizeof d->rs);memcpy(b->tss,d->tss,sizeof d->tss);
        memcpy(b->vsc,d->vsc,sizeof d->vsc);b->vs=d->vs;b->ps=d->ps;
        blocks[i]=b;return i;
    }
    return 0;
}
M(BeginStateBlock) {
    if(recording)STDRET(1,D3DERR_INVALIDCALL);
    recording=1;record_texmask=0;memset(record_tssmask,0,sizeof record_tssmask);d3d_log_once("BeginStateBlock");STDRET(1,HR_OK);
}
M(EndStateBlock) {
    HOSTFN;DEV;
    if(!recording)STDRET(2,D3DERR_INVALIDCALL);
    uint32_t id=sb_new(d);recording=0;
    if(id){
        StateBlock *b=blocks[id];b->texmask=record_texmask;
        memcpy(b->tss,record_tss,sizeof b->tss);memcpy(b->tssmask,record_tssmask,sizeof b->tssmask);
        memcpy(b->tex,record_tex,sizeof record_tex);memcpy(b->cube,record_cube,sizeof record_cube);
        memset(record_tex,0,sizeof record_tex);memset(record_cube,0,sizeof record_cube);
        rt_log("[stateblock] recorded id=%u texture_mask=%x\n",id,b->texmask);
    }else for(unsigned i=0;i<MAX_STAGES;i++)sb_store_texture(&record_tex[i],&record_cube[i],NULL,NULL);
    record_texmask=0;W32(ARG(2),id);STDRET(2,id?HR_OK:0x8007000eu);
}
M(ApplyStateBlock) {
    HOSTFN;DEV;StateBlock *b=ARG(2)<256?blocks[ARG(2)]:NULL;
    if(!b)STDRET(2,D3DERR_INVALIDCALL);
    prep_set(d->rs,b->rs,sizeof d->rs);
    for(unsigned i=0;i<MAX_STAGES;i++)for(unsigned k=0;k<32;k++)if(b->tssmask[i]&(1u<<k))prep_set(&d->tss[i][k],&b->tss[i][k],4);
    prep_set(d->vsc,b->vsc,sizeof d->vsc);PSET(vs,b->vs);PSET(ps,b->ps);
    for(unsigned i=0;i<MAX_STAGES;i++)if(b->texmask&(1u<<i))device_bind_texture(d,i,b->tex[i],b->cube[i]);
    STDRET(2,HR_OK);
}
M(CaptureStateBlock) {
    HOSTFN;DEV;StateBlock *b=ARG(2)<256?blocks[ARG(2)]:NULL;
    if(!b)STDRET(2,D3DERR_INVALIDCALL);
    memcpy(b->rs,d->rs,sizeof d->rs);
    for(unsigned i=0;i<MAX_STAGES;i++)for(unsigned k=0;k<32;k++)if(b->tssmask[i]&(1u<<k))b->tss[i][k]=d->tss[i][k];
    memcpy(b->vsc,d->vsc,sizeof d->vsc);b->vs=d->vs;b->ps=d->ps;
    sb_capture_textures(b,d);STDRET(2,HR_OK);
}
M(DeleteStateBlock) {
    HOSTFN;uint32_t id=ARG(2);if(id>=256 || !blocks[id])STDRET(2,D3DERR_INVALIDCALL);
    sb_free(blocks[id]);blocks[id]=NULL;STDRET(2,HR_OK);
}
M(CreateStateBlock) {
    HOSTFN;DEV;uint32_t type=ARG(2);if(type<1 || type>3)STDRET(3,D3DERR_INVALIDCALL);
    uint32_t id=sb_new(d);if(id){blocks[id]->texmask=type==3?0:((1u<<MAX_STAGES)-1);for(unsigned i=0;i<MAX_STAGES;i++)blocks[id]->tssmask[i]=0xffffffffu;sb_capture_textures(blocks[id],d);}
    W32(ARG(3),id);STDRET(3,id?HR_OK:0x8007000eu);
}
M(SetClipStatus) { STDRET(2, HR_OK); }
M(GetClipStatus) { HOSTFN; W32(ARG(2), 0); W32(ARG(2) + 4, 0); STDRET(2, HR_OK); }
M(GetTexture) {
    HOSTFN; DEV;
    Texture *t = ARG(2) < MAX_STAGES ? d->tex[ARG(2)] : NULL;
    ComObj *cube = ARG(2) < MAX_STAGES ? d->cube[ARG(2)] : NULL;
    addref(t);
    if (cube) cube->refs++;
    W32(ARG(3), t ? t->com.guest : cube ? cube->guest : 0);
    STDRET(3, HR_OK);
}
M(SetTexture) {
    HOSTFN; DEV;
    uint32_t s = ARG(2);
    if (s >= MAX_STAGES) STDRET(3, HR_OK);
    ComObj *cube=ARG(3) ? com_get_kind(ARG(3),K_CUBE) : NULL;
    Texture *t = ARG(3) ? com_get_kind(ARG(3), K_TEXTURE) : NULL;
    if (ARG(3) && !t && !cube) rt_fatal("[diag] unsupported texture type");
    if(recording) {
        sb_store_texture(&record_tex[s],&record_cube[s],t,cube);record_texmask|=1u<<s;
        STDRET(3,HR_OK);
    }
    if(cube){static int cube_seen;if(!cube_seen || getenv("SH3_DIAG_COMLOG"))rt_log("[diag] experimental cube bound stage=%u vs=%08x ps=%08x\n",s,d->vs,d->ps);cube_seen=1;}
    device_bind_texture(d,s,t,cube);
    STDRET(3, HR_OK);
}
M(GetTextureStageState) {
    HOSTFN; DEV;
    W32(ARG(4), ARG(2) < MAX_STAGES && ARG(3) < 32 ? d->tss[ARG(2)][ARG(3)] : 0);
    STDRET(4, HR_OK);
}
M(SetTextureStageState) {
    HOSTFN; DEV;
    if (ARG(2) < MAX_STAGES && ARG(3) < 32) {
        unsigned stage=ARG(2),state=ARG(3);uint32_t v=ARG(4);
        if(recording){record_tss[stage][state]=v;record_tssmask[stage]|=1u<<state;}
        else prep_set(&d->tss[stage][state],&v,4);
    }
    STDRET(4, HR_OK);
}
M(ValidateDevice) { HOSTFN; W32(ARG(2), 1); STDRET(2, HR_OK); }
M(GetInfo) { STDRET(4, 1); } // S_FALSE
M(SetPaletteEntries) { d3d_log_once("SetPaletteEntries"); STDRET(3, HR_OK); }
M(GetPaletteEntries) { STDRET(3, HR_OK); }
M(SetCurrentTexturePalette) { STDRET(2, HR_OK); }
M(GetCurrentTexturePalette) { HOSTFN; W32(ARG(2), 0); STDRET(2, HR_OK); }

static uint32_t prim_verts(uint32_t type, uint32_t n) {
    switch (type) {
    case 1: return n;          // POINTLIST
    case 2: return n * 2;      // LINELIST
    case 3: return n + 1;      // LINESTRIP
    case 4: return n * 3;      // TRIANGLELIST
    case 5: case 6: return n + 2; // TRIANGLESTRIP / FAN
    default: return 0;
    }
}
static int diag_skip_world(Device *d, uint32_t prim, uint32_t nprims) {   /* diagnostic: SH3_DIAG_SKIPCLASS bit 16 = skip draws with a cube texture bound, 32 = skip ps!=0 world draws */
    static int m=-1; static unsigned skipvs=0; static int skiptw=0; static long skiptf=-1; if(m<0){ const char *e=getenv("SH3_DIAG_SKIPCLASS"); m=e?atoi(e):0; e=getenv("SH3_DIAG_SKIPVS"); if(e) skipvs=(unsigned)strtoul(e,NULL,16); e=getenv("SH3_DIAG_SKIPTEXW"); if(e) skiptw=atoi(e); e=getenv("SH3_DIAG_SKIPTF"); if(e) skiptf=(long)strtoul(e,NULL,16); }
    if(skipvs && d->rt && d->rt->w==960 && d->vs==skipvs && (!skiptw || (d->tex[0] && (int)d->tex[0]->w==skiptw)) && (skiptf<0 || (long)d->rs[60]==skiptf)) return 1;   /* diagnostic: skip draws of one vertex shader id (hex), optionally for one texture-0 width */
    if(!(m&112) || !d->rt || d->rt->w!=960) return 0;
    if((m&64) && prim==5 && nprims==2 && (d->cube[1]||d->cube[2]||d->cube[3])) return 1;   /* the lamp-only cube quads */
    if((m&16) && (d->cube[1]||d->cube[2]||d->cube[3])) return 1;
    if((m&32) && d->ps && d->vs!=0x144u) return 1;
    return 0;
}
M(DrawPrimitive) {
    HOSTFN; DEV;
    if(diag_skip_world(d,ARG(2),ARG(4))) STDRET(4,HR_OK);
    fadeseq_draw(d,"Draw",ARG(2),ARG(4));
    prep_draw(d, ARG(2), ARG(3), prim_verts(ARG(2), ARG(4)), ARG(4), NULL, 0, 0, NULL, 0);
    STDRET(4, HR_OK);
}
M(DrawIndexedPrimitive) {
    HOSTFN; DEV;
    uint32_t type = ARG(2), minidx = ARG(3), start = ARG(5), n = ARG(6);
    if (!d->ib) STDRET(6, D3DERR_INVALIDCALL);
    int i32 = d->ib->fmt_or_fvf == FMT_INDEX32;
    const uint8_t *idx = mb + d->ib->mem + start * (i32 ? 4 : 2);
    if(diag_skip_world(d,type,n)) STDRET(6,HR_OK);
    fadeseq_draw(d,"DrawIdx",type,n);
    prep_draw(d, type, d->base_vertex, prim_verts(type, n), n, idx, i32, minidx, NULL, 0);
    STDRET(6, HR_OK);
}
/* SH3 "afterimage" (rémanence): the game re-draws the previous frame as a full-screen alpha quad (FVF 0x104, texture = a
   copy of the whole scene render target, SRCALPHA/INVSRCALPHA, z-test on, MODULATE2X by TFACTOR) before the scene's new
   bright parts, so bright lights smear and leave ghost squares/streaks while the camera moves (Valentin, 2026-11-16:
   "les lumières bavent et se dispersent", mall ceiling lights; same on PC). Dropped by default; SH3_AFTERIMAGE=1 restores it.
   Verified on PC (corridor mall_f_01): ghosts gone, nothing else changes. Also saves one full-screen blend per frame. */
static int sh3_afterimage_draw(Device *d) {
    static int keep=-1; if(keep<0){ const char *e=getenv("SH3_AFTERIMAGE"); keep=e&&*e=='1'; }
    if(keep || !d->rt || !d->tex[0] || d->vs!=0x104u) return 0;
    if(d->tex[0]->w!=d->rt->w || d->tex[0]->h!=d->rt->h || d->rt->w<640) return 0;
    return d->rs[27]==1 && d->rs[19]==5 && d->rs[20]==6 && d->rs[7]==1 && d->rs[14]==0 && d->rs[23]==8 && d->tss[0][1]==5u && !d->ps;   /* COLOROP MODULATE2X x TFACTOR: its colour follows the Brightness level (0x808080 at level 3, 0x4a4a4a at 7), so never test it */
}
M(DrawPrimitiveUP) {
    HOSTFN; DEV;
    if(diag_skip_world(d,ARG(2),ARG(3)) || sh3_afterimage_draw(d)) STDRET(5,HR_OK);
    fadeseq_draw(d,"DrawUP",ARG(2),ARG(3));
    if(drawlog_state==2 && d->vs==0x144u && ARG(5)==28 && ARG(3)>=2 && (uint64_t)ARG(4)+112<=g_mem_size) {   /* lamp study: first/last vertex of sprite quads */
        float f0[7],f3[7]; memcpy(f0,mb+ARG(4),28); memcpy(f3,mb+ARG(4)+84,28); uint32_t c0,c3; memcpy(&c0,&f0[4],4); memcpy(&c3,&f3[4],4);
        fadeseq("V xy=%.1f,%.1f..%.1f,%.1f z=%.3f diffuse=%08x/%08x tex0=%08x\n",f0[0],f0[1],f3[0],f3[1],f0[2],c0,c3,d->tex[0]?d->tex[0]->com.guest:0);
    }
    if(fadeseq_on>0 && d->vs==0x104u && ARG(5)==24 && (uint64_t)ARG(4)+96<=g_mem_size) {
        float v[24]; memcpy(v,mb+ARG(4),sizeof v);
        fadeseq("    xyuv %.3f,%.3f %.5f,%.5f | %.3f,%.3f %.5f,%.5f | %.3f,%.3f %.5f,%.5f | %.3f,%.3f %.5f,%.5f\n",
                v[0],v[1],v[4],v[5],v[6],v[7],v[10],v[11],v[12],v[13],v[16],v[17],v[18],v[19],v[22],v[23]);
    }
    /* Scene transition (fade-seq trace, title->load): each frame the game copies render-target texture A into
       render target B of the same size with SELECTARG1(texture), FVF 0x104, quad x 0..W / y 0..H and UV 0..1,
       i.e. D3D samples exactly on texel corners and its bilinear filter averages each 2x2 block; repeated
       every frame this is the original drifting blur. Vita's sampling of these corner positions duplicated
       texels instead (growing blocks). For this exact draw only: exact 2x2 mean in the shader (draw.c
       sh3_fadebox) with a POINT sampler; everything else unchanged. The save-screen transition does the same
       512x512 ping-pong copy with MODULATE2X x TFACTOR (darkening built in): accepted too (2026-11-16). */
    uint32_t fadebox=0,fade_mag=0,fade_min=0;
    {
        Texture *ft=d->tex[0];
        if(d->vs==0x104u && !d->ps && (d->tss[0][1]==2u || d->tss[0][1]==5u) && d->tss[0][2]==2u && ft && (ft->usage&1u) && ft->levels==1 &&
           d->rt && d->rt->tex && d->rt->w==ft->w && d->rt->h==ft->h && ft->w<65536u && ft->h<16384u &&
           ARG(2)==5 && ARG(3)==2 && ARG(5)==24 && (uint64_t)ARG(4)+96<=g_mem_size) {
            float v[24]; memcpy(v,mb+ARG(4),sizeof v);
            /* Corners on 0/W, 0/H with UV 0/1, straight or mirrored per axis (the title's 256x256 effect copy is
               mirrored on both axes): every output pixel still samples a texel corner, i.e. a 2x2 mean. */
            float W=(float)ft->w,H=(float)ft->h; int exact=1,su=-1,sv=-1;
            for(unsigned i=0;i<4;i++) {
                float x=v[6*i],y=v[6*i+1],u=v[6*i+4],t=v[6*i+5];
                if((x!=0.0f&&x!=W)||(y!=0.0f&&y!=H)||(u!=0.0f&&u!=1.0f)||(t!=0.0f&&t!=1.0f)) { exact=0; break; }
                int du=(u==1.0f)==(x==W), dv=(t==1.0f)==(y==H);   /* 1 straight, 0 mirrored */
                if(su<0) su=du; if(sv<0) sv=dv;
                if(du!=su||dv!=sv) exact=0;
            }
            static unsigned misses;
            if(!exact && misses<12) {   /* same RT->RT copy, other geometry: report it (bounded) */
                misses++;
                rt_log("[fadebox-miss] %ux%u xyuv %.3f,%.3f %.5f,%.5f | %.3f,%.3f %.5f,%.5f | %.3f,%.3f %.5f,%.5f | %.3f,%.3f %.5f,%.5f\n",
                       ft->w,ft->h,v[0],v[1],v[4],v[5],v[6],v[7],v[10],v[11],v[12],v[13],v[16],v[17],v[18],v[19],v[22],v[23]);
            }
            if(exact) {
                static unsigned seen[4];   /* one line per (size, orientation), bounded */
                unsigned key=ft->w<<2|(unsigned)su<<1|(unsigned)sv;
                for(unsigned k=0;k<4;k++) { if(seen[k]==key) break; if(!seen[k]) { seen[k]=key;
                    rt_log("[fadebox] RT copy %ux%u u:%s v:%s: exact copy (no slide)\n",ft->w,ft->h,su?"straight":"mirrored",sv?"straight":"mirrored"); break; } }
                fadebox=ft->w<<16|ft->h<<2|(uint32_t)su<<1|(uint32_t)sv; fade_mag=d->tss[0][16]; fade_min=d->tss[0][17];
                uint32_t point=1; PSET(sh3_fadebox,fadebox);
                prep_set(&d->tss[0][16],&point,4); prep_set(&d->tss[0][17],&point,4);
            }
        }
    }
#define FADEBOX_RESTORE() do{if(fadebox){PSET(sh3_fadebox,0u);prep_set(&d->tss[0][16],&fade_mag,4);prep_set(&d->tss[0][17],&fade_min,4);}}while(0)
#if defined(SH3_MOVIE_HW_LAB) && defined(__vita__)
    /* Film quad (caller 004046c4, texture 0072c080): sample the AvPlayer YUV texture instead of the game's
       copy, for this draw only; UVs, states and draw order stay the game's. */
    extern Texture *movie_hw_film_tex(void);
    Texture *film_tex=d->tex[0],*film_hw=NULL;
    if(R32(c->esp)==0x4046c4u && film_tex && film_tex->com.guest==R32(0x72c080u) && (film_hw=movie_hw_film_tex())) {
        static int uv_logged;
        if(!uv_logged++ && ARG(5)==28 && (uint64_t)ARG(4)+112<=g_mem_size) {
            float uv[8];for(unsigned i=0;i<4;i++){memcpy(uv+2*i,mb+ARG(4)+28*i+20,4);memcpy(uv+2*i+1,mb+ARG(4)+28*i+24,4);}
            rt_log("[movie-hw] film quad UV %.4f,%.4f %.4f,%.4f %.4f,%.4f %.4f,%.4f tex %ux%u\n",
                   uv[0],uv[1],uv[2],uv[3],uv[4],uv[5],uv[6],uv[7],film_tex->w,film_tex->h);
        }
        PSET(tex[0],film_hw);
    }
#define FILM_RESTORE() do{if(film_hw)PSET(tex[0],film_tex);}while(0)
#define FILM_IS_GAME_TEX (film_hw || (d->tex[0] && d->tex[0]->com.guest==R32(0x72c080u)))
#else
#define FILM_RESTORE() do{}while(0)
#define FILM_IS_GAME_TEX (d->tex[0] && d->tex[0]->com.guest==R32(0x72c080u))
#endif
#ifdef SH3_MOVIE_LAB
    /* The native SH3 film quad on the main thread's private guest stack.
       prep_draw copies UP data before returning (or waits for its consumer).
       Restore the guest bytes immediately; preserve every UV/color. */
    static int movie_wide=-1; if(movie_wide<0) movie_wide=getenv("SH3_DIAG_MOVIE_WIDE")!=NULL;   /* per UP draw: read once */
    if(movie_wide && R32(c->esp)==0x4046c4u &&
       ARG(2)==5 && ARG(3)==2 && ARG(5)==28 && d->vs==0x144u && !d->ps &&
       FILM_IS_GAME_TEX &&
       !d->vp.x && !d->vp.y && d->vp.w && d->vp.h &&
       (uint64_t)ARG(4)+112<=g_mem_size) {
        unsigned char vertices[112],original[112];
        float x[4],y[4],left,right,top,bottom;
        memcpy(vertices,mb+ARG(4),sizeof vertices);
        memcpy(original,vertices,sizeof original);
        for(unsigned i=0;i<4;i++) {memcpy(x+i,vertices+28*i,4);memcpy(y+i,vertices+28*i+4,4);}
        left=right=x[0];top=bottom=y[0];
        for(unsigned i=1;i<4;i++) {
            if(x[i]<left)left=x[i];if(x[i]>right)right=x[i];
            if(y[i]<top)top=y[i];if(y[i]>bottom)bottom=y[i];
        }
        unsigned corners=0;
        if(right>left && bottom>top)for(unsigned i=0;i<4;i++) {
            if((x[i]!=left && x[i]!=right)||(y[i]!=top && y[i]!=bottom))break;
            corners|=1u<<((x[i]==right)+2*(y[i]==bottom));
        }
        if(corners==15) {
            for(unsigned i=0;i<4;i++) {
                float px=x[i]==left?-.5f:(float)d->vp.w-.5f;
                float py=y[i]==top?-.5f:(float)d->vp.h-.5f;
                memcpy(vertices+28*i,&px,4);memcpy(vertices+28*i+4,&py,4);
            }
            static unsigned reported;
            if(!reported++)rt_log("[movie-wide] native film quad stretched to %ux%u; UV/color retained\n",d->vp.w,d->vp.h);
            memcpy(mb+ARG(4),vertices,sizeof vertices);
            prep_draw(d,5,0,4,2,NULL,0,0,mb+ARG(4),28);
            memcpy(mb+ARG(4),original,sizeof original);
            FILM_RESTORE();
            STDRET(5,HR_OK);
        }
    }
#endif
    /* Pause and its return-to-title confirmation each draw one opaque panel.
       Keep the captured scene visible behind their unchanged native text. */
    uint32_t panel_caller=R32(c->esp);
    if((panel_caller==0x5e8407u || panel_caller==0x5e85eau) &&
       ARG(2)==5 && ARG(3)==2 && ARG(5)==20 &&
       d->vs==0x44u && (uint64_t)ARG(4)+80<=g_mem_size) {
        unsigned black=1;
        for(unsigned i=0;i<4;i++)if(R32(ARG(4)+20*i+16)!=0xff000000u)black=0;
        if(black) {
            static unsigned logged;
            unsigned bit=panel_caller==0x5e8407u?1u:2u;
            if(!(logged&bit)){logged|=bit;rt_log("[console-pause] opaque panel removed caller=%08x; text and scene preserved\n",panel_caller);}
            STDRET(5,HR_OK);
        }
    }
#ifdef __vita__
    /* Lens-flare / lamp-halo visibility probe (see draw.c probe_emulate): the one-vertex pre-transformed point drawn into
       a 1x1 render target, sampling the scene texture at the light position. The consumer skips this draw and emulates
       its result, so only its sample position (from the vertex uv) is passed along, for this draw only. */
    { static int skipm=-1; if(skipm<0){ const char *e=getenv("SH3_DIAG_SKIPCLASS"); skipm=e?atoi(e):0; }
      if(skipm && d->rt && d->rt->w==960 && d->tex[0] && (((d->rs[27] && d->rs[19]==5 && d->rs[20]==2) || (skipm&(16|128))))) {
          int cls=0;
          if(d->vs==0x104u && d->tex[0]->w==256 && d->rt->w==960) cls=128;   /* bloom composite 256 -> scene */
          else if(d->vs==0x144u && d->tex[0]->w==128) cls=1;
          else if(d->vs==0x144u && d->tex[0]->w==512) cls=2;
          else if(d->vs==0x104u && d->tex[0]->w==128) cls=4;
          if(d->cube[3]||d->cube[2]||d->cube[1]) cls|=16;
          if(d->vs!=0x144u) cls|=8;
          if(cls & skipm) STDRET(5,HR_OK);
      } }
    uint32_t probe_pos=0;
    if (ARG(2)==1 && ARG(3)==1 && !(d->vs&1u) && (d->vs&0x4u) && ARG(5)>=24 && d->rt && d->rt->w==1 && d->rt->h==1 &&
        d->tex[0] && (uint64_t)ARG(4)+ARG(5)<=g_mem_size) {
        float fu,fv; memcpy(&fu,mb+ARG(4)+16,4); memcpy(&fv,mb+ARG(4)+20,4);
        int px=(int)(fu*(float)d->tex[0]->w), py=(int)(fv*(float)d->tex[0]->h);
        if (px>=0 && py>=0 && px<4096 && py<4096) { probe_pos=1u+((uint32_t)px<<12|(uint32_t)py); PSET(sh3_probe,probe_pos); }
    }
#endif
    prep_draw(d, ARG(2), 0, prim_verts(ARG(2), ARG(3)), ARG(3), NULL, 0, 0, mb + ARG(4), ARG(5));
#ifdef __vita__
    if (probe_pos) PSET(sh3_probe,0u);
#endif
    FILM_RESTORE();
    FADEBOX_RESTORE();
    STDRET(5, HR_OK);
}
#undef FADEBOX_RESTORE
#undef FILM_RESTORE
#undef FILM_IS_GAME_TEX
M(DrawIndexedPrimitiveUP) {
    HOSTFN; DEV;
    uint32_t type = ARG(2), minidx = ARG(3), n = ARG(5);
    fadeseq_draw(d,"DrawIdxUP",type,n);
    prep_draw(d, type, 0, prim_verts(type, n), n, mb + ARG(6), ARG(7) == FMT_INDEX32, minidx, mb + ARG(8), ARG(9));
    STDRET(9, HR_OK);
}
M(ProcessVertices) { d3d_log_once("ProcessVertices"); STDRET(6, D3DERR_INVALIDCALL); }
M(CreateVertexShader) {
    HOSTFN;
    static unsigned s_n;
    uint32_t h = vs_create(ARG(2), ARG(3));
    if (++s_n <= 3 || s_n % 50 == 0) rt_log("[vs] CreateVertexShader #%u -> 0x%x\n", s_n, h);
    W32(ARG(4), h);
    STDRET(5, h ? HR_OK : D3DERR_INVALIDCALL);
}
M(SetVertexShader) { HOSTFN; DEV; PSET(vs, ARG(2)); STDRET(2, HR_OK); }
M(GetVertexShader) { HOSTFN; DEV; W32(ARG(2), d->vs); STDRET(2, HR_OK); }
M(DeleteVertexShader) { HOSTFN; prep_vsdel(ARG(2)); STDRET(2, HR_OK); }
M(SetVertexShaderConstant) {
    HOSTFN; DEV;
    uint32_t r = ARG(2), n = ARG(4);
    if (r + n > MAX_VSC) n = r < MAX_VSC ? MAX_VSC - r : 0;
    if (n) prep_set(d->vsc[r], mb + ARG(3), 16u * n);
    STDRET(4, HR_OK);
}

/* GOG v5 0x428e64..0x428e9d: upload twenty three-vector model matrices.
   No draw or state read occurs between those uploads. Assemble the resulting
   constant interval once, preserving the slots whose index is -1. Keep the
   original guest register/stack effects for the surrounding generated code. */
int d3d_model_palette(CPU *c) {
    static int enabled = -1;
    if (enabled < 0) {
        const char *s = getenv("SH4_NATIVE_PALETTE");
        enabled = s && !strcmp(s, "1");
    }
    if (!enabled) return 0;
    uint8_t *const mb = g_mem;
    uint32_t guest = R32(0x6e7d94u);
    Device *d = g_dev;
    if (!d || d->com.guest != guest ||
        rt_lookup(R32(R32(guest) + 0x13cu)) != dev_SetVertexShaderConstant) return 0;

    uint32_t table = R32(c->esp + 0x10u), last = 0xffffffffu;
    float palette[60][4];
    memcpy(palette, d->vsc[34], sizeof palette);
    for (uint32_t i = 0; i < 20; ++i) {
        uint32_t index = R32(table + 4u * i);
        c->eax = index;
        if (index == 0xffffffffu) continue;
        uint32_t src = 0xf629c0u + (index << 6);
        memcpy(palette[3u * i], mb + src, 48);
        W32(c->esp - 4u, 3);
        W32(c->esp - 8u, src);
        W32(c->esp - 12u, 34u + 3u * i);
        W32(c->esp - 16u, guest);
        W32(c->esp - 20u, 0x428e8au);
        c->eax = HR_OK;
        c->ecx = guest;
        last = i;
    }
    if (last != 0xffffffffu) prep_set(d->vsc[34], palette, sizeof palette);
    c->edx = table + 80u;
    c->edi = 94;
    W32(c->esp + 0x10u, c->edx);
    return 1;
}
M(GetVertexShaderConstant) {
    HOSTFN; DEV;
    uint32_t r = ARG(2), n = ARG(4);
    if (r + n > MAX_VSC) n = r < MAX_VSC ? MAX_VSC - r : 0;
    memcpy(mb + ARG(3), d->vsc[r], 16u * n);
    STDRET(4, HR_OK);
}
M(GetVertexShaderDeclaration) { d3d_log_once("GetVertexShaderDeclaration"); STDRET(4, D3DERR_INVALIDCALL); }
M(GetVertexShaderFunction) { d3d_log_once("GetVertexShaderFunction"); STDRET(4, D3DERR_INVALIDCALL); }
M(SetStreamSource) {
    HOSTFN; DEV;
    uint32_t n = ARG(2);
    if (n >= 16) STDRET(4, D3DERR_INVALIDCALL);
    Buffer *b = ARG(3) ? com_get_kind(ARG(3), K_VB) : NULL;
    Buffer *o = d->stream[n].vb;
    addref(b);
    PSET(stream[n].vb, b);
    PSET(stream[n].stride, ARG(4));
    if (o) d3d_release(&o->com);
    STDRET(4, HR_OK);
}
M(GetStreamSource) {
    HOSTFN; DEV;
    uint32_t n = ARG(2) & 15;
    addref(d->stream[n].vb);
    W32(ARG(3), d->stream[n].vb ? d->stream[n].vb->com.guest : 0);
    W32(ARG(4), d->stream[n].stride);
    STDRET(4, HR_OK);
}
M(SetIndices) {
    HOSTFN; DEV;
    Buffer *b = ARG(2) ? com_get_kind(ARG(2), K_IB) : NULL;
    Buffer *o = d->ib;
    addref(b);
    PSET(ib, b);
    PSET(base_vertex, ARG(3));
    if (o) d3d_release(&o->com);
    STDRET(3, HR_OK);
}
M(GetIndices) {
    HOSTFN; DEV;
    addref(d->ib);
    W32(ARG(2), d->ib ? d->ib->com.guest : 0);
    W32(ARG(3), d->base_vertex);
    STDRET(3, HR_OK);
}
M(CreatePixelShader) {
    HOSTFN;
    uint32_t h = ps_create(ARG(2));
    W32(ARG(3), h);
    STDRET(3, h ? HR_OK : D3DERR_INVALIDCALL);
}
M(SetPixelShader) { HOSTFN; DEV; PSET(ps, ARG(2)); STDRET(2, HR_OK); }
M(GetPixelShader) { HOSTFN; DEV; W32(ARG(2), d->ps); STDRET(2, HR_OK); }
M(DeletePixelShader) { HOSTFN; prep_psdel(ARG(2)); STDRET(2, HR_OK); }
M(SetPixelShaderConstant) {
    HOSTFN; DEV;
    uint32_t r = ARG(2), n = ARG(4);
    if (r + n > 8) n = r < 8 ? 8 - r : 0;
    if (n) prep_set(d->psc[r], mb + ARG(3), 16u * n);
    STDRET(4, HR_OK);
}
M(GetPixelShaderConstant) {
    HOSTFN; DEV;
    uint32_t r = ARG(2), n = ARG(4);
    if (r + n > 8) n = r < 8 ? 8 - r : 0;
    memcpy(mb + ARG(3), d->psc[r], 16u * n);
    STDRET(4, HR_OK);
}
M(GetPixelShaderFunction) { STDRET(4, D3DERR_INVALIDCALL); }
M(DrawRectPatch) { d3d_log_once("DrawRectPatch"); STDRET(4, HR_OK); }
M(DrawTriPatch) { d3d_log_once("DrawTriPatch"); STDRET(4, HR_OK); }
M(DeletePatch) { STDRET(2, HR_OK); }

static const gfn dev_vt[] = {
    dev_QueryInterface, dev_AddRef, dev_Release, dev_TestCooperativeLevel, dev_GetAvailableTextureMem,
    dev_ResourceManagerDiscardBytes, dev_GetDirect3D, dev_GetDeviceCaps, dev_GetDisplayMode,
    dev_GetCreationParameters, dev_SetCursorProperties, dev_SetCursorPosition, dev_ShowCursor,
    dev_CreateAdditionalSwapChain, dev_Reset, dev_Present, dev_GetBackBuffer, dev_GetRasterStatus,
    dev_SetGammaRamp, dev_GetGammaRamp, dev_CreateTexture, dev_CreateVolumeTexture, dev_CreateCubeTexture,
    dev_CreateVertexBuffer, dev_CreateIndexBuffer, dev_CreateRenderTarget, dev_CreateDepthStencilSurface,
    dev_CreateImageSurface, dev_CopyRects, dev_UpdateTexture, dev_GetFrontBuffer, dev_SetRenderTarget,
    dev_GetRenderTarget, dev_GetDepthStencilSurface, dev_BeginScene, dev_EndScene, dev_Clear, dev_SetTransform,
    dev_GetTransform, dev_MultiplyTransform, dev_SetViewport, dev_GetViewport, dev_SetMaterial, dev_GetMaterial,
    dev_SetLight, dev_GetLight, dev_LightEnable, dev_GetLightEnable, dev_SetClipPlane, dev_GetClipPlane,
    dev_SetRenderState, dev_GetRenderState, dev_BeginStateBlock, dev_EndStateBlock, dev_ApplyStateBlock,
    dev_CaptureStateBlock, dev_DeleteStateBlock, dev_CreateStateBlock, dev_SetClipStatus, dev_GetClipStatus,
    dev_GetTexture, dev_SetTexture, dev_GetTextureStageState, dev_SetTextureStageState, dev_ValidateDevice,
    dev_GetInfo, dev_SetPaletteEntries, dev_GetPaletteEntries, dev_SetCurrentTexturePalette,
    dev_GetCurrentTexturePalette, dev_DrawPrimitive, dev_DrawIndexedPrimitive, dev_DrawPrimitiveUP,
    dev_DrawIndexedPrimitiveUP, dev_ProcessVertices, dev_CreateVertexShader, dev_SetVertexShader,
    dev_GetVertexShader, dev_DeleteVertexShader, dev_SetVertexShaderConstant, dev_GetVertexShaderConstant,
    dev_GetVertexShaderDeclaration, dev_GetVertexShaderFunction, dev_SetStreamSource, dev_GetStreamSource,
    dev_SetIndices, dev_GetIndices, dev_CreatePixelShader, dev_SetPixelShader, dev_GetPixelShader,
    dev_DeletePixelShader, dev_SetPixelShaderConstant, dev_GetPixelShaderConstant, dev_GetPixelShaderFunction,
    dev_DrawRectPatch, dev_DrawTriPatch, dev_DeletePatch};

static void default_states(Device *d, int autodepth) {
    static const uint32_t rs_def[][2] = {
        {8, 3}, {9, 2}, {14, 1}, {16, 1}, {19, 2}, {20, 1}, {22, 3}, {23, 4}, {25, 8}, {37, 0x3f800000},
        {38, 0x3f800000}, {53, 1}, {54, 1}, {55, 1}, {56, 8}, {58, 0xffffffff}, {59, 0xffffffff},
        {60, 0xffffffff}, {136, 1}, {137, 1}, {141, 1}, {142, 1}, {145, 1}, {146, 2}, {154, 0x3f800000},
        {161, 1}, {162, 0xffffffff}, {166, 0x42800000}, {168, 0xf}, {171, 1}};
    memset(d->rs, 0, sizeof d->rs);
    for (unsigned i = 0; i < sizeof rs_def / sizeof rs_def[0]; i++) d->rs[rs_def[i][0]] = rs_def[i][1];
    d->rs[7] = autodepth ? 1 : 0;
    for (int s = 0; s < MAX_STAGES; s++) {
        uint32_t *t = d->tss[s];
        memset(t, 0, 32 * 4);
        t[1] = s ? 1 : 4; t[2] = 2; t[3] = 1; t[4] = s ? 1 : 2; t[5] = 2; t[6] = 1;
        t[11] = (uint32_t)s; t[13] = 1; t[14] = 1; t[16] = 1; t[17] = 1; t[18] = 0; t[21] = 1;
        t[25] = 1; t[26] = 1; t[27] = 1; t[28] = 1;
    }
    for (int i = 0; i < 16; i++) d->world[i] = d->view[i] = d->proj[i] = (i % 5 == 0);
    for (int s = 0; s < MAX_STAGES; s++)
        for (int i = 0; i < 16; i++) d->texm[s][i] = (i % 5 == 0);
    for (int c = 0; c < 3; c++)
        for (int i = 0; i < 256; i++) d->gamma[c][i] = (uint16_t)(i * 257);
}

uint32_t device_create(uint32_t pp) {
    HOSTFN;
    Device *d = calloc(1, sizeof *d);
    d->bb_w = R32(pp) ? R32(pp) : 640;
    d->bb_h = R32(pp + 4) ? R32(pp + 4) : 480;
    default_states(d, R32(pp + 32));
    d->vp.w = d->bb_w; d->vp.h = d->bb_h; d->vp.maxz = 1.0f;
    d->backbuffer = surf_create(d->bb_w, d->bb_h, FMT_X8R8G8B8, 0);
    d->backbuffer->is_backbuffer = 1;
    surf_wrap(d->backbuffer);
    if (R32(pp + 32)) {
        d->depth = surf_create(d->bb_w, d->bb_h, R32(pp + 36), 1);
        surf_wrap(d->depth);
    }
    d->rt = d->backbuffer; d->rt->com.refs++;
    d->ds = d->depth; if (d->ds) d->ds->com.refs++;
    g_dev = d;
    prep_init(d);
    prep_gfx_init();
    uint32_t guest=com_wrap(&d->com, K_DEVICE, dev_vt, 97, "IDirect3DDevice8");
#ifdef SH3_RENDERSTATE_LAB
    { uint8_t *const mb=g_mem;g_diag_renderstate_target=R32(R32(guest)+50u*4u); }
#endif
#ifdef SH3_SHADOW_NATIVE_LAB
    { uint8_t *const mb=g_mem;g_diag_gettransform_target=R32(R32(guest)+38u*4u); }
#endif
    return guest;
}
