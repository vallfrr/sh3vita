/* Experimental SH3 Vita platform. Not a playable or production frontend. */
#include "rt.h"
#include <vitaGL.h>
#include <psp2/ctrl.h>
#include <psp2/kernel/sysmem.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/power.h>
#include <SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>

/* vitaGL invokes its optional animated splash through an external symbol.
 * The linker redirects only this call; the rest of the vetted library stays
 * byte-identical.  No splash thread is started and its active flag stays zero. */
void __wrap_invoke_splashscreen(void) {}

uint64_t g_t_sleep,g_t_gl,g_t_swap,g_t_io,g_t_wait,g_io_bytes,g_heap_used,g_heap_peak;
unsigned g_cursor_seen;
int g_movie_w,g_movie_h,g_audit_enabled,g_audit_frames,g_audit_checks,g_vita_bindings_open;
static unsigned capture_requests;
int g_diag_draw_cut,g_diag_prep_cut;
unsigned g_diag_pass_phase,g_diag_pass_skipped[2];
int g_diag_worker_budget;
unsigned g_diag_worker_eligible,g_diag_worker_skipped[2];
char g_game_dir[512]="ux0:data/sh3/cache";
void heap_report(void) {}
#ifndef SH3_MOVIE_LAB
void movie_audio_mix(float *samples,int n) { (void)samples; (void)n; }
uint16_t *movie_565_take(uint32_t w,uint32_t h,volatile int **busy) { (void)w; (void)h; (void)busy; return NULL; }
#else
void diag_movie_capture_draw(unsigned index) {
    GLint viewport[4]={0}, framebuffer=0;
    glGetIntegerv(GL_VIEWPORT,viewport);
    glGetIntegerv(GL_FRAMEBUFFER_BINDING,&framebuffer);
    int w=viewport[2],h=viewport[3];
    fprintf(stderr,"[movie-lab] current framebuffer=%d viewport=%d,%d %dx%d\n",framebuffer,viewport[0],viewport[1],w,h);
    if (w<=0 || h<=0 || w>2048 || h>2048) { fprintf(stderr,"[movie-lab] capture dimensions unsupported\n"); return; }
    char path[96];
    if (index && index<=3) snprintf(path,sizeof path,"ux0:data/sh3/diag/movie-current-%u.tga",index);
    else snprintf(path,sizeof path,"ux0:data/sh3/diag/movie-current.tga");
    FILE *f=fopen(path,"wb");
    unsigned char *row=malloc((size_t)w*4);
    if (!f || !row) { if(f)fclose(f); free(row); fprintf(stderr,"[movie-lab] current target capture allocation failed\n"); return; }
    unsigned char header[18]={0,0,2,0,0,0,0,0,0,0,0,0,(unsigned char)w,(unsigned char)(w>>8),(unsigned char)h,(unsigned char)(h>>8),32,0x28};
    int ok=fwrite(header,1,sizeof header,f)==sizeof header;
    for (int y=h-1;y>=0 && ok;y--) {
        glReadPixels(viewport[0],viewport[1]+y,w,1,GL_RGBA,GL_UNSIGNED_BYTE,row);
        for (int x=0;x<w;x++) { unsigned char v=row[4*x]; row[4*x]=row[4*x+2]; row[4*x+2]=v; }
        ok=fwrite(row,4,(size_t)w,f)==(size_t)w;
    }
    if (fclose(f)) ok=0;
    free(row);
    fprintf(stderr,"[movie-lab] current target capture %s path=%s gl_error=%x\n",ok?"saved":"failed",path,(unsigned)glGetError());
}
#endif
void rt_audit_frame(unsigned f) {
    if (!getenv("SH4_CPU_TIME") || f%300u || g_mem_size<0x070e84c0u) return;
    /* Read-only sampling of callback slots used by the observed SH3 worker.
       The addresses belong to this executable variant, not the SH4 runtime. */
    uint32_t hooks[8];
    memcpy(&hooks[0],g_mem+0x070e83d0u,4);
    memcpy(&hooks[1],g_mem+0x070e6994u,4);
    for (unsigned i=0;i<6;i++) memcpy(&hooks[2+i],g_mem+0x070e8490u+8*i,4);
    fprintf(stderr,"[diag] worker hooks frame=%u outer=%08x tail=%08x slots=%08x,%08x,%08x,%08x,%08x,%08x\n",
            f,hooks[0],hooks[1],hooks[2],hooks[3],hooks[4],hooks[5],hooks[6],hooks[7]);
}
int rt_audit_capture(unsigned f,char *p,size_t n) { (void)f; (void)p; (void)n; return 0; }
void diag_capture_request(unsigned which) {
    if (which==1 || which==2) __atomic_fetch_or(&capture_requests,1u<<which,__ATOMIC_RELAXED);
}
int diag_window_show(int show) { static int visible; int old=visible; visible=show; return old; }
void diag_window_ungrab(void) {}
int diag_window_focused(void) { return 1; }
int diag_display_mode(uint32_t index,uint32_t *w,uint32_t *h,uint32_t *bits,uint32_t *hz) {
    if (index!=0 && index!=UINT32_MAX) return 0;
    *w=960; *h=544; *bits=32; *hz=60; return 1;
}
int diag_display_mode_supported(uint32_t w,uint32_t h,uint32_t bits,uint32_t hz) {
    return w==960 && h==544 && bits==32 && (hz<=1 || hz==60);
}
/* Keep-awake from start-up to exit, independent of frame presentation (long loads, movies, stalls): the console must
   never suspend or turn the screen off under a run. The present path keeps its own ticks as well. */
