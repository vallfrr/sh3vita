/* SH3-only experimental decoder. AvPlayer memory setup follows the existing
 * vitaGL samples/video_playback sample (VitaSDK public API). No GL operations
 * here: CPU copies the hardware frame into the existing guest-renderer path.
 * Compiled and selected independently from the playable build. */
#include <psp2/avplayer.h>
#include <psp2/sysmodule.h>
#include <psp2/gxm.h>
#include <malloc.h>

static void *movie_hw_alloc(void *arg,uint32_t alignment,uint32_t size) {
    (void)arg;
    if(alignment<sizeof(void *))alignment=sizeof(void *);
    return memalign(alignment,size);
}
static void movie_hw_free(void *arg,void *p) {(void)arg;free(p);}
static void *movie_hw_alloc_frame(void *arg,uint32_t alignment,uint32_t size) {
    (void)arg;
    if(size>0x7fefffffu)return NULL;
    /* phycont (vitaGL sample recipe), then cdram (OpenFMV recipe: attr 4, align >= 256 KiB). A NULL here kills
       the player silently, so every failure is logged. */
    for(int k=0;k<2;k++) {
        uint32_t al=k?(alignment<0x40000u?0x40000u:alignment):0x100000u,sz=(size+al-1)&~(al-1);
        SceKernelAllocMemBlockOpt opt={0};opt.size=sizeof opt;opt.attr=4;opt.alignment=al;
        SceUID id=k?sceKernelAllocMemBlock("sh3-av-frame",SCE_KERNEL_MEMBLOCK_TYPE_USER_CDRAM_RW,sz,&opt)
                   :sceKernelAllocMemBlock("sh3-av-frame",SCE_KERNEL_MEMBLOCK_TYPE_USER_MAIN_PHYCONT_NC_RW,sz,NULL);
        void *p=NULL;int r=id;
        if(id>=0 && ((r=sceKernelGetMemBlockBase(id,&p))<0 || (r=sceGxmMapMemory(p,sz,SCE_GXM_MEMORY_ATTRIB_RW))<0)) {
            sceKernelFreeMemBlock(id);p=NULL;
        }
        rt_log("[movie-hw] frame alloc %u B align %u %s -> %08x %p\n",sz,alignment,k?"cdram":"phycont",(unsigned)r,p);
        if(p)return p;
    }
    return NULL;
}
static void movie_hw_event(void *arg,int32_t id,int32_t src,void *data) {
    (void)arg;(void)data;rt_log("[movie-hw] event id=%d src=%d\n",id,src);
}
static void movie_hw_free_frame(void *arg,void *p) {
    (void)arg;
    if(!p)return;
    /* AvPlayer owns these buffers; no queued GL upload ever references them.
       The output FIFO owns separate copies, so glFinish is neither needed
       nor safe from this decoder callback. */
    SceUID id=sceKernelFindMemBlockByAddr(p,0);
    sceGxmUnmapMemory(p);
    if(id>=0)sceKernelFreeMemBlock(id);
}
/* Pause/Resume before AvPlayer has created its stream return 806a0002
   (same code as the early SetLooping); only that state code is tolerated. */
static void movie_hw_ctl(int r,const char *what) {
    if(r>=0)return;
    if((unsigned)r==0x806a0002u){rt_log("[movie-hw] %s before stream ready (806a0002), ignored\n",what);return;}
    rt_fatal("[movie-hw] %s failed %08x",what,(unsigned)r);
}
/* Direct display (marker moviehwtex): the guest renderer path (FIFO -> Receive -> game copy into its 512x512
   texture) caps delivery at ~13/s (v5). Instead the NV12 frame is kept host-side and the game's own film quad
   (DrawPrimitiveUP caller 004046c4, texture 0072c080) samples a vitaGL YUV texture of the same padded size, so
   the game's UVs, draw order and every later overlay (fades, subtitles) are unchanged. The guest renderer
   receives no samples. Decoder thread stages, prep consumer uploads through the GL queue, render thread frees. */
