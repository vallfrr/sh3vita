/* Experimental macOS SDL platform for the isolated SH4 D3D backend.
 * No Vita build; no callbacks or game-specific SH4 options enabled here. */
#include "rt.h"
#include <SDL.h>
#include <SDL_opengl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static SDL_Window *window;
static SDL_GLContext context;
static unsigned capture_requested;
static char bench_shot_path[256];
void diag_bench_shot(const char *name) { snprintf(bench_shot_path,sizeof bench_shot_path,"%s",name); }
void diag_capture_request(unsigned which) { if (which==1 || which==2) capture_requested=which; }
uint64_t g_t_sleep,g_t_gl,g_t_swap,g_t_io,g_t_wait,g_io_bytes,g_heap_used,g_heap_peak;
unsigned g_cursor_seen;
int g_audit_enabled,g_audit_frames,g_audit_checks,g_vita_bindings_open;
#ifndef SH3_MOVIE_LAB
int g_movie_w,g_movie_h;
#endif
void heap_report(void) {}
#ifndef SH3_MOVIE_LAB
void movie_audio_mix(float *samples,int n) { (void)samples; (void)n; /* movie audio not connected */ }
#endif
int rt_cpu_profile_thread(const char *label) { (void)label; return 0; }
void rt_audit_frame(unsigned f) { (void)f; }
int rt_audit_capture(unsigned f,char *p,size_t n) { (void)f;(void)p;(void)n;return 0; }
int diag_window_create(void) {
    if (window) return 1;
    if (SDL_InitSubSystem(SDL_INIT_VIDEO|SDL_INIT_TIMER|SDL_INIT_GAMECONTROLLER)) { fprintf(stderr,"[diag] SDL init: %s\n",SDL_GetError()); return 0; }
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK,SDL_GL_CONTEXT_PROFILE_COMPATIBILITY);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION,2);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION,1);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER,1);
    int wide_preview=getenv("SH3_DIAG_WIDE_PREVIEW")!=NULL || getenv("SH3_DIAG_WIDE_NATIVE")!=NULL;
    window=SDL_CreateWindow("Silent Hill 3 - diagnostic",SDL_WINDOWPOS_CENTERED,SDL_WINDOWPOS_CENTERED,
                            wide_preview?960:640,wide_preview?544:480,SDL_WINDOW_OPENGL|SDL_WINDOW_HIDDEN|SDL_WINDOW_RESIZABLE);
    if (!window) { fprintf(stderr,"[diag] SDL window: %s\n",SDL_GetError()); return 0; }
    fprintf(stderr,"[diag] SDL window created, initially hidden\n"); return 1;
}
int diag_window_show(int show) {
    if (!window) rt_fatal("[diag] ShowWindow without SDL window");
    int was_visible=!!(SDL_GetWindowFlags(window)&SDL_WINDOW_SHOWN);
    if (show && !getenv("SH3_DIAG_HIDDEN")) SDL_ShowWindow(window); else SDL_HideWindow(window);
    fprintf(stderr,"[diag] SDL window %s\n",show?"shown":"hidden");
    return was_visible;
}
void diag_window_ungrab(void) { if (!window) rt_fatal("[diag] ClipCursor without window"); SDL_SetWindowGrab(window,SDL_FALSE); }
int diag_window_focused(void) {
    /* Reproducible unattended input tests when another Mac app owns focus. */
    if (getenv("SH3_DIAG_FORCE_FOCUS")) return window!=NULL;
    return window && SDL_GetKeyboardFocus()==window;
}
int diag_display_mode(uint32_t index,uint32_t *w,uint32_t *h,uint32_t *bits,uint32_t *hz) {
    /* The diagnostic fixed-size framebuffer is independent of desktop modes. */
    if (getenv("SH3_DIAG_WIDE_NATIVE")) {
        extern uint32_t g_diag_res_w,g_diag_res_h;
        if(index!=0 && index!=UINT32_MAX)return 0;
        *w=g_diag_res_w;*h=g_diag_res_h;*bits=32;*hz=60;return 1;
    }
    if (!window) return 0;
    int display=SDL_GetWindowDisplayIndex(window); SDL_DisplayMode mode;
    if (display<0) return 0;
    int r=index==UINT32_MAX?SDL_GetCurrentDisplayMode(display,&mode):
          index<=INT32_MAX?SDL_GetDisplayMode(display,(int)index,&mode):-1;
    if (r) return 0;
    *w=(uint32_t)mode.w; *h=(uint32_t)mode.h; *bits=SDL_BITSPERPIXEL(mode.format); *hz=(uint32_t)mode.refresh_rate;
    return 1;
}
int diag_display_mode_supported(uint32_t w,uint32_t h,uint32_t bits,uint32_t hz) {
    for (uint32_t i=0;i<4096;i++) {
        uint32_t mw,mh,mb,mhz;
        if (!diag_display_mode(i,&mw,&mh,&mb,&mhz)) break;
        if (w==mw && h==mh && bits==mb && (hz<=1 || hz==mhz)) return 1;
    }
    return 0;
}
void plat_gl_init(void) {
    if (!window && !diag_window_create()) rt_fatal("[diag] cannot initialize window");
    context=SDL_GL_CreateContext(window);
    if (!context) rt_fatal("[diag] OpenGL context: %s",SDL_GetError());
    SDL_GL_SetSwapInterval(0);
}
#ifdef SH3_MOVIE_LAB
/* Capture the current backbuffer immediately, before later game rendering. */
void diag_movie_capture_draw(unsigned index) {
    int w=0,h=0;
    if (!window || !context) return;
    SDL_GL_GetDrawableSize(window,&w,&h);
    if (w<=0 || h<=0 || w>4096 || h>4096) return;
    GLint viewport[4],scissor[4];
    glGetIntegerv(GL_VIEWPORT,viewport); glGetIntegerv(GL_SCISSOR_BOX,scissor);
    fprintf(stderr,"[movie-lab] capture %u drawable=%dx%d viewport=%d,%d,%d,%d scissor=%d,%d,%d,%d enabled=%d\n",
            index,w,h,viewport[0],viewport[1],viewport[2],viewport[3],scissor[0],scissor[1],scissor[2],scissor[3],glIsEnabled(GL_SCISSOR_TEST));
    size_t row=(size_t)w*3, bytes=row*(size_t)h;
    unsigned char *pixels=malloc(bytes);
    if (!pixels) return;
    GLint alignment; glGetIntegerv(GL_PACK_ALIGNMENT,&alignment);
    glPixelStorei(GL_PACK_ALIGNMENT,1);
    glReadPixels(0,0,w,h,GL_RGB,GL_UNSIGNED_BYTE,pixels);
    glPixelStorei(GL_PACK_ALIGNMENT,alignment);
    char path[100];
    if (index) snprintf(path,sizeof path,"build/pc-movie-lab/movie-draw-%u.ppm",index);
    else snprintf(path,sizeof path,"build/pc-movie-lab/movie-draw-immediate.ppm");
    FILE *f=fopen(path,"wb");
    if (f) {
        fprintf(f,"P6\n%d %d\n255\n",w,h);
        for (int y=h-1;y>=0;y--) fwrite(pixels+(size_t)y*row,1,row,f);
        fclose(f);
        fprintf(stderr,"[movie-lab] immediate draw backbuffer captured\n");
    }
    free(pixels);
}
#endif
void plat_gl_swap(void) {
    if (!window || !context) rt_fatal("[diag] no OpenGL context");
    static unsigned frames;
    if (getenv("SH3_DIAG_CAPTURE") && (++frames%60u==0 || capture_requested)) {
        int w=0,h=0; SDL_GL_GetDrawableSize(window,&w,&h);
        if (w>0 && h>0 && w<=4096 && h<=4096) {
            size_t row=(size_t)w*3, bytes=row*(size_t)h; unsigned char *pixels=malloc(bytes);
            if (pixels) {
                GLint alignment; glGetIntegerv(GL_PACK_ALIGNMENT,&alignment);
                glPixelStorei(GL_PACK_ALIGNMENT,1); glReadPixels(0,0,w,h,GL_RGB,GL_UNSIGNED_BYTE,pixels); glPixelStorei(GL_PACK_ALIGNMENT,alignment);
                char name[96];
                if (capture_requested) snprintf(name,sizeof name,"build/pc-diag/move-%s.ppm",capture_requested==1?"before":"after");
                else if (frames<=600) snprintf(name,sizeof name,"build/pc-diag/frame-%05u.ppm",frames);
                else snprintf(name,sizeof name,"build/pc-diag/frame-live.ppm");
                FILE *f=fopen(name,"wb");
                if (f) { fprintf(f,"P6\n%d %d\n255\n",w,h); for (int y=h-1;y>=0;y--) fwrite(pixels+(size_t)y*row,1,row,f); fclose(f); }
                if (capture_requested==2 && getenv("SH3_DIAG_TRACE_AFTER_CAPTURE")) {
                    extern void diag_trace_next_frames(void) __attribute__((weak));
                    if (diag_trace_next_frames) diag_trace_next_frames();
                }
                capture_requested=0;
                free(pixels);
            }
        }
    }
#ifdef _WIN32
    if (bench_shot_path[0]) {
        int w=0,h=0; SDL_GL_GetDrawableSize(window,&w,&h);
        if (w>0 && h>0 && w<=4096 && h<=4096) {
            size_t row=(size_t)w*3; unsigned char *px=malloc(row*(size_t)h);
            if (px) {
                char path[300]; snprintf(path,sizeof path,"%s.ppm",bench_shot_path);
                glPixelStorei(GL_PACK_ALIGNMENT,1); glReadPixels(0,0,w,h,GL_RGB,GL_UNSIGNED_BYTE,px);
                FILE *f=fopen(path,"wb");
                if (f) { fprintf(f,"P6%c%d %d%c255%c",10,w,h,10,10); for (int y=h-1;y>=0;y--) fwrite(px+(size_t)y*row,1,row,f); fclose(f); }
                fprintf(stderr,"[bench] shot %s %dx%d %s\n",path,w,h,f?"saved":"FAILED"); fflush(stderr); free(px);
            }
        }
        bench_shot_path[0]=0;
    }
    { /* Windows bench: one-shot capture + exit for unattended zone sweeps. SH3_DIAG_SHOT_AT_MS=<ms after first swap>, SH3_DIAG_SHOT_FILE=<path.ppm> */
        static Uint64 first; static int done; const char *at=getenv("SH3_DIAG_SHOT_AT_MS"), *file=getenv("SH3_DIAG_SHOT_FILE");
        if (!first) first=SDL_GetTicks64();
        if (at && file && !done && SDL_GetTicks64()-first>=(Uint64)atoll(at)) {
            int w=0,h=0; SDL_GL_GetDrawableSize(window,&w,&h);
            if (w>0 && h>0 && w<=4096 && h<=4096) {
                size_t row=(size_t)w*3; unsigned char *px=malloc(row*(size_t)h);
                if (px) {
                    glPixelStorei(GL_PACK_ALIGNMENT,1); glReadPixels(0,0,w,h,GL_RGB,GL_UNSIGNED_BYTE,px);
                    FILE *f=fopen(file,"wb");
                    if (f) { fprintf(f,"P6%c%d %d%c255%c",10,w,h,10,10); for (int y=h-1;y>=0;y--) fwrite(px+(size_t)y*row,1,row,f); fclose(f); }
                    fprintf(stderr,"[diag] win screenshot saved: %s %dx%d\n",file,w,h); fflush(stderr);
                    free(px);
                }
            }
            done=1; if (getenv("SH3_DIAG_EXIT_AFTER_SHOT")) { fflush(stderr); _exit(0); }
        }
    }
#endif
    SDL_GL_SwapWindow(window);
}
void plat_drawable_size(int *w,int *h) { if (!window) rt_fatal("[diag] no window"); SDL_GL_GetDrawableSize(window,w,h); }