static int keep_awake_thread(SceSize args, void *argp) {
    (void)args; (void)argp;
    for (;;) {
        sceKernelPowerTick(SCE_KERNEL_POWER_TICK_DISABLE_AUTO_SUSPEND);
        sceKernelPowerTick(SCE_KERNEL_POWER_TICK_DISABLE_OLED_OFF);
        sceKernelPowerTick(SCE_KERNEL_POWER_TICK_DISABLE_OLED_DIMMING);
        sceKernelDelayThread(5u * 1000u * 1000u);
    }
    return 0;
}
/* Suspend/resume (crash 2026-11-17, second test Vita: sleep ~30 s in game, then the render thread faulted in SceGxm at the
   first present after wake-up, reading unmapped memory). The port registered no power callback: the console froze the
   GPU in the middle of a scene. Now a callback thread raises a pause on the power button / system suspend; the render
   thread (gfx_present -> plat_power_gate) waits for the GPU (glFinish) and stays parked until the system has resumed.
   The callback itself touches no GPU, file or guest memory; it only waits (bounded) for the render thread to park. */
static volatile int power_pause, power_parked;
static int power_event(int id, int count, int flags, void *arg) {
    (void)id; (void)count; (void)arg;
    const int going = flags & (SCE_POWER_CB_BUTTON_POWER_PRESS | SCE_POWER_CB_SYSTEM_SUSPEND | SCE_POWER_CB_APP_SUSPEND);
    const int back = flags & (SCE_POWER_CB_SYSTEM_RESUME | SCE_POWER_CB_AFTER_SYSTEM_RESUME | SCE_POWER_CB_APP_RESUME);
    if (going && !power_pause) {
        power_pause = 1;
        for (int i = 0; i < 100 && !power_parked; i++) sceKernelDelayThread(5000);   /* at most 0.5 s */
    }
    if (back && power_pause) power_pause = 0;
    if (going || back) fprintf(stderr, "[power] flags=%08x pause=%d parked=%d\n", (unsigned)flags, power_pause, power_parked);
    return 0;
}
static int power_thread(SceSize args, void *argp) {
    (void)args; (void)argp;
    SceUID cb = sceKernelCreateCallback("sh3_power", 0, power_event, NULL);
    int r = cb < 0 ? cb : scePowerRegisterCallback(cb);
    fprintf(stderr, "[power] callback=%08x registered=%08x\n", (unsigned)cb, (unsigned)r);
    for (;;) sceKernelDelayThreadCB(1000u * 1000u);
    return 0;
}
/* Render thread, start of each present. One volatile load per frame when nothing happens. */
void plat_power_gate(void) {
    if (!power_pause) return;
    glFinish();
    power_parked = 1;
    /* Process time does not advance while the console sleeps: 3 s of it without a resume event means no suspend
       happened (e.g. power menu cancelled), so never stay parked forever. */
    SceUInt64 t0 = sceKernelGetProcessTimeWide();
    while (power_pause && sceKernelGetProcessTimeWide() - t0 < 3000000u) sceKernelDelayThread(10000);
    power_pause = 0;
    power_parked = 0;
    fprintf(stderr, "[power] render thread resumed\n");
}
void plat_gl_init(void) {
    static int initialized; if (initialized) return; initialized=1;
    { SceUID t=sceKernelCreateThread("sh3_keep_awake",keep_awake_thread,0xa0,0x1000,0,0,NULL);
      if (t>=0) sceKernelStartThread(t,0,NULL);
      fprintf(stderr,"[diag] keep-awake thread %s\n",t>=0?"started":"FAILED"); }
    { SceUID t=sceKernelCreateThread("sh3_power",power_thread,0x40,0x2000,0,0,NULL);
      if (t>=0) sceKernelStartThread(t,0,NULL);
      if (t<0) fprintf(stderr,"[power] callback thread FAILED %08x\n",(unsigned)t); }
    extern unsigned g_diag_perf;
    /* Sleep/resume A/B (2026-11-17: SceGxm fault after waking the console; SH4 survives sleep with the same vitaGL, so
       test the three SH3-only GPU settings). Persistent file ux0:data/sh3/diag/sleeptest (NOT consumed, so several
       sleeps can be tried), words: scenes1 (1 GXM scene per target instead of 8), noreserve (no 8 MB phycont reserve
       for the movie decoder), nolazy (depth attached at bind time, no lazy depth). Absent file: unchanged behaviour. */
    int sleep_scenes1=0, sleep_noreserve=0;
    { FILE *m=fopen("ux0:data/sh3/diag/sleeptest","rb"); if (m) { char v[128]={0}; size_t n=fread(v,1,sizeof v-1,m); fclose(m); v[n]=0;
        sleep_scenes1=strstr(v,"scenes1")!=NULL; sleep_noreserve=strstr(v,"noreserve")!=NULL;
        if (strstr(v,"nolazy")) setenv("SH3_LAZYDEPTH","0",1);
        fprintf(stderr,"[sleeptest] scenes1=%d noreserve=%d nolazy=%d\n",sleep_scenes1,sleep_noreserve,strstr(v,"nolazy")!=NULL); } }
#ifndef SH3_GXM_SCENES8_DEFAULT
#define SH3_GXM_SCENES8_DEFAULT 1 /* promoted 2026-11-12 with lazy depth; -DSH3_GXM_SCENES8_DEFAULT=0 disables */
#endif
#if SH3_GXM_SCENES8_DEFAULT
    if (!sleep_scenes1) {
    vglSetupRenderTargetScenesNum(8,8);
    fprintf(stderr,"[diag] GXM scenes per target 8/8 (compiled default)\n");
    }
#endif
    { FILE *m=fopen("ux0:data/sh3/diag/gxmscenes8","rb"); if (m) { fclose(m); unlink("ux0:data/sh3/diag/gxmscenes8"); vglSetupRenderTargetScenesNum(8,8); fprintf(stderr,"[diag] GXM scenes per target 8/8 (marker)\n"); } }
    if (g_diag_perf&32768u) {
        vglSetupRenderTargetScenesNum(2,4);
        fprintf(stderr,"[diag] experimental GXM scenes per target requested display=2 fbo=4\n");
    }
    vglSetVDMBufferSize(1024*1024); vglSetFragmentBufferSize(1024*1024); vglSetUSSEBufferSize(2048*1024);
    /* Experiment (marker pbuf=MB): GXM parameter buffer size. With 8 scenes per target in flight, an overflowing
       parameter buffer forces partial renders on the GPU; tests whether the slow "heavy" park state comes from it. */
    { FILE *m=fopen("ux0:data/sh3/diag/pbuf","rb"); if (m) { char v[16]={0}; if (!fgets(v,sizeof v,m)) v[0]=0; fclose(m); unlink("ux0:data/sh3/diag/pbuf");
        unsigned mb=(unsigned)atoi(v); if (mb>=4 && mb<=64) { vglSetParamBufferSize(mb*1024u*1024u); fprintf(stderr,"[diag] GXM parameter buffer %u MB (marker)\n",mb); } } }
    SceKernelFreeMemorySizeInfo info={.size=sizeof info};
    if (sceKernelGetFreeMemorySize(&info)<0) rt_fatal("[diag] Vita free memory query failed");
    int threshold=(int)info.size_user-(32*1024*1024);
    fprintf(stderr,"[memory-budget] guest=%u user_free_before_vgl=%u cdram_free=%u phycont_free=%u vgl_threshold=%d\n",g_mem_size,info.size_user,info.size_cdram,info.size_phycont,threshold);
    if (threshold<0) rt_fatal("[diag] not enough free Vita RAM for graphics");
#ifdef SH3_MOVIE_HW_LAB
    /* vglInitExtended takes all phycont/cdram; the AvPlayer hardware decoder then cannot allocate its frame
       buffers (v4: no frame, no audio after 15 s). Lab only: leave 8 MB phycont + 32 MB cdram free, the values
       the vitaGL video_playback sample uses. cdlg 0x8C6000
       keeps the common-dialog budget exactly like vglInitExtended. */
    /* cdram threshold 0: vitaGL keeps all CDRAM, as vglInitExtended (a 32 MB reserve was suspected of the
       <15 FPS gameplay regression; benches v5-v9 show AvPlayer using only 2+5+1 MB of phycont for 512x384). */
    /* A/B marker noreserve: stock vglInitExtended (no phycont reserve) to measure its gameplay cost. */
    FILE *nr=sleep_noreserve?NULL:fopen("ux0:data/sh3/diag/noreserve","rb");
    GLboolean reduced;
    if(nr || sleep_noreserve) {
        if (nr) { fclose(nr);unlink("ux0:data/sh3/diag/noreserve"); }
        reduced=vglInitExtended(0,960,544,threshold,SCE_GXM_MULTISAMPLE_NONE);
        fprintf(stderr,"[movie-hw] noreserve: stock vglInitExtended (AvPlayer frames may fail)\n");
    } else {
        reduced=vglInitWithCustomThreshold(0,960,544,threshold,0,8<<20,0x8C6000,SCE_GXM_MULTISAMPLE_NONE);
        fprintf(stderr,"[movie-hw] vitaGL leaves phycont 8 MB for AvPlayer (cdram unchanged)\n");
    }
#else
    GLboolean reduced=vglInitExtended(0,960,544,threshold,SCE_GXM_MULTISAMPLE_NONE);
#endif
    fprintf(stderr,"[diag] VitaGL initialized; resolution reduced=%d\n",reduced);
    extern unsigned g_diag_perf;
    if (g_diag_perf&8u) {   /* PERF_NOVSYNC: frames far above 16.7 ms otherwise wait up to one more refresh each */
        vglWaitVblankStart(GL_FALSE);
        fprintf(stderr,"[diag] vertical sync wait disabled (perf novsync)\n");
    }
    sceCtrlSetSamplingMode(SCE_CTRL_MODE_ANALOG);
    sceCtrlSetSamplingModeExt(SCE_CTRL_MODE_ANALOG);
}
static void bench_shot_half(const char *path) {
    FILE *f=fopen(path,"wb"); if (!f) { fprintf(stderr,"[bench] shot open FAILED %s\n",path); return; }
    const unsigned char header[18]={0,0,2,0,0,0,0,0,0,0,0,0,480&255,480>>8,272&255,272>>8,32,0x28};
    unsigned char r0[960*4],r1[960*4],out[480*4]; int ok=fwrite(header,1,sizeof header,f)==sizeof header;
    glBindFramebuffer(GL_FRAMEBUFFER,0);
    for (int y=271;y>=0 && ok;y--) {
        glReadPixels(0,2*y,960,1,GL_RGBA,GL_UNSIGNED_BYTE,r0); glReadPixels(0,2*y+1,960,1,GL_RGBA,GL_UNSIGNED_BYTE,r1);
        for (int x=0;x<480;x++) {
            out[4*x]=(unsigned char)((r0[8*x+2]+r0[8*x+6]+r1[8*x+2]+r1[8*x+6])>>2);
            out[4*x+1]=(unsigned char)((r0[8*x+1]+r0[8*x+5]+r1[8*x+1]+r1[8*x+5])>>2);
            out[4*x+2]=(unsigned char)((r0[8*x]+r0[8*x+4]+r1[8*x]+r1[8*x+4])>>2);
            out[4*x+3]=255;
        }
        ok=fwrite(out,1,sizeof out,f)==sizeof out;
    }
    if (fclose(f)) ok=0;
    fprintf(stderr,"[bench] shot %s %s 480x272\n",path,ok?"saved":"FAILED");
}
/* Bench producer and queued presentation consumer can run on different
   threads. A single unsynchronised path silently lost requests. */