#include "d3d8/qgl.h"
#define HWQ 3
static int s_hw_tex_on,s_hw_pw,s_hw_ph;
static Texture s_hw_tex;
static uint8_t *s_hw_nv[HWQ];
static volatile int s_hw_busy[HWQ];        /* 1 while queued for upload (render thread clears it) */
static unsigned s_hw_seq[HWQ],s_hw_seqn,s_hw_shown;
static int s_hw_writing=-1;
static SDL_mutex *s_hw_mx;
static SDL_sem *s_hw_frame_sem;            /* posted per staged frame: paces the film loop's 100 ms wait */
static unsigned s_hw_quads,s_hw_uploads,s_hw_dropped;
static unsigned po2(unsigned v){unsigned p=1;while(p<v)p<<=1;return p;}
static void movie_hw_tex_begin(Movie *m) {
    s_hw_pw=po2(m->w);s_hw_ph=po2(m->h);
    size_t yz=(size_t)s_hw_pw*s_hw_ph,sz=yz*3/2;
    if(!s_hw_mx && !(s_hw_mx=SDL_CreateMutex()))rt_fatal("[movie-hw] tex mutex");
    if(!s_hw_frame_sem && !(s_hw_frame_sem=SDL_CreateSemaphore(0)))rt_fatal("[movie-hw] frame semaphore");
    while(!SDL_SemTryWait(s_hw_frame_sem));
    for(int i=0;i<HWQ;i++) {
        while(s_hw_busy[i])SDL_Delay(1);   /* an upload of the previous film may still be queued */
        free(s_hw_nv[i]);
        if(!(s_hw_nv[i]=malloc(sz)))rt_fatal("[movie-hw] tex staging %u B",(unsigned)sz);
        memset(s_hw_nv[i],16,yz);memset(s_hw_nv[i]+yz,128,sz-yz);   /* video black in the padding */
        s_hw_seq[i]=0;
    }
    s_hw_seqn=s_hw_shown=0;s_hw_writing=-1;s_hw_quads=s_hw_uploads=s_hw_dropped=0;
    s_hw_tex.com.kind=K_TEXTURE;s_hw_tex.com.refs=1;s_hw_tex.w=s_hw_pw;s_hw_tex.h=s_hw_ph;
    s_hw_tex.levels=1;s_hw_tex.fmt=FMT_A8R8G8B8;s_hw_tex.pool=1;
    s_hw_tex_on=1;
    rt_log("[movie-hw] direct YUV texture %dx%d for film %dx%d\n",s_hw_pw,s_hw_ph,m->w,m->h);
}
/* decoder thread: copy the NV12 frame (rows of w bytes) into a free slot; oldest unshown frame is replaced
   when the consumer is behind (counted as dropped). */
static void movie_hw_stage(const uint8_t *src,unsigned w,unsigned h) {
    int k=-1;unsigned best=~0u;
    SDL_LockMutex(s_hw_mx);
    for(int i=0;i<HWQ;i++)if(!s_hw_busy[i] && s_hw_seq[i]<=s_hw_shown){k=i;break;}   /* free or already shown */
    if(k<0)for(int i=0;i<HWQ;i++)if(!s_hw_busy[i] && s_hw_seq[i]<best){best=s_hw_seq[i];k=i;}
    if(k>=0 && s_hw_seq[k]>s_hw_shown)s_hw_dropped++;
    if(k>=0){s_hw_seq[k]=0;s_hw_writing=k;}
    SDL_UnlockMutex(s_hw_mx);
    if(k<0){s_hw_dropped++;return;}
    uint8_t *d=s_hw_nv[k];size_t yz=(size_t)s_hw_pw*s_hw_ph;
    /* The game's texture holds the film bottom-up (DIB order; quad UV v 0..0.748, v7 showed it upside down):
       store rows in the same order. */
    for(unsigned y=0;y<h;y++)memcpy(d+(size_t)(h-1-y)*s_hw_pw,src+(size_t)y*w,w);
    for(unsigned y=0;y<h/2;y++)memcpy(d+yz+(size_t)(h/2-1-y)*s_hw_pw,src+(size_t)w*h+(size_t)y*w,w);
    SDL_LockMutex(s_hw_mx);s_hw_seq[k]=++s_hw_seqn;s_hw_writing=-1;SDL_UnlockMutex(s_hw_mx);
    SDL_SemPost(s_hw_frame_sem);
}
/* game thread, film loop wait (diag_runtime.c): block until a new frame is staged or the game's own timeout
   expires; 0 when direct mode is off (normal wait). */
