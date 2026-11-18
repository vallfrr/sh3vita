// Cutscenes: the game builds a DirectShow graph (source -> MPEG-1 splitter -> codecs ->
// renderer). The DirectShow base classes and the render-to-texture code are game code, already
// recompiled; the host provides everything upstream: graph, filters, pins and decoder
// (pl_mpeg). Frames are delivered to the real guest renderer filter through the standard protocol
// (ReceiveConnection / GetAllocator / GetBuffer / Receive), audio goes to the host mixer.
#include "d3d8/d3d8.h"
#include "win32/win32.h"
#include <SDL.h>
#include <stdatomic.h>
#include <stdlib.h>
#ifdef __vita__
#include <psp2/kernel/threadmgr.h>
#include <psp2/kernel/cpu.h>
#include <psp2/kernel/sysmem.h>
#endif

#include <stdio.h>
// [movie] time spent in movie file reads (decode thread stats)
static double g_st_io;
static int g_movie_all_frames;
static unsigned s_movie_serial;
#define PLM_VIDEO_SKIP_B_ALLOWED() (!g_movie_all_frames)
static size_t movie_fread(void *p, size_t s, size_t n, FILE *f) {
    uint64_t t = SDL_GetPerformanceCounter();
    size_t r = fread(p, s, n, f);
    g_st_io += (double)(SDL_GetPerformanceCounter() - t) * 1000.0 / (double)SDL_GetPerformanceFrequency();
    return r;
}
#define fread movie_fread
#define PL_MPEG_IMPLEMENTATION
#include "third_party/pl_mpeg/pl_mpeg.h"
#undef fread
#include "movie_stream_cache.h"
#ifdef SH3_MOVIE_LAB
#include "diag_filter_refs.h"
#endif

#define M(p, name) static void p##_##name(CPU *c)
enum { MK_GRAPH = 100, MK_CTL, MK_POS, MK_EV, MK_AUD, MK_SINK, MK_FILTER, MK_PIN, MK_ENUM, MK_ALLOC, MK_SAMP };
#define S_FALSE_ 1u
#define E_NOINTERFACE_ 0x80004002u
#define E_NOTIMPL_ 0x80004001u
#define VFW_E_TIMEOUT 0x80040227u
#define VFW_E_NOT_FOUND 0x80040216u

typedef struct Movie Movie;
typedef struct { ComObj com; Movie *m; int role; int dir; const char *id; uint32_t peer; } HObj;
typedef struct { ComObj com; Movie *m; uint32_t buf, size, len; int64_t t0, t1; _Atomic int sync; int busy, have_time; } Samp;
#ifdef SH3_MOVIE_LAB
/* The output thread owns one sample reference during its synchronous Receive. */
static __thread Samp *movie_active_sample;
static __thread CPU *movie_active_sample_cpu;
static __thread Movie *movie_active_eos;
static __thread CPU *movie_active_eos_cpu;
int movie_lab_receive_active(CPU *cpu) { return movie_active_sample && movie_active_sample_cpu == cpu; }
#endif
#define NSAMP 3
enum { F_SRC, F_SPLIT, F_ACODEC, F_VCODEC, F_DSR };

struct Movie {
    plm_t *plm;                                   // video only ("movie" thread, core 1)
    plm_demux_t *adm;                             // audio: second demuxer on the same file ("movie_audio" thread, core 2)
    plm_audio_t *aud;
    char path[600];
    uint32_t graph_g, ctl_g, pos_g, ev_g, aud_g, sink_g;
    HObj *graph, *filters[5], *pins[10];
    uint32_t renderer, rpin, mem_in, alloc;      // guest objects (alloc: host allocator)
    Samp *samp[NSAMP];
    uint32_t buf_size;
    SDL_Thread *thread, *athread, *othread;      // video decoding (core 1), audio and output (core 2)
    volatile int run;
    _Atomic int stop, ended, dec_done;
    // Output: decoding copies the YUV frame into yuv[otail % OQ]; the output thread converts and calls Receive.
    SDL_sem *osem;
#define OQ 5                                  // decoded frames waiting for the output thread (FIFO: a burst of decoded frames is not dropped)
    uint8_t *yuv[OQ];
    plm_frame_t of[OQ];
    unsigned ohead, otail;                        // frames read / written so far (slot = n % OQ)
    int obusy;
    int oq[OQ];                                 // queued slot IDs; never recycle the consumer's slot
    unsigned odropped;
    MovieStream *stream;
    MovieCursor cursor[2];
    _Atomic int state;                            // 0 stopped, 1 paused, 2 playing
    int w, h;
    SDL_mutex *lock;
    struct { uint32_t code, p1, p2; } ev[16];
    int nev;
#ifdef SH3_MOVIE_LAB
    uint32_t notify_hwnd, notify_message, notify_instance;
    DiagFilterRefs retained_filters;
#endif
    int vol;                                      // hundredths of a dB (0 = full, -10000 = mute)
    CPU *dec_cpu;
#if defined(__vita__) && defined(SH3_MOVIE_HW_LAB)
    int hw_active, hw_handle;
    double hw_duration;                           // .mp4 only (no .mpg opened): duration from movie_mp4_meta
    char hw_path[600];
    uint8_t *hw_planar;
    float *hw_audio;
#endif
    double st_dec, st_conv, st_recv;              // [movie] stats per 2 s window (cumulative ms)
    int st_ndec, st_npush, st_nskip;
};
static Movie *g_mv;
#if defined(__vita__) && defined(SH3_MOVIE_HW_LAB)
#define MOVIE_MP4_ONLY(m) (!(m)->plm && (m)->hw_path[0] && (m)->hw_duration > 0)   /* .mp4 played without a .mpg */
#define MOVIE_MP4_DURATION(m) (MOVIE_MP4_ONLY(m) ? (m)->hw_duration : 0)
#else
#define MOVIE_MP4_ONLY(m) 0
#define MOVIE_MP4_DURATION(m) 0
#endif
#if defined(__vita__) && defined(SH3_MOVIE_HW_LAB)
static void movie_hw_close(Movie *m);
#endif
int g_movie_w, g_movie_h;                         // size delivered to the game: identifies the movie texture (resource.c)
#ifdef SH3_MOVIE_LAB
void movie_lab_note_path(const char *guest) {
    if(!g_mv || !guest) return;
    size_t length=strlen(guest);
    if(length<4 || strcasecmp(guest+length-4,".000")) return;
    char host[sizeof g_mv->path];
    w32_host_path(guest,host,sizeof host);
    if(!host[0]) return;
    snprintf(g_mv->path,sizeof g_mv->path,"%s",host);
    rt_log("[movie-lab] prepared PC movie candidate: %s\n",g_mv->path);
}
#endif

// ---------- audio: ring buffer consumed by the dsound.c mixer ----------
static float g_ring[48000 * 2 * 4];               // 4 s stereo
static uint32_t g_ring_w, g_ring_r;               // protected by g_ring_lock
static _Atomic uint32_t g_movie_audio_played;      // diagnostic clock, stereo frames actually consumed
static _Atomic(SDL_mutex *) g_ring_lock;
static _Atomic int g_audio_gain_q8 = 256;

static SDL_mutex *movie_ring_mutex(void) {
    SDL_mutex *m=atomic_load(&g_ring_lock);
    if (!m) {
        SDL_mutex *created=SDL_CreateMutex();
        if (!created) rt_fatal("[movie] audio ring mutex allocation failed");
        SDL_mutex *expected=NULL;
        if (!atomic_compare_exchange_strong(&g_ring_lock,&expected,created)) {
            SDL_DestroyMutex(created); m=expected;
        } else m=created;
    }
    return m;
}

void movie_audio_push(const float *il, int frames, int rate) {
    SDL_mutex *lock=movie_ring_mutex();
    SDL_LockMutex(lock);
    const uint32_t cap = sizeof g_ring / sizeof g_ring[0];
    static double acc;
    double step = (double)rate / 48000.0, pos = acc;
    while (pos < frames) {
        uint32_t i = (uint32_t)pos;
        float t = (float)(pos - i);
        uint32_t j = i + 1 < (uint32_t)frames ? i + 1 : i;
        if (((g_ring_w + 2) % cap) == g_ring_r) break; // full: the surplus is dropped
        g_ring[g_ring_w] = il[2 * i] * (1 - t) + il[2 * j] * t;
        g_ring[(g_ring_w + 1) % cap] = il[2 * i + 1] * (1 - t) + il[2 * j + 1] * t;
        g_ring_w = (g_ring_w + 2) % cap;
        pos += step;
    }
    /* If the ring filled, the remaining input is deliberately discarded.
       A negative phase would be converted to a huge array index next time. */
    acc = pos < frames ? 0.0 : pos - frames;
    SDL_UnlockMutex(lock);
}
void movie_audio_flush(void) {
    SDL_mutex *lock=movie_ring_mutex();
    SDL_LockMutex(lock);
    g_ring_r = g_ring_w;
    g_movie_audio_played = 0;
    SDL_UnlockMutex(lock);
}
// Called by the mixer (n stereo frames): adds into acc[2n].
void movie_audio_mix(float *acc, int n) {
    SDL_mutex *lock=atomic_load(&g_ring_lock);
    if (!lock) return;
    const uint32_t cap = sizeof g_ring / sizeof g_ring[0];
    float g = g_audio_gain_q8 / 256.0f;
#if defined(SH3_MOVIE_LAB) && !defined(__vita__)
    /* Optional host-only proof of decoder output. File writes occur outside
       the ring mutex and are deliberately unavailable in the Vita build. */
    static FILE *capture; static int capture_init; static unsigned captured_total;
    float capture_block[2048]; unsigned captured_block=0;
    if (!capture_init) {
        const char *path=getenv("SH3_DIAG_MOVIE_AUDIO_CAPTURE");
        if (path && *path) { capture=fopen(path,"wbx"); if (!capture) rt_log("[movie-lab] audio capture open failed\n"); }
        capture_init=1;
    }
#endif
    SDL_LockMutex(lock);
    uint32_t before = g_ring_r;
    for (int i = 0; i < n && g_ring_r != g_ring_w; i++) {
        float left=g_ring[g_ring_r]*g, right=g_ring[(g_ring_r+1)%cap]*g;
        acc[2 * i] += left;
        acc[2 * i + 1] += right;
#if defined(SH3_MOVIE_LAB) && !defined(__vita__)
        if (capture && captured_block<1024 && captured_total+captured_block<480000) {
            capture_block[2*captured_block]=left; capture_block[2*captured_block+1]=right; captured_block++;
        }
#endif
        g_ring_r = (g_ring_r + 2) % cap;
    }
    g_movie_audio_played += ((g_ring_r + cap - before) % cap) / 2;
    SDL_UnlockMutex(lock);
#if defined(SH3_MOVIE_LAB) && !defined(__vita__)
    if (capture && captured_block) {
        if (fwrite(capture_block,sizeof(float)*2,captured_block,capture)!=captured_block) {
            rt_log("[movie-lab] audio capture write failed\n"); fclose(capture); capture=NULL;
        } else {
            captured_total+=captured_block;
            if (captured_total>=480000) { fclose(capture); capture=NULL; rt_log("[movie-lab] 10 s audio captured\n"); }
        }
    }
#endif
}

// ---------- calls into the guest code ----------
static uint32_t gcall(CPU *c, uint32_t obj, int slot, int nargs, ...) {
    HOSTFN;
    uint32_t args[10], fn = R32(R32(obj) + 4u * (uint32_t)slot);
#ifdef SH3_MOVIE_LAB
    if (getenv("SH3_DIAG_MOVIE_TRACE_FILTERS"))
        rt_log("[movie-lab] guest call object=%08x vtable=%08x slot=%d target=%08x\n",obj,R32(obj),slot,fn);
#endif
    va_list ap;
    va_start(ap, nargs);
    for (int i = 0; i < nargs; i++) args[i] = va_arg(ap, uint32_t);
    va_end(ap);
    switch (nargs) {
    case 0: return rt_guest_call(c, fn, 1, obj);
    case 1: return rt_guest_call(c, fn, 2, obj, args[0]);
    case 2: return rt_guest_call(c, fn, 3, obj, args[0], args[1]);
    case 3: return rt_guest_call(c, fn, 4, obj, args[0], args[1], args[2]);
    default: return rt_guest_call(c, fn, 5, obj, args[0], args[1], args[2], args[3]);
    }
}
static void put_guid(CPU *c, uint32_t p, uint32_t d1, uint16_t d2, uint16_t d3, const uint8_t d4[8]) {
    HOSTFN;
    W32(p, d1); W16(p + 4, d2); W16(p + 6, d3); memcpy(mb + p + 8, d4, 8);
}