static char bench_shots[64][256];
static unsigned bench_shot_read,bench_shot_write;
static SDL_SpinLock bench_shot_lock;
void diag_bench_shot(const char *name) {
    SDL_AtomicLock(&bench_shot_lock);
    if(bench_shot_write-bench_shot_read==64) {
        SDL_AtomicUnlock(&bench_shot_lock);
        rt_fatal("[bench] capture request queue full");
    }
    snprintf(bench_shots[bench_shot_write++%64],256,"%s.tga",name);
    SDL_AtomicUnlock(&bench_shot_lock);
}
static int bench_take_shot(char path[256]) {
    SDL_AtomicLock(&bench_shot_lock);
    int available=bench_shot_read!=bench_shot_write;
    if(available)memcpy(path,bench_shots[bench_shot_read++%64],256);
    SDL_AtomicUnlock(&bench_shot_lock);
    return available;
}
void plat_shot_raw(const char *requested) {
    const char *path="ux0:data/sh3/diag/shot.tga";
    if (requested && !strncmp(requested,"ux0:",4)) path=requested;
    if (requested && !strcmp(requested,"move-before")) path="ux0:data/sh3/diag/move-before.tga";
    else if (requested && !strcmp(requested,"move-after")) path="ux0:data/sh3/diag/move-after.tga";
    else if (requested && !strcmp(requested,"movie-probe")) path="ux0:data/sh3/diag/movie-probe.tga";
    FILE *f=fopen(path,"wb"); if (!f) { fprintf(stderr,"[diag] Vita screenshot open failed\n"); return; }
    const unsigned char header[18]={0,0,2,0,0,0,0,0,0,0,0,0,960&255,960>>8,544&255,544>>8,32,0x28};
    unsigned char row[960*4]; int ok=fwrite(header,1,sizeof header,f)==sizeof header;
    glBindFramebuffer(GL_FRAMEBUFFER,0);
    for (int y=543;y>=0 && ok;y--) {
        glReadPixels(0,y,960,1,GL_RGBA,GL_UNSIGNED_BYTE,row);
        for (int x=0;x<960;x++) { unsigned char tmp=row[4*x]; row[4*x]=row[4*x+2]; row[4*x+2]=tmp; }
        ok=fwrite(row,1,sizeof row,f)==sizeof row;
    }
    if (fclose(f)) ok=0;
    fprintf(stderr,"[diag] Vita screenshot %s: %s at %llu ms\n",ok?"saved":"failed",path,(unsigned long long)SDL_GetTicks64());
}