int movie_hw_frame_wait(uint32_t ms) {
    if(!s_hw_tex_on)return 0;
    SDL_SemWaitTimeout(s_hw_frame_sem,ms);
    while(!SDL_SemTryWait(s_hw_frame_sem));   /* several frames since the last loop: show the newest once */
    return 1;
}
/* game thread, film quad: the texture to sample, or NULL (keep the game's) until a first frame exists */
Texture *movie_hw_film_tex(void) {
    if(!s_hw_tex_on || !s_hw_seqn)return NULL;
    s_hw_quads++;return &s_hw_tex;
}
/* prep consumer (tex_gl): queue the newest unshown frame once */
GLuint movie_hw_tex_gl(Texture *t) {
    if(!t->gl) {
        glGenTextures(1,&t->gl);glBindTexture(GL_TEXTURE_2D,t->gl);
        glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_LINEAR);glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_S,GL_CLAMP_TO_EDGE);glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_T,GL_CLAMP_TO_EDGE);
    }
    int k=-1;
    SDL_LockMutex(s_hw_mx);
    for(int i=0;i<HWQ;i++)if(i!=s_hw_writing && s_hw_seq[i]>s_hw_shown && (k<0 || s_hw_seq[i]>s_hw_seq[k]))k=i;
    if(k>=0){s_hw_shown=s_hw_seq[k];s_hw_busy[k]=1;}
    SDL_UnlockMutex(s_hw_mx);
    if(k>=0) {
        glBindTexture(GL_TEXTURE_2D,t->gl);
        /* BT.601 (GXM CSC0): MPEG-1 source values are copied unchanged into the MP4, and the original DirectShow
           path and this port's software converter both use BT.601 (v8 with BT.709 measured browner). */
        q_tex_yuv(s_hw_pw,s_hw_ph,s_hw_nv[k],(size_t)s_hw_pw*s_hw_ph*3/2,&s_hw_busy[k],VGL_YUV420P_NV12_BT601);
        if(!s_hw_uploads++)rt_log("[movie-hw] first YUV upload gl=%u slot=%d seq=%u\n",t->gl,k,s_hw_shown);
    }
    return t->gl;
}
int movie_hw_is_tex(const Texture *t){return t==&s_hw_tex;}
static void movie_hw_open(Movie *m) {
    static int loaded;
    if(!m->hw_path[0])rt_fatal("[movie-hw] missing sidecar path");
    FILE *probe=fopen(m->hw_path,"rb");
    if(!probe)rt_fatal("[movie-hw] sidecar missing: %s",m->hw_path);
    fclose(probe);
    if(!loaded) {
        int r=sceSysmoduleLoadModule(SCE_SYSMODULE_AVPLAYER);
        if(r<0)rt_fatal("[movie-hw] module load failed %08x",r);
        loaded=1;
    }
    SceAvPlayerInitData init={0};
    init.memoryReplacement.allocate=movie_hw_alloc;
    init.memoryReplacement.deallocate=movie_hw_free;
    init.memoryReplacement.allocateTexture=movie_hw_alloc_frame;
    init.memoryReplacement.deallocateTexture=movie_hw_free_frame;
    init.eventReplacement.eventCallback=movie_hw_event;
    {SceKernelFreeMemorySizeInfo fi={.size=sizeof fi};sceKernelGetFreeMemorySize(&fi);
     struct mallinfo mi=mallinfo();
     rt_log("[movie-hw] free before init user=%d KB cdram=%d KB phycont=%d KB heap_inuse=%d KB vram=%u KB\n",
            fi.size_user>>10,fi.size_cdram>>10,fi.size_phycont>>10,mi.uordblks>>10,(unsigned)(vglMemFree(VGL_MEM_VRAM)>>10));}
    init.basePriority=0xa0;init.numOutputVideoFrameBuffers=4;init.autoStart=1;
    m->hw_handle=sceAvPlayerInit(&init);
    /* The handle is a heap pointer (0x8xxxxxxx on Vita): negative as int32 yet valid.
       Only the AvPlayer error range 0x806Axxxx means failure (observed handle 82bb9a60). */
    if(!m->hw_handle || ((unsigned)m->hw_handle&0xFFFF0000u)==0x806A0000u)
        rt_fatal("[movie-hw] init failed %08x",(unsigned)m->hw_handle);
    m->hw_planar=malloc((size_t)m->w*m->h*3/2);
    m->hw_audio=malloc(8192*sizeof(float));
    if(!m->hw_planar || !m->hw_audio)rt_fatal("[movie-hw] staging allocation failed");
    /* No SetLooping: before the stream exists it returns 806a0002 (observed v3);
       looping is off by default (vitaGL sample enables it explicitly after AddSource). */
    int r=sceAvPlayerAddSource(m->hw_handle,m->hw_path);
    if(r<0)rt_fatal("[movie-hw] AddSource failed %08x",r);
    m->hw_active=1;
    if(getenv("SH3_DIAG_MOVIE_HW_TEX"))movie_hw_tex_begin(m);
    rt_log("[movie-hw] source=%s handle=%08x %dx%d buffers=4\n",m->hw_path,(unsigned)m->hw_handle,m->w,m->h);
}
static int movie_hw_video_thread(void *arg) {
    Movie *m=arg;
    sceKernelChangeThreadCpuAffinityMask(sceKernelGetThreadId(),SCE_KERNEL_CPU_MASK_USER_1);
    uint64_t start=SDL_GetTicks64(),last=start;
    unsigned decoded=0,previous=0,first=0;
    while(!m->stop) {
        if(m->state!=2) {SDL_Delay(2);continue;}
        SceAvPlayerFrameInfo info={0};
        if(sceAvPlayerGetVideoData(m->hw_handle,&info)) {
            unsigned w=info.details.video.width,h=info.details.video.height;
            if(!info.pData || w!=(unsigned)m->w || h!=(unsigned)m->h || (w&15) || (h&15))
                rt_fatal("[movie-hw] unsupported hardware frame %ux%u, expected %dx%d",w,h,m->w,m->h);
            if(s_hw_tex_on) {movie_hw_stage(info.pData,w,h);decoded++;first=1;}
            else {
            size_t ys=(size_t)w*h,cs=ys/4;
            memcpy(m->hw_planar,info.pData,ys);
            /* AvPlayer's YVU420P2 frame: interleaved chroma follows Y.
               Validate channel ordering visually against the source. */
            for(size_t i=0;i<cs;i++) {
                m->hw_planar[ys+i]=info.pData[ys+2*i];
                m->hw_planar[ys+cs+i]=info.pData[ys+2*i+1];
            }
            plm_frame_t f={0};f.width=w;f.height=h;f.time=info.timeStamp/1000.0;
            f.y=(plm_plane_t){w,h,m->hw_planar};
            f.cb=(plm_plane_t){w/2,h/2,m->hw_planar+ys};
            f.cr=(plm_plane_t){w/2,h/2,m->hw_planar+ys+cs};
            hand_frame(m,&f);decoded++;first=1;
            }
            uint64_t now=SDL_GetTicks64();
            if(now-last>=2000) {
                rt_log("[movie-hw] decoded=%.2f/s total=%u pts=%.3f wall=%.3f delivered=%d tex: quads=%u uploads=%u dropped=%u\n",
                       (decoded-previous)*1000.0/(now-last),decoded,info.timeStamp/1000.0,(now-start)/1000.0,m->st_npush,
                       s_hw_quads,s_hw_uploads,s_hw_dropped);
                last=now;previous=decoded;
            }
        } else {
            if(first && !sceAvPlayerIsActive(m->hw_handle))break;
            if(!first && SDL_GetTicks64()-start>15000)rt_fatal("[movie-hw] no video frame after 15s");
            SDL_Delay(1);
        }
    }
    rt_log("[movie-hw] video end decoded=%u stop=%d elapsed_ms=%llu\n",decoded,m->stop,(unsigned long long)(SDL_GetTicks64()-start));
    m->dec_done=1;SDL_SemPost(m->osem);return 0;
}
static int movie_hw_audio_thread(void *arg) {
    Movie *m=arg;
    sceKernelChangeThreadCpuAffinityMask(sceKernelGetThreadId(),SCE_KERNEL_CPU_MASK_USER_2);
    const unsigned cap=sizeof g_ring/sizeof g_ring[0];
    unsigned blocks=0;
    while(!m->stop && !m->ended) {
        SDL_mutex *lock=movie_ring_mutex();SDL_LockMutex(lock);
        unsigned queued=(g_ring_w-g_ring_r+cap)%cap;SDL_UnlockMutex(lock);
        SceAvPlayerFrameInfo info={0};
        if(m->state==2 && queued<48000*2/4 && sceAvPlayerGetAudioData(m->hw_handle,&info)) {
            unsigned channels=info.details.audio.channelCount,rate=info.details.audio.sampleRate;
            unsigned bytes=info.details.audio.size;
            if(!info.pData || (channels!=1 && channels!=2) || rate<8000 || rate>48000 ||
               bytes%(2*channels) || bytes/(2*channels)>4096)
                rt_fatal("[movie-hw] unsupported PCM channels=%u rate=%u bytes=%u",channels,rate,bytes);
            unsigned n=bytes/(2*channels);const int16_t *pcm=(const int16_t *)info.pData;
            for(unsigned i=0;i<n;i++) {
                m->hw_audio[2*i]=pcm[channels*i]/32768.0f;
                m->hw_audio[2*i+1]=pcm[channels*i+channels-1]/32768.0f;
            }
            movie_audio_push(m->hw_audio,n,rate);blocks++;
            if(blocks==1)rt_log("[movie-hw] audio PCM channels=%u rate=%u frames=%u\n",channels,rate,n);
        } else SDL_Delay(2);
    }
    rt_log("[movie-hw] audio end blocks=%u\n",blocks);return 0;
}
static void movie_hw_close(Movie *m) {
    if(!m->hw_active)return;
    /* Every application decoder/audio/output thread has already joined. */
    int stop=sceAvPlayerStop(m->hw_handle),close=sceAvPlayerClose(m->hw_handle);
    rt_log("[movie-hw] close stop=%08x close=%08x\n",stop,close);
    if(close<0)rt_fatal("[movie-hw] close failed %08x",close);
    s_hw_tex_on=0;
    m->hw_active=0;free(m->hw_planar);m->hw_planar=NULL;
    free(m->hw_audio);m->hw_audio=NULL;
    /* Leak check per film: must match "free before init" (AvPlayer heap + frame blocks returned). */
    {SceKernelFreeMemorySizeInfo fi={.size=sizeof fi};sceKernelGetFreeMemorySize(&fi);
     struct mallinfo mi=mallinfo();
     rt_log("[movie-hw] free after close user=%d KB cdram=%d KB phycont=%d KB heap_inuse=%d KB vram=%u KB\n",
            fi.size_user>>10,fi.size_cdram>>10,fi.size_phycont>>10,mi.uordblks>>10,(unsigned)(vglMemFree(VGL_MEM_VRAM)>>10));}
}