// ---------- host COM object factory ----------
static uint32_t mkobj(Movie *m, int kind, const gfn *vt, int n, const char *iface, HObj **out) {
    HObj *o = calloc(1, sizeof *o);
    o->m = m;
    uint32_t g = com_wrap(&o->com, kind, vt, n, iface);
    if (out) *out = o;
    return g;
}
#define SELF HObj *self = (HObj *)com_get(ARG(1)); (void)self
M(any, QI) { STDRET(3, E_NOINTERFACE_); }
M(any, AddRef) { HOSTFN; ComObj *o = com_get(ARG(1)); STDRET(1, o ? (uint32_t)++o->refs : 1); }
M(any, Release) { HOSTFN; ComObj *o = com_get(ARG(1)); STDRET(1, o && o->refs > 1 ? (uint32_t)--o->refs : 1); }
static void rpush(CPU *c, uint32_t code, uint32_t p1, uint32_t p2) {
    (void)c;
    Movie *m = g_mv;
    if (!m) return;
    SDL_LockMutex(m->lock);
    int added=0;
    if (m->nev < 16) { m->ev[m->nev].code = code; m->ev[m->nev].p1 = p1; m->ev[m->nev].p2 = p2; m->nev++; added=1; }
#ifdef SH3_MOVIE_LAB
    /* Serialize notification posting with unregister: no post can begin
       after SetNotifyWindow(0) returns. Already queued messages remain. */
    if(added && m->notify_hwnd) {
        extern int diag_movie_post_message(uint32_t,uint32_t,uint32_t,uint32_t);
        if(diag_movie_post_message(m->notify_hwnd,m->notify_message,0,m->notify_instance))
            rt_log("[movie-lab] notification queue full or unavailable\n");
    }
#else
    (void)added;
#endif
    SDL_UnlockMutex(m->lock);
}

#ifdef SH3_MOVIE_LAB
static void sink_Notify(CPU *c);
/* The graph cannot be replaced before its output thread has joined. Bypass
   the dispatcher lock only for its synchronous EndOfStream notification. */
int movie_lab_try_eos_notify(gfn fn, CPU *c) {
    Movie *m=movie_active_eos;
    if (fn!=sink_Notify || !m || movie_active_eos_cpu!=c || g_mv!=m ||
        !g_mem || g_mem_size<20 || c->esp>g_mem_size-20) return 0;
    uint32_t args[4]; memcpy(args,g_mem+c->esp+4,sizeof args);
    if (args[0]!=m->sink_g) return 0;
    rpush(c,args[1],args[2],args[3]);
    c->eax=HR_OK; c->esp+=20;
    return 1;
}
#endif

// ---------- host pins (IPin) and enumerator ----------
M(pin, QueryDirection) { HOSTFN; SELF; W32(ARG(2), (uint32_t)self->dir); STDRET(2, HR_OK); }
M(pin, QueryId) {
    HOSTFN; SELF;
    W32(ARG(2), 0);
    size_t n = strlen(self->id) + 1;
    uint32_t w = gheap_alloc((uint32_t)n * 2);
    if (!w) STDRET(2, 0x8007000eu); // E_OUTOFMEMORY
    for (size_t i = 0; i < n; i++) W16(w + 2 * i, (uint8_t)self->id[i]);
    W32(ARG(2), w);
    STDRET(2, HR_OK);
}
M(pin, QueryPinInfo) {
    HOSTFN; SELF;
    uint32_t p = ARG(2);
    memset(mb + p, 0, 8 + 128);
    W32(p, 0); W32(p + 4, (uint32_t)self->dir);
    for (size_t i = 0; self->id[i] && i < 60; i++) W16(p + 8 + 2 * i, (uint8_t)self->id[i]);
    STDRET(2, HR_OK);
}
M(pin, ConnectedTo) { HOSTFN; SELF; W32(ARG(2), self->peer); STDRET(2, self->peer ? HR_OK : 0x80040209u); }
M(pin, Disconnect) { HOSTFN; SELF; self->peer = 0; STDRET(1, HR_OK); }
M(pin, Ok1) { STDRET(2, HR_OK); }
M(pin, Ok2) { STDRET(3, HR_OK); }
M(pin, NotImpl2) { STDRET(2, E_NOTIMPL_); }
M(pin, NotImpl3) { STDRET(3, E_NOTIMPL_); }
M(pin, Ok0) { STDRET(1, HR_OK); }
M(pin, NewSeg) { STDRET(6, HR_OK); }
static const gfn pin_vt[] = {any_QI, any_AddRef, any_Release, pin_Ok2 /*Connect*/, pin_Ok2 /*ReceiveConnection*/,
    pin_Disconnect, pin_ConnectedTo, pin_NotImpl2 /*ConnectionMediaType*/, pin_QueryPinInfo, pin_QueryDirection,
    pin_QueryId, pin_Ok1 /*QueryAccept*/, pin_NotImpl2 /*EnumMediaTypes*/, pin_NotImpl3 /*QueryInternalConnections*/,
    pin_Ok0 /*EndOfStream*/, pin_Ok0, pin_Ok0, pin_NewSeg};

typedef struct { uint32_t pins[4]; int n, pos; } EnumState;
M(en, Next) {
    HOSTFN;
    HObj *self = (HObj *)com_get(ARG(1));
    EnumState *e = (EnumState *)(uintptr_t)self->peer;
    uint32_t want = ARG(2), out = ARG(3), got = 0;
    while (got < want && e->pos < e->n) {
        uint32_t p = e->pins[e->pos++];
        d3d_addref(com_get(p));
        W32(out + 4 * got, p);
        got++;
    }
    if (ARG(4)) W32(ARG(4), got);
    STDRET(4, got == want ? HR_OK : S_FALSE_);
}
M(en, Skip) { STDRET(2, HR_OK); }
M(en, Reset) { HOSTFN; HObj *self = (HObj *)com_get(ARG(1)); ((EnumState *)(uintptr_t)self->peer)->pos = 0; STDRET(1, HR_OK); }
M(en, Clone) { STDRET(2, E_NOTIMPL_); }
static const gfn en_vt[] = {any_QI, any_AddRef, any_Release, en_Next, en_Skip, en_Reset, en_Clone};

// ---------- host filters (IBaseFilter) ----------
M(flt, EnumPins) {
    HOSTFN; SELF;
    EnumState *e = calloc(1, sizeof *e);
    for (int i = 0; i < 10; i++)
        if (self->m->pins[i] && self->m->pins[i]->com.kind == MK_PIN && self->m->pins[i]->role == self->role)
            e->pins[e->n++] = self->m->pins[i]->com.guest;
    HObj *eo;
    uint32_t g = mkobj(self->m, MK_ENUM, en_vt, 7, "IEnumPins", &eo);
    eo->peer = (uint32_t)(uintptr_t)e;
    W32(ARG(2), g);
    STDRET(2, HR_OK);
}
M(flt, Ok1) { STDRET(1, HR_OK); }
M(flt, Ok2) { STDRET(2, HR_OK); }
M(flt, Ok3) { STDRET(3, HR_OK); }
M(flt, GetState) { HOSTFN; W32(ARG(3), 0); STDRET(3, HR_OK); }
M(flt, FindPin) { HOSTFN; W32(ARG(3), 0); STDRET(3, 0x80040209u); }
M(flt, NI) { STDRET(2, E_NOTIMPL_); }
M(flt, QueryFilterInfo) { HOSTFN; memset(mb + ARG(2), 0, 4 + 128 + 4); STDRET(2, HR_OK); }
M(flt, Join) { STDRET(3, HR_OK); }
static const gfn flt_vt[] = {any_QI, any_AddRef, any_Release, flt_NI /*GetClassID*/, flt_Ok1 /*Stop*/, flt_Ok1 /*Pause*/,
    flt_Ok3 /*Run (i64)*/, flt_GetState, flt_Ok2 /*SetSyncSource*/, flt_Ok2 /*GetSyncSource*/, flt_EnumPins,
    flt_FindPin, flt_QueryFilterInfo, flt_Join, flt_NI /*QueryVendorInfo*/};

static void make_filter(Movie *m, int which, int nin, int nout, const char *in_id, const char *out_ids[2]) {
    uint32_t g = mkobj(m, MK_FILTER, flt_vt, 15, "IBaseFilter", &m->filters[which]);
    m->filters[which]->role = which;
    (void)g;
    for (int i = 0; i < nin + nout; i++) {
        HObj *p;
        mkobj(m, MK_PIN, pin_vt, 18, "IPin", &p);
        p->role = which;
        p->dir = i < nin ? 0 : 1;
        p->id = i < nin ? in_id : out_ids[i - nin];
        for (int k = 0; k < 10; k++)
            if (!m->pins[k]) { m->pins[k] = p; break; }
    }
}


// ---------- memory allocator and samples (host side) ----------
#define MV Movie *m = ((HObj *)com_get(ARG(1)))->m
#define SAMP Samp *sp = (Samp *)com_get(ARG(1))
static int movie_sample_addref(Samp *sp) {
    SDL_LockMutex(sp->m->lock);
    if (!sp->busy || sp->com.refs<=0) { SDL_UnlockMutex(sp->m->lock); rt_fatal("[movie] AddRef of inactive sample"); }
    int r=++sp->com.refs;
    SDL_UnlockMutex(sp->m->lock); return r;
}
static int movie_sample_release(Samp *sp) {
    SDL_LockMutex(sp->m->lock);
    if (!sp->busy || sp->com.refs<=0) { SDL_UnlockMutex(sp->m->lock); rt_fatal("[movie] Release of inactive sample"); }
    int r=--sp->com.refs;
    if (!r) sp->busy=0;
    SDL_UnlockMutex(sp->m->lock); return r;
}
M(sm, AddRef) { HOSTFN; SAMP; int r=movie_sample_addref(sp); STDRET(1,(uint32_t)r); }
M(sm, Release) { HOSTFN; SAMP; int r=movie_sample_release(sp); STDRET(1,(uint32_t)r); }
M(sm, GetPointer) { HOSTFN; SAMP; W32(ARG(2), sp->buf); STDRET(2, HR_OK); }
M(sm, GetSize) { HOSTFN; SAMP; SDL_LockMutex(sp->m->lock); uint32_t size=sp->size; SDL_UnlockMutex(sp->m->lock); STDRET(1, size); }
M(sm, GetTime) {
    HOSTFN; SAMP;
    SDL_LockMutex(sp->m->lock);
    int have_time=sp->have_time; int64_t start=sp->t0, end=sp->t1;
    SDL_UnlockMutex(sp->m->lock);
    if (!have_time) STDRET(3, 0x80040249u);
    W32(ARG(2), (uint32_t)start); W32(ARG(2) + 4, (uint32_t)((uint64_t)start >> 32));
    W32(ARG(3), (uint32_t)end); W32(ARG(3) + 4, (uint32_t)((uint64_t)end >> 32));
    STDRET(3, HR_OK);
}
M(sm, SetTime) {
    HOSTFN; SAMP;
    int64_t start=(int64_t)(((uint64_t)R32(ARG(2) + 4) << 32) | R32(ARG(2)));
    int64_t end=(int64_t)(((uint64_t)R32(ARG(3) + 4) << 32) | R32(ARG(3)));
    SDL_LockMutex(sp->m->lock);
    sp->t0=start; sp->t1=end; sp->have_time=1;
    SDL_UnlockMutex(sp->m->lock);
    STDRET(3, HR_OK);
}
M(sm, IsSync) { HOSTFN; SAMP; STDRET(1, sp->sync ? HR_OK : S_FALSE_); }
#ifdef SH3_MOVIE_LAB
static void sm_GetMT(CPU *c);
static void sm_GetLen(CPU *c);
/* Avoid the unsynchronised COM registry only for the sample owned by this
   very worker in push_frame's synchronous Receive call. */