#ifdef SH3_SCENE_COUNT
#include <psp2/gxm.h>
volatile unsigned g_gxm_scenes;
int __real_sceGxmBeginScene(SceGxmContext *c,unsigned f,const SceGxmRenderTarget *rt,const SceGxmValidRegion *v,SceGxmSyncObject *a,SceGxmSyncObject *b,const SceGxmColorSurface *cs,const SceGxmDepthStencilSurface *ds);
int __wrap_sceGxmBeginScene(SceGxmContext *c,unsigned f,const SceGxmRenderTarget *rt,const SceGxmValidRegion *v,SceGxmSyncObject *a,SceGxmSyncObject *b,const SceGxmColorSurface *cs,const SceGxmDepthStencilSurface *ds) { g_gxm_scenes++; return __real_sceGxmBeginScene(c,f,rt,v,a,b,cs,ds); }
#endif
void plat_gl_swap(void) {
    { /* Diagnostic (marker gpufinish): time spent waiting for the GPU to finish this frame before presenting. */
      static int gf=-1; static Uint64 acc,t_last; static unsigned nfr;
      if (gf<0) { FILE *m=fopen("ux0:data/sh3/diag/gpufinish","rb"); gf=m!=NULL; if (m) { fclose(m); unlink("ux0:data/sh3/diag/gpufinish"); fprintf(stderr,"[gpufinish] enabled\n"); } }
      if (gf) { Uint64 a=SDL_GetPerformanceCounter(); glFinish(); acc+=SDL_GetPerformanceCounter()-a; nfr++;
        Uint64 now=SDL_GetTicks64(); if (!t_last) t_last=now; if (now-t_last>=2000 && nfr) { { unsigned sc=0;
#ifdef SH3_SCENE_COUNT
        static unsigned last_sc; sc=g_gxm_scenes-last_sc; last_sc=g_gxm_scenes;
#endif
        fprintf(stderr,"[gpufinish] %.2f ms/frame waiting for GPU completion over %u frames, %.1f GXM scenes/frame\n",(double)acc*1000.0/(double)SDL_GetPerformanceFrequency()/nfr,nfr,(double)sc/nfr); } acc=0; nfr=0; t_last=now; } } }
    /* Match the existing SH4 keep-awake policy, only while SH3 presents.
       The agent's Shell tick prevents suspend but not OLED off/dimming. */
    static Uint64 awake_tick;
    Uint64 awake_now=SDL_GetTicks64();
    if (!awake_tick || awake_now-awake_tick>=5000u) {
        int suspend=sceKernelPowerTick(SCE_KERNEL_POWER_TICK_DISABLE_AUTO_SUSPEND);
        int off=sceKernelPowerTick(SCE_KERNEL_POWER_TICK_DISABLE_OLED_OFF);
        int dim=sceKernelPowerTick(SCE_KERNEL_POWER_TICK_DISABLE_OLED_DIMMING);
        if (!awake_tick || suspend || off || dim)
            fprintf(stderr,"[diag] in-game wake ticks suspend=%d off=%d dim=%d interval_ms=5000\n",suspend,off,dim);
        awake_tick=awake_now;
    }
    static unsigned frames;
    static Uint64 first_swap;
    static int late_checked;
    static unsigned long late_delay=110000;
    static Uint64 sample_start;
    static unsigned sample_frames;
    if (!first_swap) {
        first_swap=SDL_GetTicks64();
        FILE *schedule=fopen("ux0:data/sh3/diag/shotlate","rb");
        if (schedule) {
            unsigned long requested=0;
            if (fscanf(schedule,"%lu",&requested)==1 && requested>=1000 && requested<=600000)
                late_delay=requested;
            fclose(schedule);
        }
    }
    ++frames;
    Uint64 sample_now=SDL_GetTicks64();
    static int budget_passes=-1,pass_phase=-1;
    if (budget_passes<0) budget_passes=getenv("SH3_DIAG_BUDGET_PASSES")!=NULL;
    if (budget_passes) {
        uint32_t room=0; memcpy(&room,g_mem+0x00897e48u,4);
        unsigned stage=g_mem[0x070e66d9u];
        unsigned phase=stage==33 && room==210 && sample_now>=120000u && sample_now<150000u?1:
                       stage==33 && room==210 && sample_now>=160000u && sample_now<190000u?2:0;
        __atomic_store_n(&g_diag_pass_phase,phase,__ATOMIC_RELAXED);
        if ((int)phase!=pass_phase) {
            fprintf(stderr,"[budget-passes] ms=%llu swap=%u phase=%u stage=%u room=%u skipped_small144=%u skipped_vs2f=%u\n",
                    (unsigned long long)sample_now,frames,phase,stage,room,
                    __atomic_load_n(&g_diag_pass_skipped[0],__ATOMIC_RELAXED),
                    __atomic_load_n(&g_diag_pass_skipped[1],__ATOMIC_RELAXED));
            pass_phase=(int)phase;
        }
    }
    if (g_diag_worker_budget) {
        static int previous=-1;
        uint32_t room=0; memcpy(&room,g_mem+0x00897e48u,4);
        unsigned stage=g_mem[0x070e66d9u];
        unsigned eligible=stage==33 && room==210;
        __atomic_store_n(&g_diag_worker_eligible,eligible,__ATOMIC_RELAXED);
        int phase=eligible && sample_now>=120000u && sample_now<150000u?1:
                  eligible && sample_now>=160000u && sample_now<190000u?2:0;
        if (phase!=previous) {
            fprintf(stderr,"[budget-worker] ms=%llu swap=%u phase=%d stage=%u room=%u skipped_60f740=%u skipped_613370=%u\n",
                    (unsigned long long)sample_now,frames,phase,stage,room,
                    __atomic_load_n(&g_diag_worker_skipped[0],__ATOMIC_RELAXED),
                    __atomic_load_n(&g_diag_worker_skipped[1],__ATOMIC_RELAXED));
            previous=phase;
        }
    }
    static int budget_draw=-1,budget_phase=-1;
    if (budget_draw<0) budget_draw=getenv("SH3_DIAG_BUDGET_DRAW")!=NULL;
    if (budget_draw) {
        uint32_t room=0; memcpy(&room,g_mem+0x00897e48u,4);
        unsigned stage=g_mem[0x070e66d9u];
        int phase=0;
        if (stage==33 && room==210) {
            if (sample_now>=120000u && sample_now<150000u) phase=1;
            else if (sample_now>=160000u && sample_now<190000u) phase=2;
        }
        g_diag_draw_cut=phase==1; g_diag_prep_cut=phase==2;
        if (phase!=budget_phase) {
            fprintf(stderr,"[budget] ms=%llu swap=%u phase=%d stage=%u room=%u (0=normal,1=gfx omitted,2=prep+gfx omitted)\n",
                    (unsigned long long)sample_now,frames,phase,stage,room);
            budget_phase=phase;
            /* Phase zero also occurs between the two omission windows.
               Give automation a unique marker for the final restoration. */
            if (!phase && sample_now>=190000u && stage==33 && room==210)
                fprintf(stderr,"[budget] final restoration after both omission windows\n");
        }
    }
    if (!sample_start) sample_start=sample_now;
    sample_frames++;
    if (sample_now-sample_start>=10000u) {
        fprintf(stderr,"[diag] Vita present rate %u swaps in %llu ms\n",sample_frames,
                (unsigned long long)(sample_now-sample_start));
        if (getenv("SH3_DIAG_ZONE_TRACE") && g_mem && g_mem_size>0x070e66e0u) {
            uint32_t room=0; memcpy(&room,g_mem+0x00897e48u,sizeof room);
            /* Read-only snapshot: byte semantics other than stage remain
               unqualified, so retain raw bytes instead of inventing names. */
            const uint8_t *s=g_mem+0x070e66d8u;
            fprintf(stderr,"[zone] ms=%llu swap=%u stage=%u room_ordinal=%u raw=%02x,%02x,%02x,%02x,%02x,%02x,%02x,%02x\n",
                    (unsigned long long)sample_now,frames,s[1],room,
                    s[0],s[1],s[2],s[3],s[4],s[5],s[6],s[7]);
        }
        fflush(stderr);
        sample_frames=0; sample_start=sample_now;
    }
    const char *trigger="ux0:data/sh3/diag/shotnow";
    FILE *f=frames%60u==0 ? fopen(trigger,"rb") : NULL;
    /* Check the late trigger on every swap: gameplay may render well below
       60 frames after a long loading phase on Vita. */
    if (!f && !late_checked && SDL_GetTicks64()-first_swap>=late_delay) {
        late_checked=1; trigger="ux0:data/sh3/diag/shotlate"; f=fopen(trigger,"rb");
    }
    /* A trigger is not the output path: recreating shotnow with the TGA made
       the supposedly one-shot request run again every 60 swaps. Vita-Shot
       also expects the documented shot.tga destination for both markers. */
    if (f) { fclose(f); unlink(trigger); plat_shot_raw(NULL); fflush(stderr); }
    { char tmp[256]; if(bench_take_shot(tmp)) {bench_shot_half(tmp);fflush(stderr);} }
    unsigned pending=__atomic_exchange_n(&capture_requests,0,__ATOMIC_RELAXED);
    if (pending&2u) plat_shot_raw("move-before");
    if (pending&4u) plat_shot_raw("move-after");
    vglSwapBuffers(GL_FALSE);
}
void plat_drawable_size(int *w,int *h) { *w=960; *h=544; }