int movie_lab_try_active_sample(gfn fn, CPU *c) {
    if ((fn != sm_IsSync && fn != sm_GetTime && fn != sm_GetMT && fn != sm_GetPointer && fn != sm_GetLen && fn != sm_GetSize && fn != sm_AddRef && fn != sm_Release) || !movie_active_sample || movie_active_sample_cpu != c ||
        !g_mem || g_mem_size < 8 || c->esp > g_mem_size - 8) return 0;
    uint32_t object;
    memcpy(&object, g_mem + c->esp + 4, sizeof object);
    if (object != movie_active_sample->com.guest) return 0;
    if (fn == sm_IsSync) {
        c->eax = atomic_load_explicit(&movie_active_sample->sync, memory_order_relaxed) ? HR_OK : S_FALSE_;
        c->esp += 8;
        return 1;
    }
    if (fn == sm_AddRef || fn == sm_Release) {
        Samp *sp=movie_active_sample;
        SDL_LockMutex(sp->m->lock);
        int refs=sp->com.refs;
        int valid=sp->busy && (fn==sm_AddRef ? refs>0 && refs<0x7fffffff : refs>1);
        if (valid) refs=fn==sm_AddRef ? ++sp->com.refs : --sp->com.refs;
        SDL_UnlockMutex(sp->m->lock);
        if (!valid) return 0;
        c->eax=(uint32_t)refs; c->esp+=8;
        return fn==sm_AddRef ? 7 : 8;
    }
    if (fn == sm_GetSize) {
        Samp *sp=movie_active_sample;
        SDL_LockMutex(sp->m->lock); uint32_t size=sp->size; int busy=sp->busy; SDL_UnlockMutex(sp->m->lock);
        if (!busy) return 0;
        c->eax=size; c->esp+=8;
        return 6;
    }
    if (fn == sm_GetLen) {
        Samp *sp=movie_active_sample;
        SDL_LockMutex(sp->m->lock); uint32_t len=sp->len; int busy=sp->busy; SDL_UnlockMutex(sp->m->lock);
        if (!busy) return 0;
        c->eax=len; c->esp+=8;
        return 5;
    }
    if (fn == sm_GetPointer) {
        if (g_mem_size < 12 || c->esp > g_mem_size-12) return 0;
        uint32_t out; memcpy(&out,g_mem+c->esp+8,4);
        if (g_mem_size < 4 || out > g_mem_size-4) return 0;
        Samp *sp=movie_active_sample;
        SDL_LockMutex(sp->m->lock);
        uint32_t buf=sp->buf, size=sp->size;
        int valid=sp->busy && buf && size && size<=g_mem_size && buf<=g_mem_size-size;
        SDL_UnlockMutex(sp->m->lock);
        if (!valid) return 0;
        memcpy(g_mem+out,&buf,4); c->eax=HR_OK; c->esp+=12;
        return 4;
    }
    if (fn == sm_GetMT) {
        if (g_mem_size < 12 || c->esp > g_mem_size-12) return 0;
        uint32_t out; memcpy(&out,g_mem+c->esp+8,4);
        if (g_mem_size < 4 || out > g_mem_size-4) return 0;
        uint32_t zero=0; memcpy(g_mem+out,&zero,4);
        c->eax=S_FALSE_; c->esp+=12;
        return 3;
    }
    if (g_mem_size < 16 || c->esp > g_mem_size - 16) return 0;
    uint32_t out_start,out_end;
    memcpy(&out_start,g_mem+c->esp+8,4); memcpy(&out_end,g_mem+c->esp+12,4);
    if (g_mem_size < 8 || out_start > g_mem_size-8 || out_end > g_mem_size-8) return 0;
    Samp *sp=movie_active_sample;
    SDL_LockMutex(sp->m->lock);
    int have_time=sp->have_time; int64_t start=sp->t0, end=sp->t1;
    SDL_UnlockMutex(sp->m->lock);
    if (have_time) {
        memcpy(g_mem+out_start,&start,8); memcpy(g_mem+out_end,&end,8);
        c->eax=HR_OK;
    } else c->eax=0x80040249u;
    c->esp+=16;
    return 2;
}
#endif
M(sm, SetSync) { HOSTFN; SAMP; sp->sync = (int)ARG(2); STDRET(2, HR_OK); }
M(sm, False1) { STDRET(1, S_FALSE_); }
#ifdef SH3_MOVIE_LAB
/* Only these exact stubs change the calling CPU's return register and stack.
   Some are shared by several interfaces, so identify functions, not labels. */
int movie_lab_is_pure_worker_stub(gfn fn) { return fn == any_QI || fn == sm_False1; }
#endif
M(sm, Ok2) { STDRET(2, HR_OK); }
M(sm, GetLen) { HOSTFN; SAMP; SDL_LockMutex(sp->m->lock); uint32_t len=sp->len; SDL_UnlockMutex(sp->m->lock); STDRET(1, len); }
M(sm, SetLen) { HOSTFN; SAMP; SDL_LockMutex(sp->m->lock); sp->len = ARG(2); SDL_UnlockMutex(sp->m->lock); STDRET(2, HR_OK); }
M(sm, GetMT) { HOSTFN; W32(ARG(2), 0); STDRET(2, S_FALSE_); }
M(sm, GetMediaTime) { STDRET(3, 0x80040251u); }
M(sm, Ok3) { STDRET(3, HR_OK); }
static const gfn sm_vt[] = {any_QI, sm_AddRef, sm_Release, sm_GetPointer, sm_GetSize, sm_GetTime, sm_SetTime,
    sm_IsSync, sm_SetSync, sm_False1 /*IsPreroll*/, sm_Ok2, sm_GetLen, sm_SetLen, sm_GetMT, sm_Ok2 /*SetMediaType*/,
    sm_False1 /*IsDiscontinuity*/, sm_Ok2, sm_GetMediaTime, sm_Ok3};

#include "movie_sample_reserve.h"
M(al, SetProperties) {
    HOSTFN; MV;
    uint32_t req = ARG(2), act = ARG(3);
    uint32_t sz = R32(req + 4);
    static uint32_t sbuf[NSAMP], scap[NSAMP];
#ifdef SH3_MOVIE_LAB
    /* The reusable pool outlives each graph. SetProperties is serialized by
       the COM dispatcher; hold both owner locks until the pool is transferred
       so no output worker can reserve a sample while buffers are replaced. */
    static Movie *pool_owner;
    Movie *previous=pool_owner!=m?pool_owner:NULL;
    if (previous) SDL_LockMutex(previous->lock);
    SDL_LockMutex(m->lock);
    if (previous) for (int i=0;i<NSAMP;i++) if (previous->samp[i] && previous->samp[i]->busy) {
        SDL_UnlockMutex(m->lock); SDL_UnlockMutex(previous->lock); STDRET(3, HR_FAIL);
    }
#endif
    Samp *new_samples[NSAMP] = {0};
    for (int i = 0; i < NSAMP; ++i)
        if (m->samp[i] && m->samp[i]->busy) {
#ifdef SH3_MOVIE_LAB
            SDL_UnlockMutex(m->lock); if (previous) SDL_UnlockMutex(previous->lock);
#endif
            STDRET(3, HR_FAIL);
        }
    for (int i = 0; i < NSAMP; ++i) if (!m->samp[i]) {
        new_samples[i] = calloc(1, sizeof(Samp));
        if (!new_samples[i]) {
            for (int j = 0; j < i; ++j) free(new_samples[j]);
#ifdef SH3_MOVIE_LAB
            SDL_UnlockMutex(m->lock); if (previous) SDL_UnlockMutex(previous->lock);
#endif
            STDRET(3, 0x8007000eu);
        }
    }
    if (!movie_samples_reserve(sbuf, scap, NSAMP, sz)) {
        for (int i = 0; i < NSAMP; ++i) free(new_samples[i]);
#ifdef SH3_MOVIE_LAB
        SDL_UnlockMutex(m->lock); if (previous) SDL_UnlockMutex(previous->lock);
#endif
        STDRET(3, 0x8007000eu);
    }
    m->buf_size = sz;
    for (int i = 0; i < NSAMP; i++) {
        if (!m->samp[i]) {
            m->samp[i] = new_samples[i];
            m->samp[i]->m = m;
            com_wrap(&m->samp[i]->com, MK_SAMP, sm_vt, 19, "IMediaSample");
        }
        m->samp[i]->buf = sbuf[i];
        m->samp[i]->size = sz;
    }
#ifdef SH3_MOVIE_LAB
    pool_owner=m;
    SDL_UnlockMutex(m->lock); if (previous) SDL_UnlockMutex(previous->lock);
#endif
    if (act) { W32(act, NSAMP); W32(act + 4, sz); W32(act + 8, R32(req + 8)); W32(act + 12, 0); }
    STDRET(3, HR_OK);
}
M(al, GetProperties) { HOSTFN; MV; W32(ARG(2), NSAMP); W32(ARG(2) + 4, m->buf_size); W32(ARG(2) + 8, 1); W32(ARG(2) + 12, 0); STDRET(2, HR_OK); }
M(al, Ok1) { STDRET(1, HR_OK); }
M(al, GetBuffer) {
    HOSTFN; MV;
    for (int tries = 0; tries < 500; tries++) {
        SDL_LockMutex(m->lock);
        for (int i = 0; i < NSAMP; i++)
            if (m->samp[i] && !m->samp[i]->busy) {
                Samp *sp = m->samp[i];
                sp->busy = 1; sp->com.refs = 1; sp->have_time = 0; sp->len = 0; sp->sync = 0;
                W32(ARG(2), sp->com.guest);
                SDL_UnlockMutex(m->lock);
                STDRET(5, HR_OK);
            }
        SDL_UnlockMutex(m->lock);
        SDL_Delay(2);
    }
    W32(ARG(2), 0);
    STDRET(5, HR_FAIL);
}
M(al, ReleaseBuffer) {
    HOSTFN; Samp *sp=(Samp *)com_get(ARG(2));
    if (!sp || sp->com.kind!=MK_SAMP) STDRET(2,HR_FAIL);
    SDL_LockMutex(sp->m->lock);
    int active=sp->com.refs>0;
    if (!active) sp->busy=0;
    SDL_UnlockMutex(sp->m->lock);
    STDRET(2,active?HR_FAIL:HR_OK);
}
static const gfn al_vt[] = {any_QI, any_AddRef, any_Release, al_SetProperties, al_GetProperties, al_Ok1 /*Commit*/,
    al_Ok1 /*Decommit*/, al_GetBuffer, al_ReleaseBuffer};

// ---------- decoding and delivery ----------
static uint32_t guest_tmp(void) {
    static uint32_t t;
    if (!t) t = gheap_calloc(256);
    return t;
}

#ifdef __ARM_NEON
#include <arm_neon.h>
// YUV 4:2:0 -> BGRA (or R5G6B5 if p565) in NEON (16 pixels x 2 rows per iteration), BT.601 in 6-bit fixed point (differs by a few
// units from plm_frame_to_bgra, cf. SH4_MOVIE_BENCH). Alpha = 255.
static inline __attribute__((always_inline)) void frame_conv(plm_frame_t *f, uint8_t *dst, int stride, const int p565) {
    int w = (int)f->width, h = (int)f->height, yw = (int)f->y.width, cw = (int)f->cb.width;
    const int16x8_t c128 = vdupq_n_s16(128), yoff = vdupq_n_s16(1192);
    const uint8x8_t k149 = vdup_n_u8(149);
    for (int j = 0; j < h; j += 2) {
        const uint8_t *cb = f->cb.data + (j >> 1) * cw, *cr = f->cr.data + (j >> 1) * cw;
        for (int i = 0; i < w; i += 16) {
            int16x8_t u = vsubq_s16(vreinterpretq_s16_u16(vmovl_u8(vld1_u8(cb + i / 2))), c128);
            int16x8_t v = vsubq_s16(vreinterpretq_s16_u16(vmovl_u8(vld1_u8(cr + i / 2))), c128);
            int16x8x2_t rr = vzipq_s16(vmulq_n_s16(v, 102), vmulq_n_s16(v, 102));
            int16x8_t g = vmlaq_n_s16(vmulq_n_s16(u, -25), v, -52), b = vmulq_n_s16(u, 129);
            int16x8x2_t gg = vzipq_s16(g, g), bb = vzipq_s16(b, b);
            for (int k = 0; k < 2; k++) {
                uint8x16_t yv = vld1q_u8(f->y.data + (j + k) * yw + i);
                uint8_t *d = dst + (j + k) * stride + i * (p565 ? 2 : 4);
                for (int hf = 0; hf < 2; hf++) {
                    uint8x8_t y8 = hf ? vget_high_u8(yv) : vget_low_u8(yv);
                    int16x8_t yy = vsubq_s16(vreinterpretq_s16_u16(vshrq_n_u16(vmull_u8(y8, k149), 1)), yoff);   // (Y-16)*74.5
                    uint8x8x4_t px;
                    px.val[0] = vqrshrun_n_s16(vqaddq_s16(yy, bb.val[hf]), 6);
                    px.val[1] = vqrshrun_n_s16(vqaddq_s16(yy, gg.val[hf]), 6);
                    px.val[2] = vqrshrun_n_s16(vqaddq_s16(yy, rr.val[hf]), 6);
                    if (p565) {   // R<<8, then insert G>>5 and B>>11 -> R5G6B5
                        uint16x8_t q = vsriq_n_u16(vshll_n_u8(px.val[2], 8), vshll_n_u8(px.val[1], 8), 5);
                        vst1q_u16((uint16_t *)(d + hf * 16), vsriq_n_u16(q, vshll_n_u8(px.val[0], 8), 11));
                    } else {
                        px.val[3] = vdup_n_u8(255);
                        vst4_u8(d + hf * 32, px);
                    }
                }
            }
        }
    }
}
static void frame_to_bgra(plm_frame_t *f, uint8_t *dst, int stride) {
    if (((int)f->width | (int)f->height) & 15) plm_frame_to_bgra(f, dst, stride);
    else frame_conv(f, dst, stride, 0);
}

// Movie (Vita): the frame is also produced as R5G6B5 (top-down) in one of 3 rotating buffers, converted here (core 2); the main thread
// only has to hand it to the render thread (resource.c, tex_upload). busy: 1 = being written or waiting for upload (reset to 0 by the renderer).
#define N565 6
static uint16_t *s_m565[N565];
static volatile int s_m565_busy[N565];
static int s_m565_q[N565];                    // FIFO of complete buffers in display order (q[n % N565]); a buffer is free when not busy and not queued
static unsigned s_m565_qh, s_m565_qt, s_m565_w, s_m565_h;
static unsigned s_m565_dropped;               // converted frames dropped because the display was too far behind
static SDL_mutex *s_m565_mx;
static double s_m565_ms;
static unsigned s_m565_n;
static double s_m565_pts[N565];
static uint64_t s_m565_ready[N565];
static unsigned s_m565_serial;
static void m565_begin(void) {
    if (!s_m565_mx) return;
    SDL_LockMutex(s_m565_mx);
    s_m565_qh = s_m565_qt; // discard only queued frames, never buffers owned by the renderer
    s_m565_dropped = 0;
    SDL_UnlockMutex(s_m565_mx);
}
static int m565_queued(int k) { for (unsigned n = s_m565_qh; n != s_m565_qt; n++) if (s_m565_q[n % N565] == k) return 1; return 0; }
static void m565_put(Movie *m, plm_frame_t *f) {
    if (((int)f->width | (int)f->height) & 15) return;   // ponytail: sizes not a multiple of 16: fall back to the main thread conversion
    if (!s_m565_mx) s_m565_mx = SDL_CreateMutex();
    if (!s_m565_w) { s_m565_w = (unsigned)m->w; s_m565_h = (unsigned)m->h; }
    if (s_m565_w != (unsigned)m->w || s_m565_h != (unsigned)m->h) return;   // ponytail: buffers sized for the first movie (all 640x480)
    int k = 0;
    SDL_LockMutex(s_m565_mx);
    while (k < N565 && (s_m565_busy[k] || m565_queued(k))) k++;
    if (k < N565) s_m565_busy[k] = 1;
    else s_m565_dropped++;
    SDL_UnlockMutex(s_m565_mx);
    if (k == N565) return;   // the renderer did not return the buffers: frame skipped
    if (!s_m565[k] && !(s_m565[k] = malloc((size_t)m->w * m->h * 2))) { s_m565_busy[k] = 0; return; }
    uint64_t t0 = SDL_GetPerformanceCounter();
    s_m565_pts[k] = f->time;
    frame_conv(f, (uint8_t *)s_m565[k], m->w * 2, 1);
    s_m565_ms += (double)(SDL_GetPerformanceCounter() - t0) * 1000.0 / (double)SDL_GetPerformanceFrequency();
    SDL_LockMutex(s_m565_mx);
    s_m565_ready[k] = SDL_GetPerformanceCounter();
    s_m565_q[s_m565_qt % N565] = k; s_m565_qt++;
    s_m565_busy[k] = 0;
    SDL_UnlockMutex(s_m565_mx);
    if (!(++s_m565_n % 75)) { rt_log("[movie] 565 conversion (movie thread): %.1f ms\n", s_m565_ms / 75); s_m565_ms = 0; }
}
// Main thread: next 565 frame in display order not yet uploaded (NULL: nothing new). One frame per call, so every decoded frame is shown;
// if more than 3 are waiting the oldest are dropped (latency stays under ~100 ms). The buffer stays reserved until *busy drops to 0.
uint16_t *movie_565_take(uint32_t w, uint32_t h, volatile int **busy) {
    uint16_t *p = NULL;
    if (!s_m565_mx) return NULL;
    SDL_LockMutex(s_m565_mx);
    if (s_m565_qh != s_m565_qt && w == s_m565_w && h == s_m565_h) {
        while (s_m565_qt - s_m565_qh > 3) { s_m565_qh++; s_m565_dropped++; }
        int k = s_m565_q[s_m565_qh % N565]; s_m565_qh++;
        s_m565_busy[k] = 1;
        {   // diagnostics: order and regularity of what is shown (windows of 120 presents)
            extern unsigned g_pframe;
            static double last_pts = -1; static unsigned win0, ntake, nback, maxgap, lastf;
            if (s_m565_serial != s_movie_serial) {
                last_pts = -1; win0 = g_pframe; ntake = nback = maxgap = lastf = 0;
            }
            double pts = s_m565_pts[k];
            if (pts < last_pts - 1e-6) nback++;
            last_pts = pts; ntake++;
            if (lastf && g_pframe - lastf > maxgap) maxgap = g_pframe - lastf;
            lastf = g_pframe;
            if (g_pframe - win0 >= 120) { rt_log("[movie] display: %u new frames shown in %u presents, %u dropped, %u out of order, longest gap %u presents\n", ntake, g_pframe - win0, s_m565_dropped, nback, maxgap); win0 = g_pframe; ntake = nback = maxgap = 0; s_m565_dropped = 0; }
        }
        *busy = &s_m565_busy[k];
        p = s_m565[k];
        // Low-rate wall-clock diagnostics; Present counts alone hide long gaps.
        static uint64_t begin, previous, max_gap;
        static unsigned shown, gaps50, gaps100;
        static double max_queue_ms;
        uint64_t now = SDL_GetPerformanceCounter(), hz = SDL_GetPerformanceFrequency();
        int first = s_m565_serial != s_movie_serial;
        if (first) {
            s_m565_serial = s_movie_serial;
            begin = previous = now; max_gap = 0; shown = gaps50 = gaps100 = 0; max_queue_ms = 0;
        }
        uint64_t gap = now - previous; previous = now;
        if (gap > max_gap) max_gap = gap;
        if (gap * 1000 > hz * 50) gaps50++;
        if (gap * 1000 > hz * 100) gaps100++;
        double age = (double)(now - s_m565_ready[k]) * 1000.0 / (double)hz;
        if (age > max_queue_ms) max_queue_ms = age;
        if (!first) shown++;
        if (now - begin >= 2 * hz) {
            rt_log("[movie-flow] shown=%.2f/s max_gap_ms=%.2f gaps50=%u gaps100=%u queue_max_ms=%.2f pts=%.3f audio_minus_video_ms=%.2f\n",
                   shown * (double)hz / (double)(now - begin), max_gap * 1000.0 / (double)hz,
                   gaps50, gaps100, max_queue_ms, s_m565_pts[k],
                   g_movie_audio_played / 48.0 - s_m565_pts[k] * 1000.0);
            begin = now; shown = gaps50 = gaps100 = 0; max_gap = 0; max_queue_ms = 0;
        }
    }
    SDL_UnlockMutex(s_m565_mx);
    return p;
}
#else
#define frame_to_bgra plm_frame_to_bgra
#define m565_put(m, f) ((void)0)
#define m565_begin() ((void)0)
#endif

static void push_frame(Movie *m, CPU *c, plm_frame_t *f) {
    HOSTFN;
#if defined(SH3_MOVIE_LAB) && defined(__vita__)
    static int movie_push_audit;
    if (!movie_push_audit && getenv("SH3_DIAG_MOVIE_TEXTURE_AUDIT")) {
        movie_push_audit = 1;
        rt_log("[movie-push-audit] film=%dx%d globals=%dx%d\n",m->w,m->h,g_movie_w,g_movie_h);
    }
#endif
    Samp *sp = NULL;
    SDL_LockMutex(m->lock);
    for (int i = 0; i < NSAMP && !sp; i++)
        if (m->samp[i] && !m->samp[i]->busy) sp = m->samp[i];
    if (sp && m->mem_in) { sp->busy=1; sp->com.refs=1; }
    SDL_UnlockMutex(m->lock);
    if (!sp || !m->mem_in) {
        static int nlog; if (nlog++ < 5) rt_log("[movie] push skipped: sp=%p mem_in=%08x\n", (void *)sp, m->mem_in);
        return;                                                 // the renderer did not return its samples: we skip the frame
    }
    uint32_t pitch = (uint32_t)m->w * 4, size = pitch * (uint32_t)m->h;
    uint64_t t0 = SDL_GetPerformanceCounter();
#ifdef SH3_MOVIE_LAB
    if (getenv("SH3_DIAG_MOVIE_TOPDOWN"))
        frame_to_bgra(f, mb + sp->buf, (int)pitch);
    else
#endif
        frame_to_bgra(f, mb + sp->buf + (size - pitch), -(int)pitch);   // DIB bas-haut
    m565_put(m, f);
    uint64_t t1 = SDL_GetPerformanceCounter();
    SDL_LockMutex(m->lock);
    sp->len = size;
    sp->t0 = (int64_t)(f->time * 1e7);
    sp->t1 = sp->t0 + 333667;
    sp->have_time = 1;
    SDL_UnlockMutex(m->lock);
    sp->sync = 1;
#ifdef SH3_MOVIE_LAB
    movie_active_sample = sp; movie_active_sample_cpu = c;
#endif
    uint32_t rhr = gcall(c, m->mem_in, 6, 1, sp->com.guest);    // IMemInputPin::Receive
#ifdef SH3_MOVIE_LAB
    movie_active_sample = NULL; movie_active_sample_cpu = NULL;
#endif
#ifdef SH3_MOVIE_LAB
    if(getenv("SH3_DIAG_MOVIE_ONE_FRAME") || getenv("SH3_DIAG_MOVIE_RUN_FRAME")) {
        SDL_LockMutex(m->lock);
        rt_log("[movie-lab] frame Receive hr=%08x time=%.6f sample=%08x refs=%d busy=%d\n",
               rhr,f->time,sp->com.guest,sp->com.refs,sp->busy);
        SDL_UnlockMutex(m->lock);
    }
    if(getenv("SH3_DIAG_MOVIE_COMPARE_BUFFER") && g_mem_size>0x72c0a8u) {
        uint32_t dst=R32(0x72c0a4u),pitch=R32(0x72c0a0u);
        if(dst && dst<=g_mem_size && sp->buf<=g_mem_size && pitch==(uint32_t)m->w*4 && size<=g_mem_size-dst && size<=g_mem_size-sp->buf) {
            size_t different=0;for(uint32_t i=0;i<size;i++)different+=mb[dst+i]!=mb[sp->buf+i];
            rt_log("[movie-lab] sample vs guest surface different=%zu total=%u\n",different,size);
        } else rt_log("[movie-lab] sample comparison unsupported pointer=%08x pitch=%u\n",dst,pitch);
    }
#endif
    uint64_t t2 = SDL_GetPerformanceCounter();
    double ms = 1000.0 / (double)SDL_GetPerformanceFrequency();
    m->st_conv += (double)(t1 - t0) * ms; m->st_recv += (double)(t2 - t1) * ms; m->st_npush++;
    if ((int32_t)rhr < 0) rt_log("[movie] Receive -> %08x\n", rhr);
    movie_sample_release(sp); /* Release only our reference; renderer may retain its own. */
}
#ifdef SH3_MOVIE_LAB
void movie_lab_present_probe(CPU *c) {
    HOSTFN;
    static int done;
    static uint64_t last_tick;
    int sequence=getenv("SH3_DIAG_MOVIE_PRESENT_SEQUENCE")!=NULL;
    if(done >= (sequence ? 3 : 1) || !g_mv || !g_mem || g_mem_size<=0x72c0bfu) return;
    if(R8(0x72c064u)!=3 || !R32(0x72c0bcu)) return;
    if(sequence && done) {
        uint64_t now=SDL_GetPerformanceCounter();
        if(now-last_tick<SDL_GetPerformanceFrequency()/2) return;
        if(!g_mv->plm || !g_mv->mem_in) return;
        plm_frame_t *frame=NULL;
        for(unsigned k=0;k<15;k++) { frame=plm_decode_video(g_mv->plm); if(!frame) break; }
        if(!frame) { rt_log("[movie-lab] sequence decode ended\n"); done=3; return; }
        push_frame(g_mv,c,frame);
        rt_log("[movie-lab] sequence frame=%d time=%.6f\n",done+1,frame->time);
    }
    last_tick=SDL_GetPerformanceCounter();
    done++;
    if (getenv("SH3_DIAG_MOVIE_FIT_PROBE")) {
        /* Diagnostic geometry only: fit the 4:3 source into a 960x544 target.
           Use the guest setter; this does not establish the natural movie path. */
        uint32_t coords[4]; float rect[4]={117.33333f,0.0f,842.66669f,544.0f};
        memcpy(coords,rect,sizeof coords);
        rt_guest_call(c,0x00403e70u,4,coords[0],coords[1],coords[2],coords[3]);
    }
    if(sequence) {
        float rect[4]; memcpy(rect,mb+0x72c070u,sizeof rect);
        rt_log("[movie-lab] guest movie rectangle %.3f %.3f %.3f %.3f render-size=%ux%u\n",rect[0],rect[1],rect[2],rect[3],R32(0x72bfd0u),R32(0x72bfd4u));
    }
    rt_log("[movie-lab] drawing guest movie function on presentation thread\n");
    uint32_t result=rt_guest_call(c,0x00404440u,0);
    rt_log("[movie-lab] presentation draw returned %08x\n",result);
    if(getenv("SH3_DIAG_MOVIE_CAPTURE_DRAW")) { extern void diag_movie_capture_draw(unsigned);diag_movie_capture_draw(sequence ? (unsigned)done : 0); }
    if(getenv("SH3_DIAG_CAPTURE")) { extern void diag_capture_request(unsigned);diag_capture_request(2); }
}
#endif

static plm_frame_t *g_pending;
// Video decode time: gap between two plm_decode callbacks (g_cb_t reset at the start of each plm_decode).
// Audio: measured in its own thread.
static uint64_t g_cb_t;
static double g_st_vid, g_st_aud;
static double cb_ms(void) { uint64_t n = SDL_GetPerformanceCounter(); double d = (double)(n - g_cb_t) * 1000.0 / (double)SDL_GetPerformanceFrequency(); g_cb_t = n; return d; }
static int g_handed;
static void hand_frame(Movie *m, plm_frame_t *f);
static void on_video(plm_t *p, plm_frame_t *f, void *u) {
    g_st_vid += cb_ms();
    Movie *m = u;
    if (p->video_decoder->skipped) { m->st_nskip++; return; }   // B frame skipped (late)
    m->st_ndec++;
    if (m->yuv[0]) { hand_frame(m, f); g_handed = 1; }   // every decoded frame goes to the output FIFO, in display order
    else g_pending = f;
}

// SH4_MOVIE_BENCH=s: at startup, decodes s seconds of m_00 alone (nothing else running): raw cost of pl_mpeg.
void movie_bench(const char *dir, int secs) {
    char p[300];
    SDL_snprintf(p, sizeof p, "%s/movie/m_00.pac", dir);
    plm_t *plm = plm_create_with_filename(p);
    uint8_t *rgb = malloc(640 * 480 * 4);
    if (!plm || !rgb) { rt_log("[bench] %s: cannot open\n", p); return; }
    double ms = 1000.0 / (double)SDL_GetPerformanceFrequency(), dec = 0, conv = 0, aud = 0;
    plm_set_audio_enabled(plm, 0);
    int n = 0, na = 0;
    uint32_t hash = 2166136261u;
    for (; n < secs * 30; n++) {
        uint64_t t0 = SDL_GetPerformanceCounter();
        plm_frame_t *f = plm_decode_video(plm);
        uint64_t t1 = SDL_GetPerformanceCounter();
        if (!f) break;
        frame_to_bgra(f, rgb, (int)f->width * 4);
        dec += (double)(t1 - t0) * ms; conv += (double)(SDL_GetPerformanceCounter() - t1) * ms;
        // FNV-1a hash of the YUV planes: must equal that of test/plm_vbench.c (plain C) for the same number of frames
        plm_plane_t *pl[3] = { &f->y, &f->cb, &f->cr };
        for (int k = 0; k < 3; k++)
            for (size_t i = 0, e = (size_t)pl[k]->width * pl[k]->height; i < e; i++) hash = (hash ^ pl[k]->data[i]) * 16777619u;
        if (n % 30 == 0) rt_log("[bench] frame %d: hash %08x\n", n, (unsigned)hash);
        if (n == 90) {   // check: max difference against the reference conversion
            uint8_t *ref = malloc(640 * 480 * 4);
            int dmax = 0;
            if (ref) {
                plm_frame_to_bgra(f, ref, (int)f->width * 4);
                for (int i = 0; i < 640 * 480 * 4; i++) { int d = abs(ref[i] - rgb[i]); if ((i & 3) != 3 && d > dmax) dmax = d; }
                free(ref);
            }
            rt_log("[bench] max conversion error = %d\n", dmax);
        }
    }
    plm_destroy(plm);
    plm = plm_create_with_filename(p);
    if (plm) {
        plm_set_video_enabled(plm, 0);
        uint64_t t0 = SDL_GetPerformanceCounter();
        plm_samples_t *a;
        for (; (a = plm_decode_audio(plm)) && a->time < secs; na++) {}
        aud = (double)(SDL_GetPerformanceCounter() - t0) * ms;
    }
    rt_log("[bench] %d frames: decode %.1f ms/frame, BGRA conversion %.1f ms/frame; audio: %.0f ms for %d frames; hash %08x\n",
           n, n ? dec / n : 0, n ? conv / n : 0, aud, na, (unsigned)hash);
    free(rgb);
    if (plm) plm_destroy(plm);
}

// SH4_MOVIE_BENCH=s[,wait,core,core...]: with a wait, the bench runs during the game in a pinned thread
// successively on each given core (0-2): measures the contention suffered by movie decoding.
static char g_bench_dir[300];
static int g_bench_arg[8], g_bench_n;
static int bench_thread(void *u) {
    (void)u;
    SDL_Delay((uint32_t)g_bench_arg[1] * 1000);
    for (int i = 2; i < g_bench_n; i++) {
#ifdef __vita__
        sceKernelChangeThreadCpuAffinityMask(sceKernelGetThreadId(), SCE_KERNEL_CPU_MASK_USER_0 << g_bench_arg[i]);
#endif
        rt_log("[bench] core %d\n", g_bench_arg[i]);
        movie_bench(g_bench_dir, g_bench_arg[0]);
    }
    return 0;
}
void movie_bench_start(const char *dir, const char *spec) {
    SDL_strlcpy(g_bench_dir, dir, sizeof g_bench_dir);
    for (g_bench_n = 0; g_bench_n < 8 && *spec; g_bench_n++) {
        g_bench_arg[g_bench_n] = atoi(spec);
        spec += strcspn(spec, ",");
        if (*spec) spec++;
    }
    if (g_bench_n < 3) movie_bench(dir, g_bench_arg[0]);
    else SDL_CreateThread(bench_thread, "bench", NULL);
}
// newlib fills a FILE in BUFSIZ blocks (1 KB): unbuffered stream = one read per pl_mpeg refill (<= 128 KB).
static plm_buffer_t *movie_file(const char *host) {
    FILE *f = fopen(host, "rb");
    if (!f) return NULL;
    setvbuf(f, NULL, _IONBF, 0);
    return plm_buffer_create_with_file(f, 1);
}
static void movie_cached_load(plm_buffer_t *buffer, void *user) {
    if (buffer->discard_read_bytes) plm_buffer_discard_read_bytes(buffer);
    size_t n = movie_stream_read(user, buffer->bytes + buffer->length, buffer->capacity - buffer->length);
    buffer->length += n;
    if (!n) buffer->has_ended = TRUE;
}
static void movie_cached_seek(plm_buffer_t *buffer, size_t pos, void *user) {
    (void)buffer; ((MovieCursor *)user)->pos = pos;
}
static size_t movie_cached_tell(plm_buffer_t *buffer, void *user) {
    (void)buffer; return ((MovieCursor *)user)->pos;
}
static plm_buffer_t *movie_input(Movie *m, const char *host, int index) {
    if (!m->stream) return movie_file(host);
    m->cursor[index].source = m->stream; m->cursor[index].pos = 0;
    return plm_buffer_create_with_callbacks(movie_cached_load, movie_cached_seek,
                                          movie_cached_tell, m->stream->size, &m->cursor[index]);
}
// Audio buffer refill: demuxes up to the next audio packet (video packets are skipped).
static void apkt_load(plm_buffer_t *ab, void *u) {
    plm_packet_t *p;
    while ((p = plm_demux_decode(u)))
        if (p->type == PLM_DEMUX_PACKET_AUDIO_1) { plm_buffer_write(ab, p->data, p->length); return; }
    plm_buffer_signal_end(ab);
}
// Movie audio on core 2, ~0.25 s ahead of the mixer: video decoding keeps core 1 to itself.
static int audio_thread(void *arg) {
    Movie *m = arg;
#ifdef __vita__
    sceKernelChangeThreadCpuAffinityMask(sceKernelGetThreadId(), SCE_KERNEL_CPU_MASK_USER_2);
#endif
    const uint32_t cap = sizeof g_ring / sizeof g_ring[0];
    double ms = 1000.0 / (double)SDL_GetPerformanceFrequency();
    while (!m->stop && !m->ended) {
        plm_samples_t *s = NULL;
        SDL_mutex *lock=movie_ring_mutex();
        SDL_LockMutex(lock);
        uint32_t queued=(g_ring_w-g_ring_r+cap)%cap;
        SDL_UnlockMutex(lock);
        if (m->state == 2 && queued < 48000 * 2 / 4) {
            uint64_t t = SDL_GetPerformanceCounter();
            s = plm_audio_decode(m->aud);
            g_st_aud += (double)(SDL_GetPerformanceCounter() - t) * ms;
            if (s) movie_audio_push(s->interleaved, (int)s->count, plm_audio_get_samplerate(m->aud));
        }
        if (!s) SDL_Delay(5);
    }
    return 0;
}
// Caller must join every decoder/output/audio thread first. Queued graphics
// uploads own their separate RGB565 staging buffers, not these YUV slots.
static void movie_free_output(Movie *m) {
    for (int i = 0; i < OQ; i++) { free(m->yuv[i]); m->yuv[i] = NULL; }
    rt_free_cpu(m->dec_cpu);
    m->dec_cpu = NULL;
    m->ohead = m->otail = 0;
    m->obusy = -1;
}
static void movie_free_decoders(Movie *m) {
#if defined(__vita__) && defined(SH3_MOVIE_HW_LAB)
    movie_hw_close(m);
#endif
    if (m->plm) plm_destroy(m->plm);
    if (m->aud) plm_audio_destroy(m->aud);
    if (m->adm) plm_demux_destroy(m->adm);
    m->plm = NULL; m->aud = NULL; m->adm = NULL;
    movie_stream_close(m->stream); m->stream = NULL;
    movie_free_output(m);
}
// Decode -> output: YUV copy (~0.5 MB) into the next FIFO slot; if the output thread is too far behind, the oldest waiting frame is dropped.
static void hand_frame(Movie *m, plm_frame_t *f) {
    SDL_LockMutex(m->lock);
    while (m->otail - m->ohead >= OQ - 2) { m->ohead++; m->odropped++; }
    if (m->ohead == m->otail) m->ohead = m->otail = 0;
    int w;
    for (w = 0; w < OQ; w++) {
        if (w == m->obusy) continue;
        unsigned n = m->ohead;
        while (n != m->otail && m->oq[n % OQ] != w) n++;
        if (n == m->otail) break;
    }
    if (w == OQ) { m->odropped++; SDL_UnlockMutex(m->lock); return; }
    SDL_UnlockMutex(m->lock);
    size_t ys = (size_t)f->y.width * f->y.height, cs = (size_t)f->cb.width * f->cb.height;
    plm_frame_t *o = &m->of[w];
    *o = *f;
    o->y.data = m->yuv[w]; o->cb.data = o->y.data + ys; o->cr.data = o->cb.data + cs;
    memcpy(o->y.data, f->y.data, ys); memcpy(o->cb.data, f->cb.data, cs); memcpy(o->cr.data, f->cr.data, cs);
    SDL_LockMutex(m->lock);
    m->oq[m->otail % OQ] = w;
    m->otail++;
    SDL_UnlockMutex(m->lock);
    SDL_SemPost(m->osem);
}
// BGRA conversion + guest Receive on core 2 (core 1 only decodes), then EndOfStream at the end of the stream.
static int out_thread(void *arg) {
    Movie *m = arg;
    rt_log("[movie] output thread started\n");
#ifdef __vita__
    sceKernelChangeThreadCpuAffinityMask(sceKernelGetThreadId(), SCE_KERNEL_CPU_MASK_USER_2);
#endif
    while (!m->stop) {
        SDL_SemWaitTimeout(m->osem, 50);
        int done = m->dec_done;
        SDL_LockMutex(m->lock);
        int r = -1;
        if (m->ohead != m->otail) { r = m->oq[m->ohead % OQ]; m->ohead++; }
        m->obusy = r;
        SDL_UnlockMutex(m->lock);
        if (r >= 0) push_frame(m, m->dec_cpu, &m->of[r]);
        SDL_LockMutex(m->lock);
        m->obusy = -1;
        SDL_UnlockMutex(m->lock);
        if (r < 0 && done) {
            if (!m->stop && m->rpin) {
                rt_log("[movie] fin de flux\n");
#ifdef SH3_MOVIE_LAB
                movie_active_eos=m; movie_active_eos_cpu=m->dec_cpu;
#endif
                gcall(m->dec_cpu, m->rpin, 14, 0); // EndOfStream -> the renderer notifies EC_COMPLETE to the graph
#ifdef SH3_MOVIE_LAB
                movie_active_eos=NULL; movie_active_eos_cpu=NULL;
#endif
            }
            m->ended = 1;
            break;
        }
    }
    return 0;
}
static int decode_thread(void *arg) {
    Movie *m = arg;
#ifdef __vita__
    const char *choice = getenv("SH4_MOVIE_CORE");
    int core = choice ? atoi(choice) : 0;
    if (core < 0 || core > 2) core = 0;
    int aff = sceKernelChangeThreadCpuAffinityMask(sceKernelGetThreadId(), SCE_KERNEL_CPU_MASK_USER_0 << core);
    const char *low = getenv("SH4_MOVIE_LOW_PRIORITY");
    int priority = (!low || strcmp(low, "0")) ? SDL_SetThreadPriority(SDL_THREAD_PRIORITY_LOW) : 0;
    rt_log("[movie] decode thread on core %d -> %08x, low priority -> %08x\n", core, aff, priority);
#endif
    uint64_t last = SDL_GetPerformanceCounter(), hz = SDL_GetPerformanceFrequency(), st0 = last;
    while (!m->stop && !plm_has_ended(m->plm)) {
        uint64_t now = SDL_GetPerformanceCounter();
        double dt = (double)(now - last) / (double)hz;
        last = now;
        if (dt > 0.25) dt = 0.25;
        int got = 0;
        if (m->state == 2) {
            g_pending = NULL; g_handed = 0;
            uint64_t d0 = SDL_GetPerformanceCounter();
            g_cb_t = d0;
            plm_decode(m->plm, dt);
            m->st_dec += (double)(SDL_GetPerformanceCounter() - d0) * 1000.0 / (double)hz;
            got = g_handed;
            if (g_pending) { got = 1; push_frame(m, m->dec_cpu, g_pending); }   // no memory for the output buffers: the last frame only
        }
        if (now - st0 > 2 * hz) {
            double sec = (double)(now - st0) / (double)hz;
            rt_log("[movie] t=%.1f s: %.1f frames decoded/s (%.1f B skipped), %.1f delivered/s; per second: decode %.0f ms (video %.0f, audio %.0f, file read %.0f), conversion %.0f ms, Receive %.0f ms\n",
                   plm_get_time(m->plm), m->st_ndec / sec, m->st_nskip / sec, m->st_npush / sec, m->st_dec / sec, g_st_vid / sec, g_st_aud / sec, g_st_io / sec, m->st_conv / sec, m->st_recv / sec);
#ifdef __vita__
            // real CPU time of the thread (runClocks, us) versus elapsed time: tells preemption from raw slowness
            static uint64_t run0; static uint32_t pre0, ipre0;
            SceKernelThreadInfo ti = {.size = sizeof ti};
            if (!sceKernelGetThreadInfo(sceKernelGetThreadId(), &ti)) {
                rt_log("[movie] thread: %.0f ms CPU/s, preemptions %u (interrupts %u), core %d, priority %x\n", (double)(ti.runClocks - run0) / 1000.0 / sec,
                       ti.threadPreemptCount - pre0, ti.intrPreemptCount - ipre0, ti.currentCpuId, ti.currentPriority);
                run0 = ti.runClocks; pre0 = ti.threadPreemptCount; ipre0 = ti.intrPreemptCount;
            }
#endif
            m->st_dec = m->st_conv = m->st_recv = 0; g_st_vid = g_st_aud = g_st_io = 0; m->st_ndec = m->st_npush = m->st_nskip = 0; st0 = now;
        }
        if (!got) SDL_Delay(4);
    }
    m->dec_done = 1;
    SDL_SemPost(m->osem);
    return 0;
}

#if defined(__vita__) && defined(SH3_MOVIE_HW_LAB)
#include "movie_hw_lab.h"
#endif
// ---------- IMediaControl / IMediaPosition / IMediaEvent / IBasicAudio / IMediaEventSink ----------
M(ctl, Run) {
    HOSTFN; MV;
#ifdef SH3_MOVIE_LAB
    if(getenv("SH3_DIAG_MOVIE_RUN_FRAME") && m->plm && m->mem_in && m->renderer) {
        uint32_t hr=gcall(c,m->renderer,6,2,0u,0u);
        rt_log("[movie-lab] Run-frame renderer hr=%08x game_state=%u ready=%u\n",hr,R8(0x72c064u),R32(0x72c0bcu));
        if((int32_t)hr<0) STDRET(1,hr);
        plm_frame_t *frame=NULL;
        for(unsigned k=0;k<=150;k++) { frame=plm_decode_video(m->plm); if(!frame)break; }
        if(!frame) STDRET(1,VFW_E_NOT_FOUND);
        push_frame(m,c,frame);
        m->state=2;
        if(getenv("SH3_DIAG_CAPTURE")) { extern void diag_capture_request(unsigned);diag_capture_request(1); }
        rt_log("[movie-lab] Run-frame delivered one frame, no worker started\n");
        STDRET(1,HR_OK);
    }
    if (!getenv("SH3_DIAG_MOVIE_ALLOW_RUN")) {
        rt_log("[movie-lab] Run refused: playback deliberately disabled\n");
        STDRET(1, VFW_E_NOT_FOUND);
    }
    if ((!m->plm && !MOVIE_MP4_ONLY(m)) || !m->mem_in || !m->renderer) {
        rt_log("[movie-lab] Run refused: source or renderer is not connected\n");
        STDRET(1, VFW_E_NOT_FOUND);
    }
#endif
    if (m->state != 2) {
#if defined(__vita__) && defined(SH3_MOVIE_HW_LAB)
        if(m->hw_active && m->state==1)movie_hw_ctl(sceAvPlayerResume(m->hw_handle),"resume");
#endif
        if (m->renderer) gcall(c, m->renderer, 6, 2, 0u, 0u); // Run(tStart=0)
        m->state = 2;
        if (!m->thread && !m->ended) {
            m->dec_cpu = rt_new_cpu(1u << 20);
            size_t fs = (size_t)((m->w + 15) & ~15) * ((m->h + 15) & ~15) * 3 / 2;
            int ok = 1;
            for (int i = 0; i < OQ; i++) { if (!m->yuv[i]) m->yuv[i] = malloc(fs); if (!m->yuv[i]) ok = 0; }
            if (!ok) { for (int i = 0; i < OQ; i++) { free(m->yuv[i]); m->yuv[i] = NULL; } rt_log("[movie] output buffers: not enough memory\n"); }
            if (!m->osem) m->osem = SDL_CreateSemaphore(0);
            m->ohead = m->otail = 0; m->obusy = -1; m->dec_done = 0;
            m->odropped = 0; s_movie_serial++; m565_begin();
            g_st_vid = g_st_aud = g_st_io = 0;
#if defined(__vita__) && defined(SH3_MOVIE_HW_LAB)
            if(getenv("SH3_DIAG_MOVIE_HW") && m->hw_path[0])movie_hw_open(m);
            m->thread = SDL_CreateThread(m->hw_active ? movie_hw_video_thread : decode_thread, "movie", m);
#else
            m->thread = SDL_CreateThread(decode_thread, "movie", m);
#endif
            m->othread = SDL_CreateThread(out_thread, "movie_out", m);
#if defined(__vita__) && defined(SH3_MOVIE_HW_LAB)
            if(m->hw_active)m->athread=SDL_CreateThread(movie_hw_audio_thread,"movie_audio",m);
            else
#endif
            if (m->aud) m->athread = SDL_CreateThread(audio_thread, "movie_audio", m);
            if (!m->thread || !m->othread || (m->aud && !m->athread)) rt_log("[movie] thread creation failed : dec=%p out=%p aud=%p (%s)\n", (void *)m->thread, (void *)m->othread, (void *)m->athread, SDL_GetError());
#ifdef __vita__
            if (!m->othread) { SceKernelFreeMemorySizeInfo fi = {.size = sizeof fi}; sceKernelGetFreeMemorySize(&fi); rt_log("[movie] free user %d KB\n", fi.size_user >> 10); }
#endif
        }
    }
    STDRET(1, HR_OK);
}
M(ctl, Pause) {
    HOSTFN; MV;
#ifdef SH3_MOVIE_LAB
    if ((!m->plm && !MOVIE_MP4_ONLY(m)) || !m->mem_in || !m->renderer) STDRET(1, VFW_E_NOT_FOUND);
#endif
#if defined(__vita__) && defined(SH3_MOVIE_HW_LAB)
    if(m->hw_active)movie_hw_ctl(sceAvPlayerPause(m->hw_handle),"pause");
#endif
    m->state = 1; if (m->renderer) gcall(c, m->renderer, 5, 0); STDRET(1, HR_OK);
}
M(ctl, Stop) {
    HOSTFN; MV;
    m->stop = 1;
    if (m->thread) { SDL_WaitThread(m->thread, NULL); m->thread = NULL; }
    if (m->othread) { SDL_WaitThread(m->othread, NULL); m->othread = NULL; }
    if (m->athread) { SDL_WaitThread(m->athread, NULL); m->athread = NULL; }
#if defined(__vita__) && defined(SH3_MOVIE_HW_LAB)
    movie_hw_close(m);
#endif
    if (m->state && m->renderer) gcall(c, m->renderer, 4, 0);  // Stop
    m->state = 0;
    g_movie_w = g_movie_h = 0;
    movie_audio_flush();
    if (m->stream) rt_log("[movie-source] requests=%llu disk_reads=%llu disk_kb=%llu cache_hit_kb=%llu io_ms=%.1f output_dropped=%u\n",
                         (unsigned long long)m->stream->requests, (unsigned long long)m->stream->reads,
                         (unsigned long long)(m->stream->read_bytes / 1024),
                         (unsigned long long)(m->stream->hit_bytes / 1024),
                         m->stream->io_ticks * 1000.0 / SDL_GetPerformanceFrequency(), m->odropped);
    // The output/EOF callbacks have ended. Their private x86 CPU owns a 1 MiB
    // guest stack and a TIB; keeping it after Stop leaks one set per movie.
    movie_free_output(m);
    STDRET(1, HR_OK);
}
M(ctl, GetState) { HOSTFN; MV; W32(ARG(3), (uint32_t)m->state); STDRET(3, HR_OK); }
M(ctl, NI) { STDRET(2, E_NOTIMPL_); }
M(ctl, Disp) { STDRET(2, E_NOTIMPL_); }
static const gfn ctl_vt[] = {any_QI, any_AddRef, any_Release, ctl_Disp, ctl_Disp, ctl_Disp, ctl_Disp, ctl_Run,
    ctl_Pause, ctl_Stop, ctl_GetState, ctl_NI, ctl_NI, ctl_NI, ctl_NI, ctl_Pause};

M(pos, Duration) { HOSTFN; MV; W64(ARG(2), rt_u64f(m->plm ? plm_get_duration(m->plm) : MOVIE_MP4_DURATION(m))); STDRET(2, HR_OK); }
// ponytail: audio only follows a rewind to the start outside playback (the only expected case); a real seek would need plm_demux_seek.
M(pos, PutCur) {
    HOSTFN; MV;
    double t = rt_f64(((uint64_t)ARG(3) << 32) | ARG(2));
    rt_log("[movie] PutCurrentPosition(%.2f)\n", t);
    if (m->plm) plm_seek(m->plm, t, 0);
    if (m->aud && !m->athread && t == 0) { plm_demux_rewind(m->adm); plm_audio_rewind(m->aud); movie_audio_flush(); }
    STDRET(3, HR_OK);
}
M(pos, GetCur) { HOSTFN; MV; W64(ARG(2), rt_u64f(m->plm ? plm_get_time(m->plm) : 0)); STDRET(2, HR_OK); }
M(pos, Gen2) { STDRET(2, HR_OK); }
M(pos, Gen3) { STDRET(3, HR_OK); }
static const gfn pos_vt[] = {any_QI, any_AddRef, any_Release, ctl_Disp, ctl_Disp, ctl_Disp, ctl_Disp, pos_Duration,
    pos_PutCur, pos_GetCur, pos_Gen2, pos_Gen3, pos_Gen2, pos_Gen3, pos_Gen3, pos_Gen2, pos_Gen2, pos_Gen2};

M(ev, GetHandle) { HOSTFN; W32(ARG(2), 0); STDRET(2, HR_OK); }
M(ev, GetEvent) {
    HOSTFN; MV;
    uint32_t hr = VFW_E_TIMEOUT;
    SDL_LockMutex(m->lock);
    if (m->nev) {
        W32(ARG(2), m->ev[0].code); W32(ARG(3), m->ev[0].p1); W32(ARG(4), m->ev[0].p2);
        memmove(&m->ev[0], &m->ev[1], (size_t)(--m->nev) * sizeof m->ev[0]);
        hr = HR_OK;
    }
    SDL_UnlockMutex(m->lock);
    STDRET(5, hr);
}
M(ev, WaitComp) { HOSTFN; W32(ARG(3), 0); STDRET(3, VFW_E_TIMEOUT); }
M(ev, Free) { STDRET(4, HR_OK); }
/* IMediaEventEx is returned for IID 56a868c0. Window message delivery is
   not implemented in the diagnostic runtime, so reject requests explicitly. */
M(ev, SetNotifyWindow) {
    HOSTFN;
    if (!ARG(2)) {
#ifdef SH3_MOVIE_LAB
        MV; SDL_LockMutex(m->lock); m->notify_hwnd=m->notify_message=m->notify_instance=0; SDL_UnlockMutex(m->lock);
#endif
        rt_log("[movie] SetNotifyWindow disabled\n"); STDRET(4, HR_OK);
    }
#ifdef SH3_MOVIE_LAB
    if (getenv("SH3_DIAG_MSG_QUEUE") && ARG(2)==0x10010 && ARG(3)) {
        MV; SDL_LockMutex(m->lock); m->notify_hwnd=ARG(2); m->notify_message=ARG(3); m->notify_instance=ARG(4); SDL_UnlockMutex(m->lock);
        rt_log("[movie-lab] SetNotifyWindow registered hwnd=%08x msg=%08x\n",ARG(2),ARG(3)); STDRET(4, HR_OK);
    }
#endif
    rt_log("[movie] SetNotifyWindow hwnd=%08x msg=%08x instance=%08x unsupported\n",ARG(2),ARG(3),ARG(4));
    STDRET(4, E_NOTIMPL_);
}
M(ev, SetNotifyFlags) { STDRET(2, E_NOTIMPL_); }
M(ev, GetNotifyFlags) { STDRET(2, E_NOTIMPL_); }
static const gfn ev_vt[] = {any_QI, any_AddRef, any_Release, ctl_Disp, ctl_Disp, ctl_Disp, ctl_Disp, ev_GetHandle,
    ev_GetEvent, ev_WaitComp, pos_Gen2, pos_Gen2, ev_Free, ev_SetNotifyWindow, ev_SetNotifyFlags, ev_GetNotifyFlags};

M(au, PutVol) {
    HOSTFN; MV;
    m->vol = (int32_t)ARG(2);
    g_audio_gain_q8 = m->vol <= -10000 ? 0 : (int)(256.0f * powf(10.0f, (float)m->vol / 2000.0f));
    STDRET(2, HR_OK);
}
M(au, GetVol) { HOSTFN; MV; W32(ARG(2), (uint32_t)m->vol); STDRET(2, HR_OK); }
static const gfn au_vt[] = {any_QI, any_AddRef, any_Release, ctl_Disp, ctl_Disp, ctl_Disp, ctl_Disp, au_PutVol,
    au_GetVol, pos_Gen2, au_GetVol};

M(sink, Notify) { HOSTFN; rt_log("[movie] event %u (%08x, %08x)\n", ARG(2), ARG(3), ARG(4)); rpush(c, ARG(2), ARG(3), ARG(4)); STDRET(4, HR_OK); }
static const gfn sink_vt[] = {any_QI, any_AddRef, any_Release, sink_Notify};

// ---------- IGraphBuilder ----------
static uint32_t guid_d1(CPU *c, uint32_t p) { HOSTFN; return R32(p); }

M(gr, QI) {
    HOSTFN; MV;
    uint32_t d1 = guid_d1(c, ARG(2)), out = ARG(3), r = 0;
    switch (d1) {
    case 0x56a868b1: r = m->ctl_g; break;
    case 0x56a868b2: r = m->pos_g; break;
    case 0x56a868b6: case 0x56a868c0: r = m->ev_g; break;
    case 0x56a868b3: r = m->aud_g; break;
    case 0x56a868a2: r = m->sink_g; break;
    case 0x56a868a9: case 0x56a8689f: case 0: r = m->graph_g; break;
    default: rt_log("[movie] graph: unsupported QueryInterface IID %08x\n", d1); break;
    }
    W32(out, r);
    STDRET(3, r ? HR_OK : E_NOINTERFACE_);
}
M(gr, AddFilter) {
    HOSTFN; MV;
    uint32_t f = ARG(2);
    if (!f) STDRET(3, HR_OK);
#ifdef SH3_MOVIE_LAB
    if (getenv("SH3_DIAG_MOVIE_TRACE_FILTERS") && f<=g_mem_size-4) {
        uint32_t vt=R32(f);
        if (vt && vt<=g_mem_size-16*4) {
            rt_log("[movie-lab] guest filter=%08x vtable=%08x methods",f,vt);
            for (unsigned i=0;i<16;i++) rt_log(" %08x",R32(vt+4*i));
            rt_log("\n");
        }
    }
#endif
    if (!com_get(f)) { // guest filter (the renderer): it joins the graph
#ifdef SH3_MOVIE_LAB
        /* Experimental lifetime trace: one candidate at a time, no change to ownership. */
        extern void diag_movie_watch_guest_filter(uint32_t);
        if (getenv("SH3_DIAG_MOVIE_TRACE_FILTERS")) diag_movie_watch_guest_filter(f);
        if (getenv("SH3_DIAG_MOVIE_RETAIN_FILTERS")) {
            int inserted=diag_filter_insert(&m->retained_filters,f);
            if(inserted==1) STDRET(3,0x8004022du);
            if(inserted) STDRET(3,0x8007000eu);
            uint32_t refs=gcall(c,f,1,0);
            rt_log("[movie-lab] retained filter=%08x refs=%u\n",f,refs);
        }
#endif
#ifdef SH3_MOVIE_LAB
        /* This PE's first filter is constructed with three video callbacks;
           the second wraps the file reader. This is a diagnostic hypothesis,
           not a general DirectShow filter classifier. */
        if (getenv("SH3_DIAG_MOVIE_GUEST_RENDERER")) {
            uint32_t vt=f<=g_mem_size-4?R32(f):0;
            if (vt==0x0068a960u) {
                m->renderer=f;
                rt_log("[movie-lab] candidate renderer=%08x\n",f);
            } else rt_log("[movie-lab] guest filter %08x is not renderer candidate (vt=%08x)\n",f,vt);
        } else
#endif
        m->renderer = f;
        gcall(c, f, 13, 2, m->graph_g, ARG(3));
        rt_log("[movie] guest filter %08x added to the graph\n", f);
#ifdef SH3_MOVIE_LAB
        if (getenv("SH3_DIAG_MOVIE_RETAIN_FILTERS") && getenv("SH3_DIAG_MOVIE_TEST_REMOVE")) {
            uint32_t hr=gcall(c,m->graph_g,4,1,f);
            rt_log("[movie-lab] immediate test RemoveFilter %08x -> %08x\n",f,hr);
        }
#endif
    }
    STDRET(3, HR_OK);
}
M(gr, Connect) {
    HOSTFN; MV;
    uint32_t out = ARG(2), in = ARG(3);
    HObj *po = (HObj *)com_get(out), *pi = (HObj *)com_get(in);
#ifdef SH3_MOVIE_LAB
    if (getenv("SH3_DIAG_MOVIE_TRACE_FILTERS")) {
        uint32_t ov=out && out<=g_mem_size-4 ? R32(out) : 0;
        uint32_t iv=in && in<=g_mem_size-4 ? R32(in) : 0;
        rt_log("[movie-lab] Connect out=%08x vt=%08x host=%d in=%08x vt=%08x host=%d\n",
               out,ov,po!=NULL,in,iv,pi!=NULL);
    }
#endif
    if (po && pi) { po->peer = in; pi->peer = out; STDRET(3, HR_OK); }
    if (!po || pi) STDRET(3, 0x80040217u);
    // host output pin (video codec) -> guest input pin (renderer)
    uint32_t tmp = guest_tmp();
    uint8_t g_vid[8] = {0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71}, g_rgb[8] = {0x9F, 0x53, 0x00, 0x20, 0xAF, 0x0B, 0xA7, 0x70}, /* RGB32 */
            g_vi[8] = {0xBF, 0x01, 0x00, 0xAA, 0x00, 0x55, 0x59, 0x5A};
    uint32_t mt = tmp, fmt = tmp + 80, w = (uint32_t)m->w, h = (uint32_t)m->h;
    memset(mb + tmp, 0, 256);
    put_guid(c, mt, 0x73646976, 0x0000, 0x0010, g_vid);
    put_guid(c, mt + 16, 0xe436eb7e, 0x524f, 0x11ce, g_rgb); // MEDIASUBTYPE_RGB32: the only one the game's renderer accepts
    W32(mt + 32, 1); W32(mt + 40, w * h * 4);
    put_guid(c, mt + 44, 0x05589f80, 0xc356, 0x11ce, g_vi);
    W32(mt + 64, 88); W32(mt + 68, fmt);
    W32(fmt + 16, w); W32(fmt + 20, h); W32(fmt + 24, w); W32(fmt + 28, h);        // rcSource/rcTarget
    W32(fmt + 40, 333667); W32(fmt + 44, 0);                                          // AvgTimePerFrame
    W32(fmt + 48, 40); W32(fmt + 52, w);
#ifdef SH3_MOVIE_LAB
    W32(fmt + 56, getenv("SH3_DIAG_MOVIE_TOPDOWN") ? (uint32_t)(-(int32_t)h) : h);
#else
    W32(fmt + 56, h);
#endif
    W16(fmt + 60, 1); W16(fmt + 62, 32); W32(fmt + 68, w * h * 4);
    uint32_t hr = gcall(c, in, 4, 2, out, mt); // IPin::ReceiveConnection
    rt_log("[movie] ReceiveConnection -> %08x\n", hr);
    if ((int32_t)hr < 0) STDRET(3, hr);
    m->rpin = in;
    uint32_t pm = tmp + 160;
    W32(pm, 0);
    uint8_t g_mi[8] = {0xb0, 0x3a, 0x00, 0x20, 0xaf, 0x0b, 0xa7, 0x70};
    put_guid(c, tmp + 176, 0x56a8689d, 0x0ad4, 0x11ce, g_mi);
    hr = gcall(c, in, 0, 2, tmp + 176, pm);   // QueryInterface(IID_IMemInputPin)
    m->mem_in = R32(pm);
    if ((int32_t)hr < 0 || !m->mem_in) STDRET(3, hr ? hr : E_NOINTERFACE_);
    W32(pm, 0);
    hr = gcall(c, m->mem_in, 3, 1, pm);       // GetAllocator (CoCreateInstance(CLSID_MemoryAllocator) -> host)
    m->alloc = R32(pm);
    rt_log("[movie] GetAllocator -> %08x (%08x)\n", hr, m->alloc);
    if ((int32_t)hr < 0 || !m->alloc) STDRET(3, hr ? hr : HR_FAIL);
    uint32_t req = tmp + 192;
    W32(req, 2); W32(req + 4, w * h * 4); W32(req + 8, 1); W32(req + 12, 0);
    hr = gcall(c, m->alloc, 3, 2, req, req + 16);  // SetProperties
    rt_log("[movie] SetProperties -> %08x\n", hr);
    hr = gcall(c, m->mem_in, 4, 2, m->alloc, 0u);  // NotifyAllocator
    rt_log("[movie] NotifyAllocator -> %08x\n", hr);
    po->peer = in;
    STDRET(3, hr);
}
#if defined(__vita__) && defined(SH3_MOVIE_HW_LAB)
/* The seven films of the supported sh3.exe data, as pl_mpeg reads them from the decrypted .mpg (all 512x384 with
   sound). With only the .mp4 present the hardware decoder plays the film and these values stand in for the .mpg. */
static int movie_mp4_meta(const char *host, Movie *m) {
    static const struct { const char *name; double duration; } films[] = {
        {"op", 192.09189999999998}, {"muxx", 13.133333333333333}, {"huxx", 17.951266666666669},
        {"henshin", 24.691322222222219}, {"ed_n", 300.93396666666666}, {"ed_u", 342.07505555555554},
        {"ed_b", 300.16653333333335}};
    const char *base = strrchr(host, '/'); base = base ? base + 1 : host;
    size_t n = strlen(base);
    if (n < 5 || strcmp(base + n - 4, ".mp4")) return 0;
    for (unsigned i = 0; i < sizeof films / sizeof films[0]; i++)
        if (strlen(films[i].name) == n - 4 && !strncmp(base, films[i].name, n - 4)) {
            m->w = 512; m->h = 384; m->hw_duration = films[i].duration;
            return 1;
        }
    return 0;
}
#endif
static int movie_open_source(Movie *m,const char *host) {
#if defined(__vita__) && defined(SH3_MOVIE_HW_LAB)
    m->hw_path[0]=0; m->hw_duration=0;
    if(getenv("SH3_DIAG_MOVIE_HW") && movie_mp4_meta(host,m)) {
        size_t n=strlen(host);
        if(n<sizeof m->hw_path) {
            memcpy(m->hw_path,host,n+1);
            g_movie_w=m->w; g_movie_h=m->h;
            rt_log("[movie-hw] %s played directly (no .mpg), %dx%d, %.1f s\n",host,m->w,m->h,m->hw_duration);
            return 1;
        }
    }
    if(getenv("SH3_DIAG_MOVIE_HW")) {
        /* Per movie: the hardware path only when its .mp4 sits next to the allowlisted .mpg; otherwise the
           software decoder plays it (no fatal stop in the playable build). */
        size_t n=strlen(host);
        if(n>=4 && n<sizeof m->hw_path && !strcmp(host+n-4,".mpg")) {
            memcpy(m->hw_path,host,n+1);memcpy(m->hw_path+n-4,".mp4",5);
            FILE *f=fopen(m->hw_path,"rb");
            if(f)fclose(f);else m->hw_path[0]=0;
        }
        rt_log("[movie-hw] %s -> %s\n",host,m->hw_path[0]?m->hw_path:"software decoder (no .mp4)");
    }
#endif
    const char *cache = getenv("SH4_MOVIE_CACHE"), *all = getenv("SH4_MOVIE_ALL_FRAMES");
    int use_cache = cache && !strcmp(cache, "1");
#ifdef __vita__
    if (!cache) use_cache = 1;
#endif
    if (use_cache) m->stream = movie_stream_open(host);
    // Allocation failure retains the old streaming/catch-up path. An explicit
    // flag can still force either policy for paired laboratory comparisons.
    g_movie_all_frames = all ? !strcmp(all, "1") : m->stream != NULL;
    rt_log("[movie-mode] shared_cache=%d all_frames=%d\n", m->stream != NULL, g_movie_all_frames);
    plm_buffer_t *fb = movie_input(m, host, 0);
    m->plm = fb ? plm_create_with_buffer(fb, TRUE) : NULL;
    rt_log("[movie] source \"%s\" -> %s\n", host, m->plm ? "ok" : "FAILED");
    if (!m->plm) return 0;
    plm_set_loop(m->plm, 0);
    plm_set_audio_enabled(m->plm, 0);
    plm_set_video_decode_callback(m->plm, on_video, m);
    if (plm_get_num_audio_streams(m->plm) && (fb = movie_input(m, host, 1))) {
        m->adm = plm_demux_create(fb, TRUE);
        plm_buffer_t *ab = plm_buffer_create_with_capacity(PLM_BUFFER_DEFAULT_SIZE);
        plm_buffer_set_load_callback(ab, apkt_load, m->adm);
        m->aud = plm_audio_create_with_buffer(ab, TRUE);
    }
    m->w = plm_get_width(m->plm);
    m->h = plm_get_height(m->plm);
    g_movie_w = m->w; g_movie_h = m->h;
    rt_log("[movie] %dx%d, %.2f i/s, %.1f s, audio=%d\n", m->w, m->h, plm_get_framerate(m->plm), plm_get_duration(m->plm),
           plm_get_num_audio_streams(m->plm));
    return 1;
}
M(gr, AddSourceFilter) {
    HOSTFN; MV;
    char path[600], host[700];
    uint32_t w = ARG(2);
    size_t i = 0;
    for (; i < sizeof path - 1 && R16(w + 2 * i); i++) path[i] = (char)R16(w + 2 * i);
    path[i] = 0;
    w32_host_path(path, host, sizeof host);
    if (!movie_open_source(m,host)) { W32(ARG(4),0); STDRET(4,VFW_E_NOT_FOUND); }
    W32(ARG(4), m->filters[F_SRC]->com.guest);
    STDRET(4, HR_OK);
}
M(gr, Ok2) { STDRET(2, HR_OK); }
M(gr, RemoveFilter) {
    HOSTFN; MV;
#ifdef SH3_MOVIE_LAB
    uint32_t f=ARG(2);
    if(diag_filter_remove(&m->retained_filters,f)==1) {
        if(m->renderer==f) m->renderer=0;
        gcall(c,f,13,2,0u,0u); gcall(c,f,2,0); STDRET(2,HR_OK);
    }
#endif
    (void)m; STDRET(2, E_NOTIMPL_);
}
M(gr, ConnectDirect) {
    HOSTFN;
#ifdef SH3_MOVIE_LAB
    if (getenv("SH3_DIAG_MOVIE_TRACE_FILTERS")) {
        uint32_t out=ARG(2),in=ARG(3);
        uint32_t ov=out && out<=g_mem_size-4 ? R32(out) : 0;
        uint32_t iv=in && in<=g_mem_size-4 ? R32(in) : 0;
        rt_log("[movie-lab] ConnectDirect out=%08x vt=%08x host=%d in=%08x vt=%08x host=%d media=%08x\n",
               out,ov,com_get(out)!=NULL,in,iv,com_get(in)!=NULL,ARG(4));
    }
    if (!getenv("SH3_DIAG_MOVIE_ASSUME_RENDER")) STDRET(4, E_NOTIMPL_);
#endif
    STDRET(4, HR_OK);
}
M(gr, Render) {
    HOSTFN;
#ifdef SH3_MOVIE_LAB
    int connected=0;
    if(getenv("SH3_DIAG_MOVIE_PREP_SOURCE")) {
        MV;
        if(!m->plm && !MOVIE_MP4_ONLY(m) && m->path[0]) {
            int ok=movie_open_source(m,m->path);
            rt_log("[movie-lab] PC movie source prepared=%d (playback remains disabled)\n",ok);
        }
    }
    if (getenv("SH3_DIAG_MOVIE_TRACE_FILTERS")) {
        uint32_t pin=ARG(2),vt=pin && pin<=g_mem_size-4 ? R32(pin) : 0;
        uint32_t dir=pin && pin<=g_mem_size-20 ? R32(pin+16) : 0xffffffffu;
        rt_log("[movie-lab] Render pin=%08x vt=%08x host=%d direction_field=%08x\n",
               pin,vt,com_get(pin)!=NULL,dir);
    }
    if(getenv("SH3_DIAG_MOVIE_PROBE_PINS")) {
        MV;
        if(m->renderer) {
            uint32_t tmp=gheap_calloc(16);
            if(!tmp) rt_fatal("[movie-lab] pin probe allocation failed");
            uint32_t hr=gcall(c,m->renderer,10,1,tmp);
            uint32_t en=R32(tmp);
            rt_log("[movie-lab] renderer EnumPins hr=%08x enum=%08x\n",hr,en);
            if((int32_t)hr>=0 && en) {
                for(unsigned i=0;i<16;i++) {
                    W32(tmp+4,0); W32(tmp+8,0); W32(tmp+12,0xffffffffu);
                    hr=gcall(c,en,3,3,1u,tmp+4,tmp+8);
                    uint32_t pin=R32(tmp+4), fetched=R32(tmp+8);
                    if(hr || fetched!=1 || !pin) break;
                    uint32_t direction_hr=gcall(c,pin,9,1,tmp+12);
                    rt_log("[movie-lab] renderer pin=%08x direction=%08x hr=%08x\n",pin,R32(tmp+12),direction_hr);
                    if(getenv("SH3_DIAG_MOVIE_TEST_CONNECT") && !direction_hr && R32(tmp+12)==0 && (m->plm || MOVIE_MP4_ONLY(m)) && !m->mem_in) {
                        HObj *out=NULL;
                        for(unsigned j=0;j<10;j++) if(m->pins[j] && m->pins[j]->role==F_VCODEC && m->pins[j]->dir==1) { out=m->pins[j];break; }
                        if(out) {
                            uint32_t connect_hr=gcall(c,m->graph_g,11,2,out->com.guest,pin);
                            rt_log("[movie-lab] test connect output=%08x input=%08x hr=%08x mem_in=%08x\n",out->com.guest,pin,connect_hr,m->mem_in);
                            if(!connect_hr && m->mem_in) connected=1;
                            if(connected && getenv("SH3_DIAG_MOVIE_ONE_FRAME")) {
                                if(getenv("SH3_DIAG_MOVIE_INIT_GUEST")) {
                                    extern uint32_t rt_guest_call(CPU *,uint32_t,int,...);
                                    uint32_t result=rt_guest_call(c,0x004040e0u,0),ready=R32(0x72c0bcu);
                                    rt_log("[movie-lab] guest movie surface init result=%08x ready=%08x\n",result,ready);
                                }
                                uint32_t run_hr=gcall(c,m->renderer,6,2,0u,0u);
                                rt_log("[movie-lab] single-frame renderer Run hr=%08x\n",run_hr);
                                if(!(int32_t)run_hr) {
                                    unsigned frame_index=0;
                                    const char *setting=getenv("SH3_DIAG_MOVIE_FRAME_INDEX");
                                    if(setting) {
                                        char *end=NULL; unsigned long value=strtoul(setting,&end,10);
                                        if(end && !*end && value<=300) frame_index=(unsigned)value;
                                    }
                                    plm_frame_t *frame=NULL;
                                    for(unsigned k=0;k<=frame_index;k++) { frame=plm_decode_video(m->plm);if(!frame)break; }
                                    rt_log("[movie-lab] single-frame decode %s\n",frame?"ok":"failed");
                                    if(frame) {
                                        push_frame(m,c,frame);
                                        if(getenv("SH3_DIAG_MOVIE_DRAW_PROBE")) {
                                            extern uint32_t rt_guest_call(CPU *,uint32_t,int,...);
                                            rt_log("[movie-lab] guest draw probe state=%u ready=%u\n",R8(0x72c064u),R32(0x72c0bcu));
                                            uint32_t draw_result=rt_guest_call(c,0x00404440u,0);
                                            rt_log("[movie-lab] guest draw probe returned=%08x state=%u\n",draw_result,R8(0x72c064u));
                                        }
                                        if(getenv("SH3_DIAG_CAPTURE")) {
                                            extern void diag_capture_request(unsigned);
                                            diag_capture_request(1);
                                            rt_log("[movie-lab] requested next presentation capture after single-frame Receive\n");
                                        }
                                    }
                                    if(!getenv("SH3_DIAG_MOVIE_HOLD_FRAME")) {
                                        uint32_t stop_hr=gcall(c,m->renderer,4,0);
                                        rt_log("[movie-lab] single-frame renderer Stop hr=%08x\n",stop_hr);
                                    } else rt_log("[movie-lab] renderer left running for capture experiment\n");
                                }
                            }
                        }
                    }
                    gcall(c,pin,2,0);
                }
                gcall(c,en,2,0);
            }
            gheap_free(tmp);
        }
    }
    if (connected) STDRET(2,HR_OK);
    if (!getenv("SH3_DIAG_MOVIE_ASSUME_RENDER")) STDRET(2, E_NOTIMPL_);
#endif
    STDRET(2, HR_OK);
}
M(gr, NI3) { STDRET(3, E_NOTIMPL_); }
static const gfn gr_vt[] = {gr_QI, any_AddRef, any_Release, gr_AddFilter, gr_RemoveFilter, gr_NI3 /*EnumFilters*/,
    gr_NI3 /*FindFilterByName*/, gr_ConnectDirect, gr_Ok2 /*Reconnect*/, gr_Ok2 /*Disconnect*/,
    gr_Ok2 /*SetDefaultSyncSource*/, gr_Connect, gr_Render, gr_NI3 /*RenderFile*/, gr_AddSourceFilter,
    gr_Ok2 /*SetLogFile*/, gr_Ok2 /*Abort*/, gr_Ok2 /*ShouldOperationContinue*/};

// ---------- entry points for CoCreateInstance ----------
static Movie *movie_new(void) {
    Movie *m = calloc(1, sizeof *m);
    if (!m) rt_fatal("[movie] graph allocation failed");
    atomic_init(&m->stop,0); atomic_init(&m->ended,0);
    atomic_init(&m->dec_done,0); atomic_init(&m->state,0);
    m->lock = SDL_CreateMutex();
    if (!m->lock) { free(m); rt_fatal("[movie] graph mutex allocation failed"); }
    m->vol = -10000;
    m->graph_g = mkobj(m, MK_GRAPH, gr_vt, 18, "IGraphBuilder", &m->graph);
    m->ctl_g = mkobj(m, MK_CTL, ctl_vt, 16, "IMediaControl", NULL);
    m->pos_g = mkobj(m, MK_POS, pos_vt, 18, "IMediaPosition", NULL);
    m->ev_g = mkobj(m, MK_EV, ev_vt, 16, "IMediaEventEx", NULL);
    m->aud_g = mkobj(m, MK_AUD, au_vt, 11, "IBasicAudio", NULL);
    m->sink_g = mkobj(m, MK_SINK, sink_vt, 4, "IMediaEventSink", NULL);
    const char *none[2] = {0, 0}, *vs[2] = {"Audio", "Video"}, *one[2] = {"Output", 0};
    make_filter(m, F_SRC, 0, 1, NULL, one);
    make_filter(m, F_SPLIT, 1, 2, "Input", vs);
    make_filter(m, F_ACODEC, 1, 1, "Input", one);
    make_filter(m, F_VCODEC, 1, 1, "Input", one);
    make_filter(m, F_DSR, 1, 0, "Input", none);
    return m;
}

// Returns 1 if the CLSID is handled (object written to *out), 0 otherwise.
int movie_cocreate(CPU *c, uint32_t clsid, uint32_t riid, uint32_t out, uint32_t *hr) {
    HOSTFN;
    (void)riid;
    uint32_t d1 = R32(clsid);
    int which = d1 == 0x336475d0 ? F_SPLIT : d1 == 0x4a2286e0 ? F_ACODEC : d1 == 0xfeb50740 ? F_VCODEC : d1 == 0x79376820 ? F_DSR : -1;
    if (d1 == 0xe436ebb3) { // CLSID_FilterGraph : nouveau film
        if (g_mv) {                                             // movie finished without Stop: without a join, dead threads keep their stack and creating the next one fails
            g_mv->stop = 1;
            if (g_mv->thread) { SDL_WaitThread(g_mv->thread, NULL); g_mv->thread = NULL; }
            if (g_mv->othread) { SDL_WaitThread(g_mv->othread, NULL); g_mv->othread = NULL; }
            if (g_mv->athread) { SDL_WaitThread(g_mv->athread, NULL); g_mv->athread = NULL; }
            movie_free_decoders(g_mv);
#ifdef SH3_MOVIE_LAB
            while(g_mv->retained_filters.count) {
                uint32_t f=g_mv->retained_filters.items[g_mv->retained_filters.count-1];
                diag_filter_remove(&g_mv->retained_filters,f);
                gcall(c,f,13,2,0u,0u); gcall(c,f,2,0);
                rt_log("[movie-lab] released prior graph filter=%08x\n",f);
            }
            g_mv->renderer=0;
#endif
        }
        // ponytail: the rest of the previous movie (COM objects, ~1 KB) leaks
        g_mv = movie_new();
        W32(out, g_mv->graph_g);
        *hr = HR_OK;
        return 1;
    }
    if (d1 == 0x1e651cc0 && g_mv) { // CLSID_MemoryAllocator
        if (!g_mv->alloc) g_mv->alloc = mkobj(g_mv, MK_ALLOC, al_vt, 9, "IMemAllocator", NULL);
        W32(out, g_mv->alloc);
        *hr = HR_OK;
        return 1;
    }
    if (which < 0 || !g_mv) return 0;
    W32(out, g_mv->filters[which]->com.guest);
    *hr = HR_OK;
    return 1;
}
