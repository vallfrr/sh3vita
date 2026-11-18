/* Isolated, fail-closed host diagnostic: never a playable runtime. Generic x87
 * helpers adapted from sh4vita/port/src/rt/x86rt.c, GPL-3.0 (see LICENSE). */
#include "x86rt.h"
#include "pe_image.h"
#include "diag_heap.h"
#include "diag_taskmem.h"
#include "diag_sync.h"
#include "diag_thread.h"
#include "diag_event.h"
#include "diag_fileio.h"
#ifdef SH3_MOVIE_LAB
#include "diag_message.h"
#include "diag_multiwait.h"
#endif
#include <SDL.h>
#ifdef _WIN32
#define SH3_MKDIR(p,m) _mkdir(p)
#define SH3_GMTOFF(t,sec) ((long)(_mkgmtime(&(t))-(sec)))
#else
#define SH3_MKDIR(p,m) mkdir(p,m)
#define SH3_GMTOFF(t,sec) ((t).tm_gmtoff)
#endif
#include <errno.h>
#include <dirent.h>
#include <fcntl.h>
#include <math.h>
#include <fenv.h>
#include "diag_fist_bits.h"
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>
#ifdef __vita__
#include "vita_paths.h"
#include <psp2/io/stat.h>
#include <psp2/kernel/sysmem.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/modulemgr.h>
#include <psp2/power.h>
#include <psp2/ctrl.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/kernel/cpu.h>
#include <psp2/rtc.h>
static int diag_save_fixture;
#define DIAG_SAVE_SOURCE (diag_save_fixture ? "ux0:data/sh3/diag/save-fixture" : "ux0:data/sh3/savedata")
#define DIAG_SAVE_SCRATCH DIAG_SAVE_SOURCE
#define DIAG_SAVE_READ_FILE(name) (diag_save_fixture ? "ux0:data/sh3/diag/save-fixture/" name : "ux0:data/sh3/savedata/" name)
#define DIAG_SAVE_WRITE_FILE(name) DIAG_SAVE_READ_FILE(name)
#define DIAG_ROOT_FILE (diag_save_fixture ? "ux0:data/sh3/diag/save-fixture/root-data.sys" : "ux0:data/sh3/data.sys")
#define DIAG_ROOT_DIR (diag_save_fixture ? "ux0:data/sh3/diag/save-fixture" : "ux0:data/sh3")
#else
#define DIAG_SAVE_SOURCE "game/savedata"
#define DIAG_SAVE_SCRATCH "build/pc-diag/savedata"
#define DIAG_ROOT_FILE "game/data.sys"
#define DIAG_ROOT_DIR "game"
#define DIAG_SAVE_READ_FILE(name) DIAG_SAVE_SOURCE "/" name
#define DIAG_SAVE_WRITE_FILE(name) DIAG_SAVE_SCRATCH "/" name
#endif
#ifdef __vita__
int _newlib_heap_size_user=24*1024*1024;
unsigned int sceUserMainThreadStackSize=4*1024*1024;
#endif

uint8_t *g_mem;
#ifndef SH3_GUEST_MEMORY_MB
#define SH3_GUEST_MEMORY_MB 256
#endif
#if SH3_GUEST_MEMORY_MB < 200 || SH3_GUEST_MEMORY_MB > 256
#error "SH3 guest memory must remain within the measured 200..256 MiB budget"
#endif
#define DIAG_MEM_SIZE (SH3_GUEST_MEMORY_MB * 1024u * 1024u)
uint32_t g_mem_size=DIAG_MEM_SIZE;
extern const struct { uint32_t a; gfn f; } x86_functab[];
extern const unsigned x86_functab_n;
extern const uint32_t x86_entry;
extern const struct { uint32_t slot; gfn f; const char *name; } x86_iat[];
extern const unsigned x86_iat_n;
#define DIAG_IMPORT_BASE 0xf0000000u
#define DIAG_HOST_BASE 0xf0100000u
#define DIAG_HEAP_HANDLE 0xa00000u
/* Adapted from sh4vita: one slot per guest CPU and return site. Recursive
 * reuse of the same site and a jump after its C frame returns are unsupported. */
typedef struct DiagJmpSlot { CPU *cpu; uint32_t ret; jmp_buf jb; struct DiagJmpSlot *next; } DiagJmpSlot;
static DiagJmpSlot *diag_jumps;
static struct { gfn f; const char *name; unsigned char d3d; } diag_hosts[8192];
static unsigned diag_host_count;
static pthread_mutex_t diag_host_lock=PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t diag_jump_lock=PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t diag_object_lock=PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t diag_com_lock;
static pthread_once_t diag_com_once=PTHREAD_ONCE_INIT;
/* Optional low-rate timing of outermost COM lock acquisitions; sums protected
   by the same recursive lock. The held interval includes the host method. */
static int diag_com_profile;
static __thread unsigned diag_com_depth, diag_com_sequence;
static uint64_t diag_com_wait_ticks, diag_com_hold_ticks, diag_com_wait_max;
static unsigned diag_com_samples, diag_com_worker_samples;
static void diag_init_com_lock(void) {
    pthread_mutexattr_t attr;
    if (pthread_mutexattr_init(&attr) || pthread_mutexattr_settype(&attr,PTHREAD_MUTEX_RECURSIVE) ||
        pthread_mutex_init(&diag_com_lock,&attr)) { fprintf(stderr,"[diag] COM lock init failed\n"); exit(36); }
    pthread_mutexattr_destroy(&attr);
}
static pthread_mutex_t diag_critical[512];
static unsigned char diag_critical_live[512];
static unsigned diag_critical_count;
static pthread_mutex_t diag_mutexes[128];
static unsigned diag_mutex_count;
static DiagSem *diag_semaphores[128];
static unsigned diag_semaphore_count;
#define DIAG_MMIO_LIMIT 4096u
typedef struct { uint32_t base, size, pos; } DiagMmio;
static DiagMmio diag_mmio[DIAG_MMIO_LIMIT];
static unsigned diag_mmio_count;
typedef struct { int fd; uint32_t size; int active; } DiagMapping;
static DiagMapping diag_mappings[128];
static uint32_t diag_mapping_views[128];
static unsigned diag_mapping_count;
typedef struct { CPU cpu; uint32_t stack, tib, start, arg, tid; int32_t priority; DiagThread *thread; } DiagGuestThread;
static DiagGuestThread *diag_threads[16];
static unsigned diag_thread_count;
static CPU *diag_main_cpu;
/* Performance options, marker perf=<comma list> (docs/AUDIT-PERFORMANCE-PC.md section 12). Without the marker the
   compiled default applies; "perf=none" restores the previous code paths for a paired control run. */
enum { PERF_FASTCOM=1, PERF_LOOKUP=2, PERF_LOGASYNC=4, PERF_NOVSYNC=8, PERF_FILECACHE=16, PERF_AUDIOCACHE=32, PERF_PREPTHREAD=64, PERF_LATEPACE=128, PERF_MATRIX=256, PERF_WIDECACHE=1024, PERF_GEOMO2=2048, PERF_FTOL=4096, PERF_PASSAUDIT=8192, PERF_PRESENTCLEAR=16384, PERF_GXMSCENES=32768, PERF_PACEPROBE=65536, PERF_FISTBITS=131072, PERF_FXHALF=262144, PERF_FLOORINLINE=524288, PERF_GLSTATECACHE=1048576, PERF_LENSASYNC=2097152, PERF_KEEPCOLOR=8388608, PERF_FXQUARTER=16777216 };
#ifdef SH3_MATRIX_LAB
#define DIAG_MATRIX_DEFAULT PERF_MATRIX
#else
#define DIAG_MATRIX_DEFAULT 0
#endif
#ifdef SH3_PLAYTEST_PROFILE
#define DIAG_PLAYTEST_PERF (PERF_FISTBITS|PERF_FXHALF|PERF_FXQUARTER|PERF_GLSTATECACHE|PERF_LENSASYNC)
#else
#define DIAG_PLAYTEST_PERF 0u
#endif
#define DIAG_PERF_DEFAULT (PERF_FASTCOM|PERF_LOOKUP|PERF_LOGASYNC|PERF_NOVSYNC|PERF_FILECACHE|PERF_AUDIOCACHE|PERF_PREPTHREAD|PERF_LATEPACE|PERF_WIDECACHE|DIAG_MATRIX_DEFAULT|DIAG_PLAYTEST_PERF)
unsigned g_diag_perf=DIAG_PERF_DEFAULT;
/* Initialized before guest threads start; immutable during a run. */
int g_diag_native_clip;
#ifdef SH3_PC24_VEC3_LAB
int g_diag_pc24_vec3=1;
#endif
/* Internal 3D resolution requested from the game (marker res=WxH); the presenter scales it to the 960x544 panel. */
uint32_t g_diag_res_w=960, g_diag_res_h=544;
/* Per-frame counters written by the game thread only; reported with SH4_STATS by diag_perf_report. */
static unsigned ct_com, ct_com_locked, ct_ind, ct_ind_miss, ct_sleep, ct_sleep_ms, ct_open, ct_open_hit, ct_cs;
static uint64_t ct_sleep_ticks, ct_open_ticks, ct_com_ticks; static unsigned ct_com_sampled;
static uint64_t ct_method_ticks[8192]; static unsigned ct_method_n[8192]; /* sampled 1/16 host D3D calls per registry entry */
static unsigned ct_pace, ct_pace_late, ct_pace_ticks, ct_pace_count;
static unsigned long long ct_pace_ns; /* wall time inside the game limiter: frame interval minus this = real work */
static struct { uint32_t ret, ms; unsigned n; } ct_sleep_site[8];   /* game-thread Sleep callers (guest return address) */
/* getenv walks the environment under a lock: with fastcom the diagnostic switches are read once. */
static int env_verbose=-1, env_comlog=-1, env_tinylog=-1;
#define DIAG_ENV(cache,name) ((g_diag_perf&PERF_FASTCOM) ? ((cache)<0 ? ((cache)=getenv(name)!=NULL) : (cache)) : getenv(name)!=NULL)
/* Environment is frozen by main() before guest entry: read each diagnostic flag once (hot paths: waits, messages, mmio). */
#define DIAG_FLAG(name) ({ static int _df=-1; if (_df<0) _df=getenv(name)!=NULL; _df; })
/* Closed read-only game data streams kept for the next CreateFileA of the same path (filecache). */
static struct { char path[256]; FILE *f; } diag_file_cache[8];
static char diag_file_path[64][256];
#ifdef SH3_MOVIE_LAB
static uint32_t diag_movie_filter_watch;
void diag_movie_watch_guest_filter(uint32_t f) { __atomic_store_n(&diag_movie_filter_watch,f,__ATOMIC_RELAXED); }
#endif
/* Hot-path lab switches (guest call dispatch): environment fixed by main() before the guest entry point, so read
   once. A getenv per indirect call cost 37 -> 26 presents/s at the park, and 13/s once the movie flags lengthened
   the environment scanned by every lookup (bench fps-*, 2026-11-15). */
static int diag_movie_trace_callbacks(void) { static int v=-1; if (v<0) v=getenv("SH3_DIAG_MOVIE_TRACE_CALLBACKS")!=NULL; return v; }
static int diag_movie_allow_run(void) { static int v=-1; if (v<0) v=getenv("SH3_DIAG_MOVIE_ALLOW_RUN")!=NULL; return v; }
static int32_t diag_main_priority;
/* Freeze watchdog (Valentin: during loads even his overlay stops, i.e. no Present at all): when the game thread
   has presented nothing for >= 1 s, report once per second its last host call and how many host calls it made. */
static const char *volatile diag_main_last_host;
static volatile unsigned diag_main_host_calls;
static volatile uint32_t diag_main_last_ret;   /* guest return address of that call */
static volatile int diag_main_in_host;        /* 1 while the game thread is inside a fast-path Direct3D call */
#ifdef _WIN32
static void *diag_main_thread_handle;   /* PC only: real instruction pointer of a stalled game thread */
#endif
static int diag_freeze_watch(void *unused) {
    (void)unused; extern unsigned g_frame;
    unsigned lastf=0,lastcalls=0,lines=0; uint64_t since=SDL_GetTicks64();
    for (;;) {
        SDL_Delay(500);
        unsigned f=__atomic_load_n(&g_frame,__ATOMIC_RELAXED),calls=diag_main_host_calls; uint64_t now=SDL_GetTicks64();
        if (f!=lastf) { lastf=f; since=now; lastcalls=calls; continue; }
        if (now-since>=1000 && lines<300) {
            const char *h=diag_main_last_host;
            fprintf(stderr,"[freeze-sample] stalled=%llu ms frame=%u last_host=%s ret=%08x in_host=%d host_calls=+%u\n",
                    (unsigned long long)(now-since),f,h?h:"-",(unsigned)diag_main_last_ret,diag_main_in_host,calls-lastcalls);
            lastcalls=calls; lines++;
            if (now-since>=3000 && now-since<3500 && diag_main_cpu && diag_main_cpu->esp<DIAG_MEM_SIZE-64) {
                /* guest call chain of the stalled game thread (stack words that look like code addresses) */
                char line[400]; int k=0; uint32_t sp=diag_main_cpu->esp;
                for (unsigned i=0;i<64 && k<360;i++) { uint32_t w; memcpy(&w,g_mem+sp+4*i,4); if (w>=0x401000u && w<0x680000u) k+=snprintf(line+k,sizeof line-k," %08x",w); }
                fprintf(stderr,"[freeze-stack] esp=%08x code-words:%s\n",sp,line);
#ifdef _WIN32
                if (diag_main_thread_handle) {
                    for (int rep=0;rep<5;rep++) {
                        CONTEXT ctx; memset(&ctx,0,sizeof ctx); ctx.ContextFlags=CONTEXT_CONTROL;
                        if (SuspendThread((HANDLE)diag_main_thread_handle)!=(DWORD)-1) {
                            if (GetThreadContext((HANDLE)diag_main_thread_handle,&ctx)) fprintf(stderr,"[freeze-rip] sample %d rip=%p off=%llx\n",rep,(void *)(uintptr_t)ctx.Rip,(unsigned long long)(ctx.Rip-(uintptr_t)GetModuleHandle(NULL)));
                            ResumeThread((HANDLE)diag_main_thread_handle);
                        }
                        SDL_Delay(20);
                    }
                }
#endif
            }
        }
    }
    return 0;
}
typedef struct { CPU cpu; uint32_t stack,tib,callback,user,handle; SDL_TimerID timer; pthread_mutex_t lock; int self_killed; } DiagTimer;
static DiagTimer *diag_timer;
static char diag_mutex_names[128][128];
static uint32_t diag_tls_used;
static DiagHeap *diag_guest_heap;
void diag_memory_report(void) {
    diag_heap_report(diag_guest_heap);
    diag_event_report();
#ifdef __vita__
    SceKernelFreeMemorySizeInfo info={.size=sizeof info};
    int result=sceKernelGetFreeMemorySize(&info);
    if(result<0) fprintf(stderr,"[memory-live] query_failed=%08x\n",(unsigned)result);
    else fprintf(stderr,"[memory-live] user_free=%u cdram_free=%u phycont_free=%u\n",info.size_user,info.size_cdram,info.size_phycont);
#endif
}
static DiagTaskMem *diag_taskmem;
static uint32_t diag_command_line;
static uint32_t diag_env_w;
static uint32_t diag_exception_filter;
static unsigned diag_registry_open;
static FILE *diag_files[64];
static unsigned char diag_file_game[64];
static int diag_direct_io, diag_io_profile;
static uint64_t diag_pause_trace_from, diag_pause_trace_to;
static void diag_trace_pause_for(unsigned ms) { diag_pause_trace_from=SDL_GetTicks64(); diag_pause_trace_to=diag_pause_trace_from+ms; }
int diag_trace_pause(void) {
    if (!diag_pause_trace_to) return 0;
    uint64_t t=SDL_GetTicks64();
    return t>=diag_pause_trace_from && t<diag_pause_trace_to;
}
static uint64_t diag_io_ticks, diag_io_bytes;
static unsigned diag_io_reads;
static uint32_t diag_window_proc;
static uint32_t diag_window_class_style;
static uint32_t diag_window_style=0x02cb0000u, diag_window_exstyle=0x300;
static _Thread_local unsigned diag_com_init_count;
_Noreturn void rt_fatal(const char *fmt, ...);
uint32_t rt_guest_call(CPU *c,uint32_t fn,int nargs,...);
#ifdef __vita__
void rt_cpu_profile_init(void);
int rt_cpu_profile_thread(const char *label);
#endif

/* Resolve only the synthetic game's data subtree, following the candidate/data
 * symlink once and rejecting any resolved path outside that root. */
static int diag_data_path(const char *guest,char *resolved,size_t size) {
#ifdef __vita__
    struct stat st;
    if (sh3_vita_data_path(guest,resolved,size)) return -1;
    if (!stat(resolved,&st)) return 1;
    return errno==ENOENT?0:-1;
#else
    const char *prefix="C:\\SILENT HILL 3\\data"; size_t plen=strlen(prefix);
    if (strncasecmp(guest,prefix,plen) || (guest[plen] && guest[plen]!='/' && guest[plen]!='\\')) return -1;
    char root[4096],host[4096],tmp[4096];
    if (!realpath("game/data",root)) { fprintf(stderr,"[diag] dp: root realpath failed\n"); return -1; }
    size_t used=strlen(root), extra=strlen(guest+plen);
    if (used+extra>=sizeof host) return -1;
    memcpy(host,root,used);
    for (const char *s=guest+plen;*s;s++) host[used++]=*s=='\\'?'/':*s;
    host[used]=0;
    if (!realpath(host,tmp)) return errno==ENOENT?0:-1;
    size_t rootlen=strlen(root),n=strlen(tmp);
    if (getenv("SH3_DP_DEBUG")) fprintf(stderr,"[dp] root=%s tmp=%s\n",root,tmp);
    if (strncmp(tmp,root,rootlen) || (tmp[rootlen] && tmp[rootlen]!='/') || n>=size) return -1;
    memcpy(resolved,tmp,n+1); return 1;
#endif
}
static const uint16_t cp1252_extra[32]={
    0x20ac,0x0081,0x201a,0x0192,0x201e,0x2026,0x2020,0x2021,
    0x02c6,0x2030,0x0160,0x2039,0x0152,0x008d,0x017d,0x008f,
    0x0090,0x2018,0x2019,0x201c,0x201d,0x2022,0x2013,0x2014,
    0x02dc,0x2122,0x0161,0x203a,0x0153,0x009d,0x017e,0x0178};

static int diag_cp1252_encode(uint16_t ch) {
    if (ch<0x80 || (ch>=0xa0 && ch<=0xff)) return ch;
    for (int i=0; i<32; i++) if (cp1252_extra[i]==ch) return 0x80+i;
    return -1;
}

/* Synthetic Windows XP environment, matching sh4vita's diagnostic choice.
 * Fail closed for invalid guest pointers rather than writing outside the arena. */
void imp_KERNEL32_GetVersionExA(CPU *c) {
    uint32_t p, sz;
    if (c->esp > DIAG_MEM_SIZE - 8) { fprintf(stderr, "[diag] invalid GetVersionExA stack\n"); exit(36); }
    memcpy(&p, g_mem+c->esp+4, 4);
    if (p > DIAG_MEM_SIZE-4) { fprintf(stderr, "[diag] invalid GetVersionExA pointer\n"); exit(36); }
    memcpy(&sz, g_mem+p, 4);
    if (sz != 148 && sz != 156) { c->eax=0; c->esp+=8; return; }
    if (p > DIAG_MEM_SIZE-sz) { fprintf(stderr, "[diag] invalid GetVersionExA buffer\n"); exit(36); }
    memset(g_mem+p+4, 0, sz-4);
    uint32_t values[4]={5,1,2600,2};
    memcpy(g_mem+p+4,values,sizeof values);
    c->eax=1; c->esp+=8;
    fprintf(stderr, "[diag] GetVersionExA: synthetic Windows XP, buffer=%08x size=%u\n",p,sz);
}

void imp_KERNEL32_GetModuleHandleA(CPU *c) {
    uint32_t name;
    if (c->esp > DIAG_MEM_SIZE-8) { fprintf(stderr, "[diag] invalid GetModuleHandleA stack\n"); exit(36); }
    memcpy(&name,g_mem+c->esp+4,4);
    if (name) {
        if (name >= DIAG_MEM_SIZE || !memchr(g_mem+name,0,DIAG_MEM_SIZE-name)) {
            fprintf(stderr,"[diag] invalid GetModuleHandleA name\n"); exit(36);
        }
        if (!strcasecmp((char *)g_mem+name,"kernel32.dll") || !strcasecmp((char *)g_mem+name,"kernel32")) {
            fprintf(stderr,"[diag] GetModuleHandleA(kernel32.dll) -> synthetic handle\n");
            c->eax=0x7c800000u; c->esp+=8; return;
        }
        if (!strcasecmp((char *)g_mem+name,"d3d8.dll")) {
            if (DIAG_FLAG("SH3_DIAG_VERBOSE")) fprintf(stderr,"[diag] GetModuleHandleA(d3d8.dll) -> synthetic handle\n");
            c->eax=0x7b000000u; c->esp+=8; return;
        }
        if (!strcasecmp((char *)g_mem+name,"d3d8d.dll")) {
            if (DIAG_FLAG("SH3_DIAG_VERBOSE")) fprintf(stderr,"[diag] GetModuleHandleA(d3d8d.dll) -> debug module absent\n");
            c->eax=0; c->esp+=8; return;
        }
        fprintf(stderr, "[diag] GetModuleHandleA named module not implemented: %.128s\n",g_mem+name); exit(30);
    }
    c->eax=0x400000; c->esp+=8;
    fprintf(stderr, "[diag] GetModuleHandleA(NULL) -> main PE image\n");
}

void imp_KERNEL32_LoadLibraryA(CPU *c) {
    uint32_t p;
    if (c->esp>DIAG_MEM_SIZE-8) { fprintf(stderr,"[diag] invalid LoadLibraryA stack\n"); exit(36); }
    memcpy(&p,g_mem+c->esp+4,4);
    if (!p || p>=DIAG_MEM_SIZE || !memchr(g_mem+p,0,DIAG_MEM_SIZE-p)) { fprintf(stderr,"[diag] invalid LoadLibraryA name\n"); exit(36); }
    if (!strcasecmp((char *)g_mem+p,"d3d8.dll")) {
        if (DIAG_FLAG("SH3_DIAG_VERBOSE")) fprintf(stderr,"[diag] LoadLibraryA d3d8.dll -> synthetic module\n"); c->eax=0x7b000000u; c->esp+=8; return;
    }
    fprintf(stderr,"[diag] LoadLibraryA %.200s not implemented\n",g_mem+p); exit(30);
}

void imp_KERNEL32_lstrlenA(CPU *c) {
    uint32_t p;
    if (c->esp>DIAG_MEM_SIZE-8) { fprintf(stderr,"[diag] invalid lstrlenA stack\n"); exit(36); }
    memcpy(&p,g_mem+c->esp+4,4);
    if (!p) c->eax=0;
    else {
        if (p>=DIAG_MEM_SIZE) { fprintf(stderr,"[diag] invalid lstrlenA pointer\n"); exit(36); }
        const uint8_t *end=memchr(g_mem+p,0,DIAG_MEM_SIZE-p);
        if (!end) { fprintf(stderr,"[diag] unterminated lstrlenA input\n"); exit(36); }
        c->eax=(uint32_t)(end-(g_mem+p));
    }
    c->esp+=8;
}

void imp_OLE32_CoInitialize(CPU *c) {
    uint32_t reserved;
    if (c->esp>DIAG_MEM_SIZE-8) { fprintf(stderr,"[diag] invalid CoInitialize stack\n"); exit(36); }
    memcpy(&reserved,g_mem+c->esp+4,4);
    if (reserved) { fprintf(stderr,"[diag] unsupported CoInitialize arguments\n"); exit(30); }
    c->eax=diag_com_init_count++?1:0; c->esp+=8;
}
void imp_OLE32_CoUninitialize(CPU *c) {
    if (c->esp>DIAG_MEM_SIZE-4 || !diag_com_init_count) { fprintf(stderr,"[diag] unmatched CoUninitialize\n"); exit(30); }
    diag_com_init_count--; c->esp+=4;
}

void imp_KERNEL32_HeapCreate(CPU *c) {
    uint32_t a[3];
    if (c->esp > DIAG_MEM_SIZE-16) { fprintf(stderr, "[diag] invalid HeapCreate stack\n"); exit(36); }
    memcpy(a,g_mem+c->esp+4,sizeof a);
    fprintf(stderr,"[diag] HeapCreate options=%08x initial=%u maximum=%u\n",a[0],a[1],a[2]);
    if (a[0] || a[2] || a[1] > DIAG_MEM_SIZE-0x7180000u) {
        fprintf(stderr,"[diag] unsupported HeapCreate options\n"); exit(30);
    }
    c->eax=DIAG_HEAP_HANDLE; c->esp+=16;
}

void imp_KERNEL32_GetProcAddress(CPU *c) {
    uint32_t args[2];
    if (c->esp > DIAG_MEM_SIZE-12) { fprintf(stderr,"[diag] invalid GetProcAddress stack\n"); exit(36); }
    memcpy(args,g_mem+c->esp+4,sizeof args);
    if (args[1] <= 0xffff) {
        fprintf(stderr,"[diag] GetProcAddress module=%08x ordinal=%u not implemented\n",args[0],args[1]);
    } else if (args[1] < DIAG_MEM_SIZE && memchr(g_mem+args[1],0,DIAG_MEM_SIZE-args[1])) {
        if (args[0]==0x7b000000u && (!strcmp((char *)g_mem+args[1],"ValidatePixelShader") ||
                                    !strcmp((char *)g_mem+args[1],"ValidateVertexShader") ||
                                    !strcmp((char *)g_mem+args[1],"DebugSetMute"))) {
            if (DIAG_FLAG("SH3_DIAG_VERBOSE")) fprintf(stderr,"[diag] optional %.128s unavailable\n",g_mem+args[1]); c->eax=0; c->esp+=12; return;
        }
        if (args[0] == 0x7c800000u &&
            (!strcmp((char *)g_mem+args[1],"InitializeCriticalSectionAndSpinCount") ||
             !strcmp((char *)g_mem+args[1],"IsProcessorFeaturePresent"))) {
            /* Deliberately absent from this synthetic environment, as in SH4;
             * the guest can use its fallback. No synchronisation is faked. */
            fprintf(stderr,"[diag] GetProcAddress %.128s -> unavailable\n",g_mem+args[1]);
            c->eax=0; c->esp+=12; return;
        }
        fprintf(stderr,"[diag] GetProcAddress module=%08x name=%.128s not implemented\n",args[0],g_mem+args[1]);
    } else { fprintf(stderr,"[diag] invalid GetProcAddress name\n"); exit(36); }
    exit(30);
}

/* Layout and table-index scheme follow sh4vita's kernel32 compatibility layer. */
void imp_KERNEL32_InitializeCriticalSection(CPU *c) {
    uint32_t p;
    if (c->esp > DIAG_MEM_SIZE-8) { fprintf(stderr,"[diag] invalid critical section stack\n"); exit(36); }
    memcpy(&p,g_mem+c->esp+4,4);
    if (p > DIAG_MEM_SIZE-24 || diag_critical_count >= 512) { fprintf(stderr,"[diag] invalid critical section storage\n"); exit(36); }
    pthread_mutexattr_t attr;
    if (pthread_mutexattr_init(&attr) || pthread_mutexattr_settype(&attr,PTHREAD_MUTEX_RECURSIVE) ||
        pthread_mutex_init(&diag_critical[diag_critical_count],&attr)) {
        fprintf(stderr,"[diag] critical section initialization failed\n"); exit(36);
    }
    pthread_mutexattr_destroy(&attr);
    uint32_t id=++diag_critical_count;
    diag_critical_live[id-1]=1;
    memset(g_mem+p,0,24); memcpy(g_mem+p+16,&id,4);
    c->esp+=8;
    fprintf(stderr,"[diag] InitializeCriticalSection guest=%08x id=%u\n",p,id);
}

static pthread_mutex_t *diag_critical_arg(CPU *c, uint32_t *p) {
    uint32_t id;
    if (c->esp > DIAG_MEM_SIZE-8) { fprintf(stderr,"[diag] invalid critical section stack\n"); exit(36); }
    memcpy(p,g_mem+c->esp+4,4);
    if (*p > DIAG_MEM_SIZE-24) { fprintf(stderr,"[diag] invalid critical section address\n"); exit(36); }
    memcpy(&id,g_mem+*p+16,4);
    if (!id || id>diag_critical_count || !diag_critical_live[id-1]) {
        fprintf(stderr,"[diag] uninitialized critical section\n"); exit(36);
    }
    return &diag_critical[id-1];
}
void imp_KERNEL32_EnterCriticalSection(CPU *c) {
    uint32_t p; pthread_mutex_t *m=diag_critical_arg(c,&p);
    if (pthread_mutex_lock(m)) { fprintf(stderr,"[diag] critical section lock failed\n"); exit(36); }
    if (c==diag_main_cpu) ct_cs++;
    c->esp+=8;
}
void imp_KERNEL32_LeaveCriticalSection(CPU *c) {
    uint32_t p; pthread_mutex_t *m=diag_critical_arg(c,&p);
    if (pthread_mutex_unlock(m)) { fprintf(stderr,"[diag] critical section unlock failed\n"); exit(36); }
    c->esp+=8;
}
void imp_KERNEL32_DeleteCriticalSection(CPU *c) {
    uint32_t p, id; pthread_mutex_t *m=diag_critical_arg(c,&p);
    memcpy(&id,g_mem+p+16,4);
    if (pthread_mutex_destroy(m)) { fprintf(stderr,"[diag] critical section destroy failed\n"); exit(36); }
    diag_critical_live[id-1]=0; memset(g_mem+p+16,0,4); c->esp+=8;
}

void imp_KERNEL32_TlsAlloc(CPU *c) {
    if (c->esp > DIAG_MEM_SIZE-4) { fprintf(stderr,"[diag] invalid TlsAlloc stack\n"); exit(36); }
    uint32_t i;
    for (i=0; i<32 && (diag_tls_used & (1u<<i)); i++) {}
    if (i<32) diag_tls_used|=1u<<i;
    c->eax=i<32?i:0xffffffffu; c->esp+=4;
    fprintf(stderr,"[diag] TlsAlloc -> %08x\n",c->eax);
}

void imp_KERNEL32_HeapAlloc(CPU *c) {
    uint32_t a[3];
    if (c->esp > DIAG_MEM_SIZE-16) { fprintf(stderr,"[diag] invalid HeapAlloc stack\n"); exit(36); }
    memcpy(a,g_mem+c->esp+4,sizeof a);
    if (a[0]!=DIAG_HEAP_HANDLE || (a[1]&~8u)) { fprintf(stderr,"[diag] unsupported HeapAlloc handle/flags\n"); exit(30); }
    c->eax=diag_heap_alloc(diag_guest_heap,a[2],!!(a[1]&8));
    if (!c->eax && a[2]) {   /* allocation failure: who asked (guest return address and the code addresses on the stack) */
        static unsigned reported; extern unsigned g_frame;
        if (reported++<8) {
            char chain[300]; int k=0;
            for (unsigned i=0;i<48 && k<260 && c->esp+4*i<DIAG_MEM_SIZE-4;i++) { uint32_t w; memcpy(&w,g_mem+c->esp+4*i,4); if (w>=0x401000u && w<0x680000u) k+=snprintf(chain+k,sizeof chain-k," %08x",w); }
            fprintf(stderr,"[heap-failure-caller] size=%u frame=%u stage=%u chain:%s\n",a[2],g_frame,(unsigned)g_mem[0x70e66d9u],chain);
        }
    }
    if (a[2]<=4 && DIAG_ENV(env_tinylog,"SH3_DIAG_TINYLOG")) {
        uint32_t return_pc; memcpy(&return_pc,g_mem+c->esp,4);
        uint32_t frame_return=0, frame_parent=0;
        if (c->ebp && c->ebp<=DIAG_MEM_SIZE-8) {
            memcpy(&frame_parent,g_mem+c->ebp,4); memcpy(&frame_return,g_mem+c->ebp+4,4);
        }
        fprintf(stderr,"[diag] tiny guest HeapAlloc addr=%08x size=%u return=%08x frame_return=%08x parent=%08x\n",c->eax,a[2],return_pc,frame_return,frame_parent);
    }
    c->esp+=16;
    if (DIAG_ENV(env_verbose,"SH3_DIAG_VERBOSE")) fprintf(stderr,"[diag] HeapAlloc size=%u flags=%x -> %08x\n",a[2],a[1],c->eax);
}

void imp_KERNEL32_HeapFree(CPU *c) {
    uint32_t a[3];
    if (c->esp > DIAG_MEM_SIZE-16) { fprintf(stderr,"[diag] invalid HeapFree stack\n"); exit(36); }
    memcpy(a,g_mem+c->esp+4,sizeof a);
#ifdef SH3_MOVIE_LAB
    uint32_t watched=__atomic_load_n(&diag_movie_filter_watch,__ATOMIC_RELAXED);
    if (watched && a[2] && a[2]<=watched && watched-a[2]<4096) {
        uint32_t n=0,ret=0;
        if (!diag_heap_size(diag_guest_heap,a[2],&n) && watched-a[2]<n) {
            memcpy(&ret,g_mem+c->esp,4);
            fprintf(stderr,"[movie-lab] watched filter %08x freed with block=%08x size=%u caller=%08x\n",watched,a[2],n,ret);
        }
    }
#endif
    if (a[0]!=DIAG_HEAP_HANDLE || a[1]!=0 || !a[2]) { fprintf(stderr,"[diag] unsupported HeapFree arguments\n"); exit(30); }
    if (diag_heap_free(diag_guest_heap,a[2])) { fprintf(stderr,"[diag] untracked HeapFree pointer %08x\n",a[2]); exit(30); }
    c->eax=1; c->esp+=16;
    if (DIAG_ENV(env_verbose,"SH3_DIAG_VERBOSE")) fprintf(stderr,"[diag] HeapFree %08x\n",a[2]);
}
void imp_OLE32_CoTaskMemAlloc(CPU *c) {
    uint32_t size;
    if(c->esp>DIAG_MEM_SIZE-8) { fprintf(stderr,"[diag] invalid CoTaskMemAlloc stack\n"); exit(36); }
    memcpy(&size,g_mem+c->esp+4,4);
    c->eax=diag_taskmem_alloc(diag_taskmem,size); c->esp+=8;
}
void imp_OLE32_CoTaskMemFree(CPU *c) {
    uint32_t ptr;
    if(c->esp>DIAG_MEM_SIZE-8) { fprintf(stderr,"[diag] invalid CoTaskMemFree stack\n"); exit(36); }
    memcpy(&ptr,g_mem+c->esp+4,4);
    if(diag_taskmem_free(diag_taskmem,ptr)) { fprintf(stderr,"[diag] invalid CoTaskMemFree %08x\n",ptr); exit(36); }
    c->esp+=8;
}

void imp_KERNEL32_HeapSize(CPU *c) {
    uint32_t a[3];
    if (c->esp > DIAG_MEM_SIZE-16) { fprintf(stderr,"[diag] invalid HeapSize stack\n"); exit(36); }
    memcpy(a,g_mem+c->esp+4,sizeof a);
    if (a[0]!=DIAG_HEAP_HANDLE || a[1]!=0 || !a[2]) { fprintf(stderr,"[diag] unsupported HeapSize arguments\n"); exit(30); }
    uint32_t size;
    if (diag_heap_size(diag_guest_heap,a[2],&size)) { fprintf(stderr,"[diag] untracked HeapSize pointer %08x\n",a[2]); exit(30); }
    c->eax=size; c->esp+=16;
}

void imp_KERNEL32_TlsSetValue(CPU *c) {
    uint32_t a[2];
    if (c->esp > DIAG_MEM_SIZE-12) { fprintf(stderr,"[diag] invalid TlsSetValue stack\n"); exit(36); }
    memcpy(a,g_mem+c->esp+4,sizeof a);
    if (a[0]>=32 || !(diag_tls_used & (1u<<a[0])) || c->fs_base > DIAG_MEM_SIZE-0xe10-128) {
        fprintf(stderr,"[diag] unsupported TlsSetValue index or TIB\n"); exit(30);
    }
    memcpy(g_mem+c->fs_base+0xe10+4*a[0],&a[1],4);
    c->eax=1; c->esp+=12;
    fprintf(stderr,"[diag] TlsSetValue index=%u value=%08x\n",a[0],a[1]);
}

void imp_KERNEL32_TlsGetValue(CPU *c) {
    uint32_t index, zero=0;
    if (c->esp>DIAG_MEM_SIZE-8 || c->fs_base>DIAG_MEM_SIZE-0xe10-128) { fprintf(stderr,"[diag] invalid TlsGetValue stack or TIB\n"); exit(36); }
    memcpy(&index,g_mem+c->esp+4,4);
    if (index>=32 || !(diag_tls_used & (1u<<index))) { fprintf(stderr,"[diag] unsupported TlsGetValue index\n"); exit(30); }
    memcpy(&c->eax,g_mem+c->fs_base+0xe10+4*index,4); memcpy(g_mem+c->fs_base+0x34,&zero,4); c->esp+=8;
}
/* CRT per-thread data (_getptd) of the calling guest thread; 0 if its TLS slot is not set (native rand paths). */
uint32_t diag_rand_thread_data(CPU *c) {
    uint32_t index=0,p=0;
    memcpy(&index,g_mem+0x7296b8u,4);
    if (index>=32 || !(diag_tls_used&(1u<<index)) || c->fs_base>DIAG_MEM_SIZE-0xe10-128) return 0;
    memcpy(&p,g_mem+c->fs_base+0xe10+4*index,4);
    return p;
}

void imp_KERNEL32_GetCurrentThreadId(CPU *c) {
    if (c->esp > DIAG_MEM_SIZE-4 || c->fs_base>DIAG_MEM_SIZE-0x28) { fprintf(stderr,"[diag] invalid GetCurrentThreadId stack/TIB\n"); exit(36); }
    memcpy(&c->eax,g_mem+c->fs_base+0x24,4); c->esp+=4;
}

void imp_KERNEL32_GetCurrentProcessId(CPU *c) {
    if (c->esp > DIAG_MEM_SIZE-4) { fprintf(stderr,"[diag] invalid process id stack\n"); exit(36); }
    c->eax=1; c->esp+=4; /* diagnostic models a single process */
}
void imp_KERNEL32_GetCurrentThread(CPU *c) {
    if (c->esp>DIAG_MEM_SIZE-4) { fprintf(stderr,"[diag] invalid GetCurrentThread stack\n"); exit(36); }
    c->eax=0xfffffffeu; c->esp+=4;
}
void imp_KERNEL32_SetThreadAffinityMask(CPU *c) {
    uint32_t a[2];
    if (c->esp>DIAG_MEM_SIZE-12) { fprintf(stderr,"[diag] invalid affinity stack\n"); exit(36); }
    memcpy(a,g_mem+c->esp+4,sizeof a);
    if ((a[0]!=0xfffffffeu && (a[0]<0xf8000000u || a[0]-0xf8000000u>=diag_thread_count)) || a[1]!=1) {
        fprintf(stderr,"[diag] unsupported thread affinity request handle=%08x mask=%08x\n",a[0],a[1]); exit(30);
    }
    fprintf(stderr,"[diag] SetThreadAffinityMask advisory only; host affinity unchanged\n"); c->eax=1; c->esp+=12;
}
void imp_KERNEL32_SetThreadPriority(CPU *c) {
    uint32_t a[2];
    if (c->esp>DIAG_MEM_SIZE-12) { fprintf(stderr,"[diag] invalid thread priority stack\n"); exit(36); }
    memcpy(a,g_mem+c->esp+4,sizeof a);
    pthread_mutex_lock(&diag_object_lock);
    int32_t *priority=NULL;
    if (a[0]==0xfffffffeu) {
        if (c==diag_main_cpu) priority=&diag_main_priority;
        else for (unsigned i=0;i<diag_thread_count;i++) if (&diag_threads[i]->cpu==c) priority=&diag_threads[i]->priority;
    } else if (a[0]>=0xf8000000u && a[0]-0xf8000000u<diag_thread_count) priority=&diag_threads[a[0]-0xf8000000u]->priority;
    if (!priority) { pthread_mutex_unlock(&diag_object_lock); fprintf(stderr,"[diag] unsupported thread priority handle\n"); exit(30); }
    *priority=(int32_t)a[1]; pthread_mutex_unlock(&diag_object_lock);
    fprintf(stderr,"[diag] SetThreadPriority %d recorded; host priority unchanged\n",(int32_t)a[1]); c->eax=1; c->esp+=12;
}
void imp_KERNEL32_GetThreadPriority(CPU *c) {
    uint32_t h; int found=0; int32_t priority=0;
    if (c->esp>DIAG_MEM_SIZE-8) { fprintf(stderr,"[diag] invalid GetThreadPriority stack\n"); exit(36); }
    memcpy(&h,g_mem+c->esp+4,4); pthread_mutex_lock(&diag_object_lock);
    if (h==0xfffffffeu) {
        if (c==diag_main_cpu) { priority=diag_main_priority; found=1; }
        else for (unsigned i=0;i<diag_thread_count;i++) if (&diag_threads[i]->cpu==c) { priority=diag_threads[i]->priority; found=1; }
    } else if (h>=0xf8000000u && h-0xf8000000u<diag_thread_count) { priority=diag_threads[h-0xf8000000u]->priority; found=1; }
    pthread_mutex_unlock(&diag_object_lock);
    if (!found) { fprintf(stderr,"[diag] unsupported GetThreadPriority handle=%08x\n",h); exit(30); }
    c->eax=(uint32_t)priority; c->esp+=8;
}
void imp_KERNEL32_SetThreadPriorityBoost(CPU *c) {
    uint32_t a[2];
    if (c->esp>DIAG_MEM_SIZE-12) { fprintf(stderr,"[diag] invalid priority boost stack\n"); exit(36); }
    memcpy(a,g_mem+c->esp+4,sizeof a);
    if (a[0]!=0xfffffffeu && (a[0]<0xf8000000u || a[0]-0xf8000000u>=diag_thread_count)) {
        fprintf(stderr,"[diag] unsupported priority boost handle\n"); exit(30);
    }
    fprintf(stderr,"[diag] SetThreadPriorityBoost ignored in host diagnostic\n"); c->eax=1; c->esp+=12;
}

void imp_KERNEL32_GetLastError(CPU *c) {
    if (c->esp>DIAG_MEM_SIZE-4 || c->fs_base>DIAG_MEM_SIZE-0x38) { fprintf(stderr,"[diag] invalid last-error location\n"); exit(36); }
    memcpy(&c->eax,g_mem+c->fs_base+0x34,4); c->esp+=4;
}

void imp_KERNEL32_SetLastError(CPU *c) {
    uint32_t value;
    if (c->esp>DIAG_MEM_SIZE-8 || c->fs_base>DIAG_MEM_SIZE-0x38) { fprintf(stderr,"[diag] invalid last-error location\n"); exit(36); }
    memcpy(&value,g_mem+c->esp+4,4); memcpy(g_mem+c->fs_base+0x34,&value,4); c->esp+=8;
}

void imp_KERNEL32_InterlockedExchange(CPU *c) {
    uint32_t a[2];
    if (c->esp>DIAG_MEM_SIZE-12) { fprintf(stderr,"[diag] invalid InterlockedExchange stack\n"); exit(36); }
    memcpy(a,g_mem+c->esp+4,sizeof a);
    if (!a[0] || (a[0]&3u) || a[0]>DIAG_MEM_SIZE-4) { fprintf(stderr,"[diag] invalid atomic address\n"); exit(36); }
    c->eax=__atomic_exchange_n((uint32_t *)(g_mem+a[0]),a[1],__ATOMIC_SEQ_CST); c->esp+=12;
}
static void diag_interlocked_add(CPU *c,uint32_t delta) {
    uint32_t p;
    if (c->esp>DIAG_MEM_SIZE-8) { fprintf(stderr,"[diag] invalid atomic stack\n"); exit(36); }
    memcpy(&p,g_mem+c->esp+4,4);
    if (!p || (p&3u) || p>DIAG_MEM_SIZE-4) { fprintf(stderr,"[diag] invalid atomic address\n"); exit(36); }
    c->eax=__atomic_add_fetch((uint32_t *)(g_mem+p),delta,__ATOMIC_SEQ_CST); c->esp+=8;
}
void imp_KERNEL32_InterlockedIncrement(CPU *c) { diag_interlocked_add(c,1); }
void imp_KERNEL32_InterlockedDecrement(CPU *c) { diag_interlocked_add(c,UINT32_MAX); }
static void diag_interlocked_exchange(CPU *c,int compare) {
    uint32_t a[3]={0}; unsigned n=compare?3:2;
    if (c->esp>DIAG_MEM_SIZE-4-4*n) { fprintf(stderr,"[diag] invalid atomic stack\n"); exit(36); }
    memcpy(a,g_mem+c->esp+4,4*n);
    if (!a[0] || (a[0]&3u) || a[0]>DIAG_MEM_SIZE-4) { fprintf(stderr,"[diag] invalid atomic address\n"); exit(36); }
    uint32_t *p=(uint32_t *)(g_mem+a[0]);
    if (compare) { uint32_t old=a[2]; __atomic_compare_exchange_n(p,&old,a[1],0,__ATOMIC_SEQ_CST,__ATOMIC_SEQ_CST); c->eax=old; }
    else c->eax=__atomic_fetch_add(p,a[1],__ATOMIC_SEQ_CST);
    c->esp+=4+4*n;
}
void imp_KERNEL32_InterlockedExchangeAdd(CPU *c) { diag_interlocked_exchange(c,0); }
void imp_KERNEL32_InterlockedCompareExchange(CPU *c) { diag_interlocked_exchange(c,1); }
void imp_KERNEL32_Sleep(CPU *c) {
    uint32_t ms;
    if (c->esp>DIAG_MEM_SIZE-8) { fprintf(stderr,"[diag] invalid Sleep stack\n"); exit(36); }
    memcpy(&ms,g_mem+c->esp+4,4);
    if (c==diag_main_cpu) {
        uint64_t t0=SDL_GetPerformanceCounter(); SDL_Delay(ms);
        ct_sleep_ticks+=SDL_GetPerformanceCounter()-t0; ct_sleep++; ct_sleep_ms+=ms;
        uint32_t site; memcpy(&site,g_mem+c->esp,4);
        unsigned k=0; while (k<7 && ct_sleep_site[k].n && ct_sleep_site[k].ret!=site) k++;
        ct_sleep_site[k].ret=site; ct_sleep_site[k].n++; ct_sleep_site[k].ms+=ms;
    } else SDL_Delay(ms);
    c->esp+=8;
}
void imp_KERNEL32_GetCurrentProcess(CPU *c) { c->eax=0xffffffffu; c->esp+=4; }
void imp_KERNEL32_GetLocalTime(CPU *c) {
    uint32_t p; struct timespec now; struct tm local;
    if (c->esp>DIAG_MEM_SIZE-8) { fprintf(stderr,"[diag] invalid GetLocalTime stack\n"); exit(36); }
    memcpy(&p,g_mem+c->esp+4,4);
    if (!p || p>DIAG_MEM_SIZE-16 || clock_gettime(CLOCK_REALTIME,&now) || !localtime_r(&now.tv_sec,&local)) {
        fprintf(stderr,"[diag] GetLocalTime failed\n"); exit(36);
    }
    uint16_t st[8]={(uint16_t)(local.tm_year+1900),(uint16_t)(local.tm_mon+1),(uint16_t)local.tm_wday,
        (uint16_t)local.tm_mday,(uint16_t)local.tm_hour,(uint16_t)local.tm_min,(uint16_t)local.tm_sec,(uint16_t)(now.tv_nsec/1000000)};
    memcpy(g_mem+p,st,sizeof st); c->esp+=8;
}
void imp_KERNEL32_FileTimeToLocalFileTime(CPU *c) {
    uint32_t a[2]; uint64_t ft;
    if (c->esp>DIAG_MEM_SIZE-12) { fprintf(stderr,"[diag] invalid FileTimeToLocalFileTime stack\n"); exit(36); }
    memcpy(a,g_mem+c->esp+4,sizeof a);
    if (!a[0] || !a[1] || a[0]>DIAG_MEM_SIZE-8 || a[1]>DIAG_MEM_SIZE-8) { fprintf(stderr,"[diag] invalid filetime pointer\n"); exit(36); }
    memcpy(&ft,g_mem+a[0],8);
    if (ft<116444736000000000ULL) { fprintf(stderr,"[diag] pre-1970 filetime unsupported\n"); exit(30); }
#ifdef __vita__
    SceDateTime dt; SceRtcTick utc,local; SceUInt64 converted=0;
    if (sceRtcSetWin32FileTime(&dt,ft)<0 || sceRtcGetTick(&dt,&utc)<0 ||
        sceRtcConvertUtcToLocalTime(&utc,&local)<0 || sceRtcSetTick(&dt,&local)<0 ||
        sceRtcGetWin32FileTime(&dt,&converted)<0) { fprintf(stderr,"[diag] Vita filetime conversion failed\n"); exit(36); }
    ft=(uint64_t)converted+(ft%10u); memcpy(g_mem+a[1],&ft,8); c->eax=1; c->esp+=12;
#else
    time_t sec=(time_t)((ft-116444736000000000ULL)/10000000ULL); struct tm t;
    if (!localtime_r(&sec,&t)) { fprintf(stderr,"[diag] invalid local filetime\n"); exit(36); }
    int64_t adjusted=(int64_t)ft+(int64_t)SH3_GMTOFF(t,sec)*10000000LL;
    if (adjusted<0) { fprintf(stderr,"[diag] filetime underflow\n"); exit(36); }
    ft=(uint64_t)adjusted; memcpy(g_mem+a[1],&ft,8); c->eax=1; c->esp+=12;
#endif
}
void imp_KERNEL32_FileTimeToSystemTime(CPU *c) {
    uint32_t a[2]; uint64_t ft;
    if (c->esp>DIAG_MEM_SIZE-12) { fprintf(stderr,"[diag] invalid FileTimeToSystemTime stack\n"); exit(36); }
    memcpy(a,g_mem+c->esp+4,sizeof a);
    if (!a[0] || !a[1] || a[0]>DIAG_MEM_SIZE-8 || a[1]>DIAG_MEM_SIZE-16) { fprintf(stderr,"[diag] invalid system time pointer\n"); exit(36); }
    memcpy(&ft,g_mem+a[0],8);
    if (ft<116444736000000000ULL) { fprintf(stderr,"[diag] pre-1970 system time unsupported\n"); exit(30); }
    uint64_t ticks=ft-116444736000000000ULL; time_t sec=(time_t)(ticks/10000000ULL); struct tm t;
    if (!gmtime_r(&sec,&t)) { fprintf(stderr,"[diag] invalid system time\n"); exit(36); }
    uint16_t st[8]={(uint16_t)(t.tm_year+1900),(uint16_t)(t.tm_mon+1),(uint16_t)t.tm_wday,
        (uint16_t)t.tm_mday,(uint16_t)t.tm_hour,(uint16_t)t.tm_min,(uint16_t)t.tm_sec,(uint16_t)((ticks%10000000ULL)/10000)};
    memcpy(g_mem+a[1],st,sizeof st); c->eax=1; c->esp+=12;
}
void imp_KERNEL32_SetPriorityClass(CPU *c) {
    uint32_t a[2];
    if (c->esp>DIAG_MEM_SIZE-12) { fprintf(stderr,"[diag] invalid SetPriorityClass stack\n"); exit(36); }
    memcpy(a,g_mem+c->esp+4,sizeof a);
    if (a[0]!=0xffffffffu || (a[1]!=0x20 && a[1]!=0x40 && a[1]!=0x80 && a[1]!=0x100 && a[1]!=0x4000 && a[1]!=0x8000)) {
        fprintf(stderr,"[diag] unsupported SetPriorityClass %08x %08x\n",a[0],a[1]); exit(30);
    }
    fprintf(stderr,"[diag] SetPriorityClass %08x advisory only\n",a[1]); c->eax=1; c->esp+=12;
}

void imp_USER32_LoadIconA(CPU *c) {
    uint32_t a[2];
    if (c->esp>DIAG_MEM_SIZE-12) { fprintf(stderr,"[diag] invalid LoadIconA stack\n"); exit(36); }
    memcpy(a,g_mem+c->esp+4,sizeof a);
    if (a[0]!=0x400000 || a[1]!=101) { fprintf(stderr,"[diag] unsupported LoadIconA resource\n"); exit(30); }
    fprintf(stderr,"[diag] LoadIconA uses placeholder; no icon is rendered\n");
    c->eax=0xf2000001u; c->esp+=12;
}

void imp_GDI32_GetStockObject(CPU *c) {
    uint32_t type;
    if (c->esp>DIAG_MEM_SIZE-8) { fprintf(stderr,"[diag] invalid GetStockObject stack\n"); exit(36); }
    memcpy(&type,g_mem+c->esp+4,4);
    if (type!=4) { fprintf(stderr,"[diag] unsupported GetStockObject type=%u\n",type); exit(30); }
    fprintf(stderr,"[diag] GetStockObject black brush placeholder\n");
    c->eax=0xf3000004u; c->esp+=8;
}

void imp_USER32_RegisterClassExA(CPU *c) {
    uint32_t p, size;
    if (c->esp>DIAG_MEM_SIZE-8) { fprintf(stderr,"[diag] invalid window class stack\n"); exit(36); }
    memcpy(&p,g_mem+c->esp+4,4);
    if (!p || p>DIAG_MEM_SIZE-48) { fprintf(stderr,"[diag] invalid window class\n"); exit(36); }
    memcpy(&size,g_mem+p,4);
    if (size!=48 || diag_window_proc) { fprintf(stderr,"[diag] unsupported window class\n"); exit(30); }
    memcpy(&diag_window_proc,g_mem+p+8,4);
    memcpy(&diag_window_class_style,g_mem+p+4,4);
    fprintf(stderr,"[diag] registered synthetic window class, proc=%08x (no window)\n",diag_window_proc);
    c->eax=0xc001; c->esp+=8;
}

void imp_USER32_AdjustWindowRectEx(CPU *c) {
    uint32_t a[4];
    if (c->esp>DIAG_MEM_SIZE-20) { fprintf(stderr,"[diag] invalid window rect stack\n"); exit(36); }
    memcpy(a,g_mem+c->esp+4,sizeof a);
    if (!a[0] || a[0]>DIAG_MEM_SIZE-16) { fprintf(stderr,"[diag] invalid window rectangle\n"); exit(36); }
    if (a[1]!=0x02cb0000u || a[2] || a[3]!=0x300) { fprintf(stderr,"[diag] unsupported window rectangle style\n"); exit(30); }
    fprintf(stderr,"[diag] AdjustWindowRectEx headless approximation: no borders\n");
    c->eax=1; c->esp+=20;
}

void imp_USER32_CreateWindowExA(CPU *c) {
    uint32_t a[12];
    if (c->esp>DIAG_MEM_SIZE-52) { fprintf(stderr,"[diag] invalid CreateWindowExA stack\n"); exit(36); }
    memcpy(a,g_mem+c->esp+4,sizeof a);
    fprintf(stderr,"[diag] CreateWindowExA class=%08x title=%08x style=%08x x=%d y=%d w=%d h=%d\n",a[1],a[2],a[3],(int32_t)a[4],(int32_t)a[5],(int32_t)a[6],(int32_t)a[7]);
    if (!diag_window_proc || a[3]!=0x02cb0000u || a[6]!=640 || a[7]!=480) {
        fprintf(stderr,"[diag] unsupported synthetic window\n"); exit(30);
    }
    fprintf(stderr,"[diag] headless placeholder: no actual window created\n");
    c->eax=0x10010; c->esp+=52;
}

void imp_USER32_GetWindowRect(CPU *c) {
    uint32_t a[2]; const int32_t rect[4]={0,0,640,480};
    if (c->esp>DIAG_MEM_SIZE-12) { fprintf(stderr,"[diag] invalid GetWindowRect stack\n"); exit(36); }
    memcpy(a,g_mem+c->esp+4,sizeof a);
    if (a[0]!=0x10010 || !a[1] || a[1]>DIAG_MEM_SIZE-sizeof rect) { fprintf(stderr,"[diag] unsupported GetWindowRect arguments\n"); exit(30); }
    memcpy(g_mem+a[1],rect,sizeof rect); c->eax=1; c->esp+=12;
    fprintf(stderr,"[diag] GetWindowRect headless approximation 640x480\n");
}

void imp_USER32_GetWindowLongA(CPU *c) {
    uint32_t a[2];
    if (c->esp>DIAG_MEM_SIZE-12) { fprintf(stderr,"[diag] invalid GetWindowLongA stack\n"); exit(36); }
    memcpy(a,g_mem+c->esp+4,sizeof a);
    fprintf(stderr,"[diag] GetWindowLongA hwnd=%08x index=%d\n",a[0],(int32_t)a[1]);
    if (a[0]!=0x10010) { fprintf(stderr,"[diag] unsupported window\n"); exit(30); }
    if ((int32_t)a[1]==-16) c->eax=diag_window_style;
    else if ((int32_t)a[1]==-20) c->eax=diag_window_exstyle;
    else if ((int32_t)a[1]==-4) c->eax=diag_window_proc;
    else { fprintf(stderr,"[diag] unsupported GetWindowLongA index\n"); exit(30); }
    c->esp+=12;
}

void imp_USER32_SetWindowLongA(CPU *c) {
    uint32_t a[3], *slot;
    if (c->esp>DIAG_MEM_SIZE-16) { fprintf(stderr,"[diag] invalid SetWindowLongA stack\n"); exit(36); }
    memcpy(a,g_mem+c->esp+4,sizeof a);
    if (a[0]!=0x10010) { fprintf(stderr,"[diag] unsupported SetWindowLongA window\n"); exit(30); }
    if ((int32_t)a[1]==-16) slot=&diag_window_style;
    else if ((int32_t)a[1]==-20) slot=&diag_window_exstyle;
    else if ((int32_t)a[1]==-4) slot=&diag_window_proc;
    else { fprintf(stderr,"[diag] unsupported SetWindowLongA index=%d\n",(int32_t)a[1]); exit(30); }
    fprintf(stderr,"[diag] SetWindowLongA index=%d value=%08x (headless)\n",(int32_t)a[1],a[2]);
    c->eax=*slot; *slot=a[2]; c->esp+=16;
}

void imp_USER32_RedrawWindow(CPU *c) {
    uint32_t a[4];
    if (c->esp>DIAG_MEM_SIZE-20) { fprintf(stderr,"[diag] invalid RedrawWindow stack\n"); exit(36); }
    memcpy(a,g_mem+c->esp+4,sizeof a);
    if (a[0]!=0x10010 || a[1] || a[2]) { fprintf(stderr,"[diag] unsupported RedrawWindow\n"); exit(30); }
    fprintf(stderr,"[diag] RedrawWindow skipped (headless) flags=%x\n",a[3]); c->eax=1; c->esp+=20;
}

void imp_USER32_RegisterDeviceNotificationA(CPU *c) {
    uint32_t a[3];
    if (c->esp>DIAG_MEM_SIZE-16) { fprintf(stderr,"[diag] invalid RegisterDeviceNotificationA stack\n"); exit(36); }
    memcpy(a,g_mem+c->esp+4,sizeof a);
    fprintf(stderr,"[diag] RegisterDeviceNotificationA recipient=%08x filter=%08x flags=%x\n",a[0],a[1],a[2]);
    if (a[0]!=0x10010 || !a[1] || a[1]>DIAG_MEM_SIZE-12 || a[2] || c->fs_base>DIAG_MEM_SIZE-0x38) {
        fprintf(stderr,"[diag] unsupported notification arguments\n"); exit(30);
    }
    fprintf(stderr,"[diag] synthetic notification registration: hotplug events not yet forwarded\n");
    c->eax=0xf6000001u; c->esp+=16;
}

void imp_USER32_LoadStringA(CPU *c) {
    uint32_t a[4];
    if (c->esp>DIAG_MEM_SIZE-20) { fprintf(stderr,"[diag] invalid LoadStringA stack\n"); exit(36); }
    memcpy(a,g_mem+c->esp+4,sizeof a);
    fprintf(stderr,"[diag] LoadStringA module=%08x id=%u buffer=%08x cap=%u ebp=%08x not implemented\n",a[0],a[1],a[2],a[3],c->ebp);
    if (a[1]==109) {
        uint32_t caps[4]; memcpy(&caps[0],g_mem+0x72c698+0x98,4); memcpy(&caps[1],g_mem+0x72c698+0xcc,4);
        memcpy(&caps[2],g_mem+0x72c698+0xc4,4); memcpy(&caps[3],g_mem+0x72c698+0x3c,4);
        fprintf(stderr,"[diag] checked caps stages=%x ps=%08x vs=%08x texture=%08x\n",caps[0],caps[1],caps[2],caps[3]);
    }
    if (c->esp<=DIAG_MEM_SIZE-1052) { uint32_t ret; memcpy(&ret,g_mem+c->esp+1048,4); fprintf(stderr,"[diag] error-report caller=%08x\n",ret); }
    for (uint32_t p=c->ebp, i=0; i<8 && p>=0x200000 && p<=0x300000-8; i++) {
        uint32_t next,ret; memcpy(&next,g_mem+p,4); memcpy(&ret,g_mem+p+4,4);
        fprintf(stderr,"[diag] frame ebp=%08x return=%08x next=%08x\n",p,ret,next);
        if (next<=p) break; p=next;
    }
    exit(30);
}

/* Virtual mouse for the game's cursor screens (puzzles, events: F_004dd130 follows the mouse once it is "active", exactly like
   on a PC with a mouse). The Vita has none: while such a screen is updated (diag_cursor_ui_tick, cursor_ui_unit.py) the LEFT
   STICK moves a cursor kept in the game's own 512-wide space and CROSS is the left button (GetKeyState VK_LBUTTON). The game
   converts the position it reads with x*512/window width, so the screen position is the inverse of that formula. Outside those
   screens the position never changes (the game then treats the mouse as idle) and the button reads released. */
static volatile unsigned diag_cursor_ui_last=0x7fffffffu;
/* Called by the bounded collision-list walk (loop_guard_unit.py) when it gives up: log the registers and the start of both lists. */
void diag_loop_guard_trip(unsigned fn,unsigned a_ecx,unsigned a_eax,unsigned a_esi,unsigned a_edi) {
    static unsigned trips; if (trips++>=6) return;
    extern unsigned g_frame;
    fprintf(stderr,"[loop-guard] F_%08x gave up at frame %u: ecx=%08x eax=%08x esi=%08x edi=%08x stage=%u room=%u\n",fn,g_frame,a_ecx,a_eax,a_esi,a_edi,
            (unsigned)g_mem[0x70e66d9u],(unsigned)*(uint32_t *)(g_mem+0x897e48u));
    unsigned bases[2]={a_ecx,a_esi};
    for (unsigned k=0;k<2;k++) if (bases[k]<DIAG_MEM_SIZE-80) {
        fprintf(stderr,"[loop-guard] list %c @%08x:",k?'i':'o',bases[k]);
        for (unsigned i=0;i<16;i++) { uint32_t w; memcpy(&w,g_mem+bases[k]+4*i,4); fprintf(stderr," %08x",w); }
        fprintf(stderr,"\n");
    }
}
void diag_cursor_ui_tick(void) { extern unsigned g_frame; diag_cursor_ui_last=g_frame; }
static float vc_x=256.0f, vc_y=256.0f; static int vc_click, vc_seen;   /* vc_seen: a cursor screen was shown at least once */
static int vc_active(void) {
    extern unsigned g_frame;
    static int test=-1; if (test<0) test=getenv("SH3_DIAG_VCURSOR_TEST")!=NULL;
    if (test) { uint32_t ui; memcpy(&ui,g_mem+0xbcc660u,4); return ui==11; }   /* check of the plumbing (puzzle screen only) without reaching a puzzle (PC env var, Vita marker vcursortest) */
    return (unsigned)(g_frame-diag_cursor_ui_last)<=3u;
}
/* 1 while the stick drives the cursor (the pad keys for it are then muted in dinput.c) */
int diag_vcursor_stick_mode(void) { return vc_active(); }
static void vc_update(void) {
    extern unsigned g_frame; static unsigned last_frame=~0u; static uint64_t last_ms;
    if (g_frame==last_frame) return; last_frame=g_frame;
    uint64_t now=SDL_GetTicks64(); float dt=last_ms?(float)(now-last_ms)/1000.0f:0.0f; last_ms=now; if (dt>0.1f) dt=0.1f;
    float sx=0,sy=0; int cross=0;
#ifdef __vita__
    SceCtrlData pad;
    if (sceCtrlPeekBufferPositiveExt2(0,&pad,1)>0 && !(pad.buttons&SCE_CTRL_INTERCEPTED)) {
        sx=((int)pad.lx-128)/128.0f; sy=((int)pad.ly-128)/128.0f; cross=!!(pad.buttons&SCE_CTRL_CROSS);
        /* The D-pad does exactly what the left stick does (checked on a PS2, Valentin 2026-11-17). */
        if (pad.buttons&SCE_CTRL_LEFT) sx=-1.0f; if (pad.buttons&SCE_CTRL_RIGHT) sx=1.0f;
        if (pad.buttons&SCE_CTRL_UP) sy=-1.0f; if (pad.buttons&SCE_CTRL_DOWN) sy=1.0f;
    }
    if (getenv("SH3_DIAG_VCURSOR_TEST")) cross=((g_frame/60u)%3u)==0;
    if (getenv("SH3_DIAG_VCURSOR_SWEEP")) { unsigned k=(g_frame/120u)%40u; vc_x=60.0f+60.0f*(float)(k%8u); vc_y=110.0f+80.0f*(float)(k/8u); cross=(g_frame%120u)>60u; }   /* Vita test: grid of 40 positions, click in each */   /* Vita test: a one-second click every three seconds, no stick */
#else
    if (getenv("SH3_DIAG_VCURSOR_TEST")) {   /* PC test: the cursor circles, with a click pulse every 2 s */
        float t=(float)now/1000.0f; sx=cosf(t*2.0f); sy=sinf(t*2.0f); cross=((int)(t*2.0f)&3)==0;
    }
#endif
    {   /* test build log: cursor screens entered/left and clicks (bounded) */
        static int was_active,was_down; static unsigned lines; int act=vc_active();
        if (lines<200 && (act!=was_active || (act && cross && !was_down))) { lines++;
            fprintf(stderr,"[cursor] frame=%u %s pos=%.0f,%.0f ui=%u stage=%u room=%u\n",g_frame,act!=was_active?(act?"screen-on":"screen-off"):"click",
                    vc_x,vc_y,*(uint32_t*)(g_mem+0xbcc660u),(unsigned)g_mem[0x70e66d9u],*(uint32_t*)(g_mem+0x897e48u)); }
        was_active=act; was_down=act && cross;
    }
    if (!vc_active()) { vc_click=0; return; }
    if (!vc_seen) { const char *pos=getenv("SH3_DIAG_VCURSOR_POS"); float px,py; if (pos && sscanf(pos,"%f %f",&px,&py)==2) { vc_x=px; vc_y=py; } }
    vc_seen=1;
    float m=sqrtf(sx*sx+sy*sy);
    if (m>0.2f) {   /* dead zone, then a curve that stays precise near the centre */
        float k=(m-0.2f)/0.8f; k*=k; float sp=280.0f*k/m;
        vc_x+=sx*sp*dt; vc_y+=sy*sp*dt;
        if (vc_x<0) vc_x=0; if (vc_x>511) vc_x=511; if (vc_y<0) vc_y=0; if (vc_y>511) vc_y=511;
    }
    vc_click=cross;
}
/* 1 while a cursor screen (puzzle...) updates its pointer: the game's own cursor function ran in the last frames. */
int diag_cursor_screen_active(void) { extern unsigned g_frame; return (unsigned)(g_frame-diag_cursor_ui_last)<=3u; }
void imp_USER32_GetCursorPos(CPU *c) {
    uint32_t p;
    if (c->esp>DIAG_MEM_SIZE-8) { fprintf(stderr,"[diag] invalid GetCursorPos stack\n"); exit(36); }
    memcpy(&p,g_mem+c->esp+4,4);
    if (!p || p>DIAG_MEM_SIZE-8) { fprintf(stderr,"[diag] invalid cursor output\n"); exit(36); }
    vc_update();
    uint32_t w=0,h=0; memcpy(&w,g_mem+0x72c780u,4); memcpy(&h,g_mem+0x72c784u,4);
    if (!w || w>8192) w=960; if (!h || h>8192) h=544;
    int32_t xy[2]={0,0};   /* as before until a cursor screen has been shown: no phantom mouse move at start */
    if (vc_seen) { xy[0]=(int32_t)(vc_x*(float)w/512.0f+0.5f); xy[1]=(int32_t)(vc_y*(float)h/512.0f+0.5f);
        /* The game (F_006008f0 poll) calls the mouse "idle" the first frame its position does not change and then ignores the
           button, so a still pointer could never click. A one-pixel flip each frame keeps it "moving" (Valentin 2026-11-17: click did nothing). */
        { extern unsigned g_frame; if (vc_active()) xy[0]+=(int32_t)(g_frame&1u); } }
    memcpy(g_mem+p,xy,8); c->eax=1; c->esp+=8;
}

void imp_USER32_GetClassLongA(CPU *c) {
    uint32_t a[2];
    if (c->esp>DIAG_MEM_SIZE-12) { fprintf(stderr,"[diag] invalid GetClassLongA stack\n"); exit(36); }
    memcpy(a,g_mem+c->esp+4,sizeof a);
    if (a[0]!=0x10010 || (int32_t)a[1]!=-26) { fprintf(stderr,"[diag] unsupported GetClassLongA\n"); exit(30); }
    c->eax=diag_window_class_style; c->esp+=12;
}

void imp_USER32_IsIconic(CPU *c) {
    uint32_t hwnd;
    if (c->esp>DIAG_MEM_SIZE-8) { fprintf(stderr,"[diag] invalid IsIconic stack\n"); exit(36); }
    memcpy(&hwnd,g_mem+c->esp+4,4);
    if (hwnd!=0x10010) { fprintf(stderr,"[diag] unsupported IsIconic window\n"); exit(30); }
    c->eax=0; c->esp+=8; /* synthetic window is never minimized */
}

void imp_USER32_SetWindowPos(CPU *c) {
    uint32_t a[7];
    if (c->esp>DIAG_MEM_SIZE-32) { fprintf(stderr,"[diag] invalid SetWindowPos stack\n"); exit(36); }
    memcpy(a,g_mem+c->esp+4,sizeof a);
    fprintf(stderr,"[diag] SetWindowPos hwnd=%08x insert=%08x x=%d y=%d w=%d h=%d flags=%x\n",a[0],a[1],(int32_t)a[2],(int32_t)a[3],(int32_t)a[4],(int32_t)a[5],a[6]);
    int wide_resize=getenv("SH3_DIAG_WIDE_NATIVE") && a[6]==4 &&
        ((a[4]==640 && a[5]==480) || (a[4]==960 && a[5]==544) || (a[4]==g_diag_res_w && a[5]==g_diag_res_h));
    if (a[0]!=0x10010 || (a[1]!=0xffffffffu && a[1]!=0) || (a[6]!=0x13 && a[6]!=3 && !wide_resize)) { fprintf(stderr,"[diag] unsupported SetWindowPos\n"); exit(30); }
    fprintf(stderr,"[diag] headless SetWindowPos ignores topmost state\n"); c->eax=1; c->esp+=32;
}

void imp_WINMM_timeBeginPeriod(CPU *c) {
    uint32_t period;
    if (c->esp>DIAG_MEM_SIZE-8) { fprintf(stderr,"[diag] invalid timer period stack\n"); exit(36); }
    memcpy(&period,g_mem+c->esp+4,4);
    if (period!=1) { fprintf(stderr,"[diag] unsupported timer period %u\n",period); exit(30); }
    if (DIAG_FLAG("SH3_DIAG_VERBOSE")) fprintf(stderr,"[diag] timeBeginPeriod(1): resolution unchanged (SH4 approximation)\n");
    c->eax=0; c->esp+=8;
}
void imp_WINMM_timeEndPeriod(CPU *c) {
    uint32_t period;
    if (c->esp>DIAG_MEM_SIZE-8) { fprintf(stderr,"[diag] invalid timeEndPeriod stack\n"); exit(36); }
    memcpy(&period,g_mem+c->esp+4,4);
    if (period!=1) { fprintf(stderr,"[diag] unsupported end timer period\n"); exit(30); }
    c->eax=0; c->esp+=8;
}

static Uint32 diag_timer_callback(Uint32 interval,void *opaque) {
    (void)interval; DiagTimer *t=opaque;
    pthread_mutex_lock(&t->lock);
    if (DIAG_FLAG("SH3_DIAG_VERBOSE")) fprintf(stderr,"[diag] experimental one-shot timer callback %08x\n",t->callback);
    rt_guest_call(&t->cpu,t->callback,5,t->handle,0u,t->user,0u,0u);
    pthread_mutex_unlock(&t->lock);
    return 0;
}
void imp_WINMM_timeSetEvent(CPU *c) {
    uint32_t a[5];
    if (c->esp>DIAG_MEM_SIZE-24) { fprintf(stderr,"[diag] invalid timeSetEvent stack\n"); exit(36); }
    memcpy(a,g_mem+c->esp+4,sizeof a);
    if (DIAG_FLAG("SH3_DIAG_VERBOSE")) fprintf(stderr,"[diag] timeSetEvent delay=%u resolution=%u callback=%08x user=%08x flags=%x\n",a[0],a[1],a[2],a[3],a[4]);
    if (!getenv("SH3_DIAG_THREADS") || !a[0] || a[0]>1000 || a[1] || a[4] || !rt_lookup(a[2])) {
        fprintf(stderr,"[diag] unsupported multimedia timer\n"); exit(30);
    }
    if (diag_timer) {
        DiagTimer *t=diag_timer;
        if (c!=&t->cpu || !t->self_killed || a[2]!=t->callback || a[3]!=t->user) {
            fprintf(stderr,"[diag] unsupported multimedia timer reuse\n"); exit(30);
        }
        t->self_killed=0; t->timer=SDL_AddTimer(a[0],diag_timer_callback,t);
        if (!t->timer) { fprintf(stderr,"[diag] SDL_AddTimer reuse failed\n"); exit(36); }
        c->eax=t->handle; c->esp+=24; return;
    }
    DiagTimer *t=calloc(1,sizeof *t); if (!t) { fprintf(stderr,"[diag] timer allocation failed\n"); exit(36); }
    if (pthread_mutex_init(&t->lock,NULL)) { fprintf(stderr,"[diag] timer lock failed\n"); exit(36); }
    const uint32_t stack_size=0x40000;
    t->stack=diag_heap_alloc(diag_guest_heap,stack_size,0); t->tib=diag_heap_alloc(diag_guest_heap,0x1000,1);
    if (!t->stack || !t->tib) { fprintf(stderr,"[diag] timer memory exhausted\n"); exit(36); }
    t->callback=a[2]; t->user=a[3]; t->handle=0xfa000001u;
    t->cpu.fs_base=t->tib; t->cpu.esp=t->stack+stack_size; t->cpu.fcw=0x37f;
    uint32_t empty=0xffffffffu,top=t->stack+stack_size,tid=0x200;
    memcpy(g_mem+t->tib,&empty,4); memcpy(g_mem+t->tib+4,&top,4); memcpy(g_mem+t->tib+8,&t->stack,4);
    memcpy(g_mem+t->tib+0x18,&t->tib,4); memcpy(g_mem+t->tib+0x24,&tid,4);
    diag_timer=t; t->timer=SDL_AddTimer(a[0],diag_timer_callback,t);
    if (!t->timer) { fprintf(stderr,"[diag] SDL_AddTimer failed: %s\n",SDL_GetError()); exit(36); }
    c->eax=t->handle; c->esp+=24;
}

void imp_WINMM_timeKillEvent(CPU *c) {
    uint32_t h,tid=0;
    if (c->esp>DIAG_MEM_SIZE-8) { fprintf(stderr,"[diag] invalid timeKillEvent stack\n"); exit(36); }
    memcpy(&h,g_mem+c->esp+4,4); if(c->fs_base<=DIAG_MEM_SIZE-0x28)memcpy(&tid,g_mem+c->fs_base+0x24,4);
    if (!diag_timer || h!=diag_timer->handle || tid!=0x200) {
        fprintf(stderr,"[diag] external timeKillEvent %08x caller=%u unsupported\n",h,tid); exit(30);
    }
    if (DIAG_FLAG("SH3_DIAG_VERBOSE")) fprintf(stderr,"[diag] one-shot timer cancelled by its own callback\n");
    diag_timer->self_killed=1;
    c->eax=0; c->esp+=8; /* callback returns zero to SDL and is not rescheduled */
}

#ifdef SH3_MOVIE_LAB
static DiagMessageQueue *diag_window_queue;
static pthread_once_t diag_window_queue_once=PTHREAD_ONCE_INIT;
static void diag_window_queue_init(void) { diag_window_queue=diag_message_create(128); }
int diag_movie_post_message(uint32_t hwnd,uint32_t message,uint32_t wparam,uint32_t lparam) {
    if (!DIAG_FLAG("SH3_DIAG_MSG_QUEUE") || hwnd!=0x10010 || !message) return -1;
    pthread_once(&diag_window_queue_once,diag_window_queue_init);
    DiagMessage m={hwnd,message,wparam,lparam,(uint32_t)SDL_GetTicks64(),0,0};
    return diag_message_post(diag_window_queue,&m);
}
#endif
void imp_USER32_PeekMessageA(CPU *c) {
    uint32_t a[5];
    if (c->esp>DIAG_MEM_SIZE-24) { fprintf(stderr,"[diag] invalid PeekMessageA stack\n"); exit(36); }
    memcpy(a,g_mem+c->esp+4,sizeof a);
    if (!a[0] || a[0]>DIAG_MEM_SIZE-28 || (a[1] && a[1]!=0x10010) || a[2] || a[3] || (a[4]&~1u)) {
        fprintf(stderr,"[diag] unsupported PeekMessageA args\n"); exit(30);
    }
#ifdef SH3_MOVIE_LAB
    if (DIAG_FLAG("SH3_DIAG_MSG_QUEUE")) {
        if(c!=diag_main_cpu) { fprintf(stderr,"[diag] message peek from worker unsupported\n"); exit(30); }
        pthread_once(&diag_window_queue_once,diag_window_queue_init);
        DiagMessage m; int result=diag_message_peek(diag_window_queue,&m,(int)(a[4]&1));
        if(result<0) { fprintf(stderr,"[diag] message queue failure\n"); exit(36); }
        if(result) { memcpy(g_mem+a[0],&m,sizeof m); c->eax=1; c->esp+=24; return; }
    }
#endif
    c->eax=0; c->esp+=24; /* no queued message */
}
void imp_USER32_TranslateMessage(CPU *c) {
#ifdef SH3_MOVIE_LAB
    if (DIAG_FLAG("SH3_DIAG_MSG_QUEUE") && c==diag_main_cpu && c->esp<=DIAG_MEM_SIZE-8) {
        uint32_t p; memcpy(&p,g_mem+c->esp+4,4);
        if (p && p<=DIAG_MEM_SIZE-28) {
            DiagMessage m; memcpy(&m,g_mem+p,sizeof m);
            /* TranslateMessage returns false for this non-keyboard notification. */
            if (m.hwnd==0x10010 && m.message==0x40d) { c->eax=0; c->esp+=8; return; }
        }
    }
#endif
    fprintf(stderr,"[diag] TranslateMessage unsupported outside the movie notification\n"); exit(30);
}
void imp_USER32_DispatchMessageA(CPU *c) {
#ifdef SH3_MOVIE_LAB
    if(DIAG_FLAG("SH3_DIAG_MSG_QUEUE") && c==diag_main_cpu && c->esp<=DIAG_MEM_SIZE-8) {
        uint32_t p; memcpy(&p,g_mem+c->esp+4,4);
        if(p && p<=DIAG_MEM_SIZE-28 && diag_window_proc) {
            DiagMessage m; memcpy(&m,g_mem+p,sizeof m);
            if(m.hwnd==0x10010) {
                uint32_t result=rt_guest_call(c,diag_window_proc,4,m.hwnd,m.message,m.wparam,m.lparam);
                c->eax=result; c->esp+=8; return;
            }
        }
    }
#endif
    fprintf(stderr,"[diag] DispatchMessageA unsupported outside message laboratory\n"); exit(30);
}
void imp_USER32_GetQueueStatus(CPU *c) {
    uint32_t flags,ret;
    if(c->esp>DIAG_MEM_SIZE-8) { fprintf(stderr,"[diag] invalid GetQueueStatus stack\n"); exit(36); }
    memcpy(&flags,g_mem+c->esp+4,4);memcpy(&ret,g_mem+c->esp,4);
#ifdef SH3_MOVIE_LAB
    if(DIAG_FLAG("SH3_DIAG_MSG_QUEUE") && c==diag_main_cpu) {
        pthread_once(&diag_window_queue_once,diag_window_queue_init);
        DiagMessage m; int n=diag_message_peek(diag_window_queue,&m,0);
        if(n==0) { fprintf(stderr,"[diag] GetQueueStatus empty flags=%08x caller=%08x\n",flags,ret); c->eax=0;c->esp+=8;return; }
    }
#endif
    fprintf(stderr,"[diag] GetQueueStatus unsupported flags=%08x caller=%08x\n",flags,ret);exit(30);
}
extern int diag_window_show(int show);
void imp_USER32_ShowWindow(CPU *c) {
    uint32_t a[2];
    if (c->esp>DIAG_MEM_SIZE-12) { fprintf(stderr,"[diag] invalid ShowWindow stack\n"); exit(36); }
    memcpy(a,g_mem+c->esp+4,sizeof a);
    if (a[0]!=0x10010 || (a[1]!=0 && a[1]!=1 && a[1]!=5 && a[1]!=10)) {
        fprintf(stderr,"[diag] unsupported ShowWindow %08x command=%u\n",a[0],a[1]); exit(30);
    }
    c->eax=diag_window_show(a[1]!=0); c->esp+=12;
}
void imp_USER32_UpdateWindow(CPU *c) {
    uint32_t hwnd;
    if (c->esp>DIAG_MEM_SIZE-8) { fprintf(stderr,"[diag] invalid UpdateWindow stack\n"); exit(36); }
    memcpy(&hwnd,g_mem+c->esp+4,4);
    if (hwnd!=0x10010) { fprintf(stderr,"[diag] unsupported UpdateWindow\n"); exit(30); }
    fprintf(stderr,"[diag] UpdateWindow: WM_PAINT dispatch not implemented\n");
    c->eax=1; c->esp+=8;
}
void imp_USER32_ClipCursor(CPU *c) {
    uint32_t p;
    if (c->esp>DIAG_MEM_SIZE-8) { fprintf(stderr,"[diag] invalid ClipCursor stack\n"); exit(36); }
    memcpy(&p,g_mem+c->esp+4,4);
    if (p) { fprintf(stderr,"[diag] ClipCursor rect=%08x unsupported\n",p); exit(30); }
    extern void diag_window_ungrab(void);
    diag_window_ungrab(); c->eax=1; c->esp+=8;
}
void imp_USER32_SetCursor(CPU *c) {
    static uint32_t previous=0xf2000002u;
    uint32_t cursor;
    if (c->esp>DIAG_MEM_SIZE-8) { fprintf(stderr,"[diag] invalid SetCursor stack\n"); exit(36); }
    memcpy(&cursor,g_mem+c->esp+4,4);
    if (cursor) { fprintf(stderr,"[diag] SetCursor cursor=%08x unsupported\n",cursor); exit(30); }
    SDL_ShowCursor(SDL_DISABLE); c->eax=previous; previous=0; c->esp+=8;
}
void imp_USER32_GetForegroundWindow(CPU *c) {
    extern int diag_window_focused(void);
    c->eax=diag_window_focused()?0x10010:0; c->esp+=4;
}
void imp_USER32_EnumDisplaySettingsA(CPU *c) {
    uint32_t a[3];
    if (c->esp>DIAG_MEM_SIZE-16) { fprintf(stderr,"[diag] invalid EnumDisplaySettingsA stack\n"); exit(36); }
    memcpy(a,g_mem+c->esp+4,sizeof a);
    uint16_t size=0; if (a[2] && a[2]<=DIAG_MEM_SIZE-40) memcpy(&size,g_mem+a[2]+36,2);
    if (a[0] || size!=156 || a[2]>DIAG_MEM_SIZE-size) {
        fprintf(stderr,"[diag] unsupported EnumDisplaySettingsA device=%08x mode=%08x data=%08x size=%u\n",a[0],a[1],a[2],size); exit(30);
    }
    extern int diag_display_mode(uint32_t index,uint32_t *w,uint32_t *h,uint32_t *bits,uint32_t *hz);
    uint32_t w,h,bits,hz;
    int ok=diag_display_mode(a[1],&w,&h,&bits,&hz);
    if (ok) {
        uint32_t fields=0x007c0000u, flags=0;
        memcpy(g_mem+a[2]+40,&fields,4); memcpy(g_mem+a[2]+104,&bits,4);
        memcpy(g_mem+a[2]+108,&w,4); memcpy(g_mem+a[2]+112,&h,4);
        memcpy(g_mem+a[2]+116,&flags,4); memcpy(g_mem+a[2]+120,&hz,4);
        fprintf(stderr,"[diag] display mode %u -> %ux%u %u bpp %u Hz\n",a[1],w,h,bits,hz);
    }
    c->eax=!!ok; c->esp+=16;
}
void imp_USER32_ChangeDisplaySettingsA(CPU *c) {
    uint32_t a[2];
    if (c->esp>DIAG_MEM_SIZE-12) { fprintf(stderr,"[diag] invalid ChangeDisplaySettingsA stack\n"); exit(36); }
    memcpy(a,g_mem+c->esp+4,sizeof a);
    if (a[1]!=2 || !a[0] || a[0]>DIAG_MEM_SIZE-156) {
        fprintf(stderr,"[diag] ChangeDisplaySettingsA mode=%08x flags=%08x unsupported\n",a[0],a[1]); exit(30);
    }
    uint32_t bits,w,h,hz; uint16_t size;
    memcpy(&size,g_mem+a[0]+36,2); memcpy(&bits,g_mem+a[0]+104,4); memcpy(&w,g_mem+a[0]+108,4);
    memcpy(&h,g_mem+a[0]+112,4); memcpy(&hz,g_mem+a[0]+120,4);
    if (size!=156) { fprintf(stderr,"[diag] unsupported display mode size %u\n",size); exit(30); }
    extern int diag_display_mode_supported(uint32_t w,uint32_t h,uint32_t bits,uint32_t hz);
    int ok=diag_display_mode_supported(w,h,bits,hz);
    fprintf(stderr,"[diag] test display mode %ux%u %u bpp %u Hz -> %s\n",w,h,bits,hz,ok?"supported":"unavailable");
    c->eax=ok?0:(uint32_t)-2; c->esp+=12;
}
void imp_USER32_GetKeyState(CPU *c) {
    uint32_t key;
    if (c->esp>DIAG_MEM_SIZE-8) { fprintf(stderr,"[diag] invalid GetKeyState stack\n"); exit(36); }
    memcpy(&key,g_mem+c->esp+4,4);
    unsigned button=key==1?SDL_BUTTON_LEFT:key==2?SDL_BUTTON_RIGHT:key==4?SDL_BUTTON_MIDDLE:0;
    if (!button) { fprintf(stderr,"[diag] GetKeyState key=%08x unsupported\n",key); exit(30); }
    c->eax=((SDL_GetMouseState(NULL,NULL)&SDL_BUTTON(button))||(button==SDL_BUTTON_LEFT && vc_click))?0xff80u:0; c->esp+=8;   /* like Windows (-128): the game tests bit 7 of the LOW byte */
}
void imp_USER32_KillTimer(CPU *c) {
    uint32_t a[2];
    if (c->esp>DIAG_MEM_SIZE-12) rt_fatal("[diag] invalid KillTimer stack");
    memcpy(a,g_mem+c->esp+4,sizeof a);
    /* No USER32 window timers have been registered by this runtime. */
    fprintf(stderr,"[diag] KillTimer hwnd=%08x id=%08x -> no registered timer\n",a[0],a[1]);
    c->eax=0; c->esp+=12;
}
void imp_OLE32_CoCreateInstance(CPU *c) {
    uint32_t a[5];
    if (c->esp>DIAG_MEM_SIZE-24) { fprintf(stderr,"[diag] invalid CoCreateInstance stack\n"); exit(36); }
    memcpy(a,g_mem+c->esp+4,sizeof a);
    fprintf(stderr,"[diag] CoCreateInstance clsid=%08x outer=%08x context=%x iid=%08x out=%08x\n",a[0],a[1],a[2],a[3],a[4]);
    if (a[0] && a[0]<=DIAG_MEM_SIZE-16 && a[3] && a[3]<=DIAG_MEM_SIZE-16) {
        for (unsigned i=0;i<4;i++) { uint32_t cl,id; memcpy(&cl,g_mem+a[0]+i*4,4); memcpy(&id,g_mem+a[3]+i*4,4); fprintf(stderr,"[diag] COM guid[%u]=%08x iid[%u]=%08x\n",i,cl,i,id); }
    }
    if (!a[0] || a[0]>DIAG_MEM_SIZE-16 || !a[3] || a[3]>DIAG_MEM_SIZE-16 || !a[4] || a[4]>DIAG_MEM_SIZE-4 || a[1]) exit(30);
#ifdef SH3_MOVIE_LAB
    if (getenv("SH3_DIAG_MOVIES")) {
        extern int movie_cocreate(CPU *,uint32_t,uint32_t,uint32_t,uint32_t *);
        uint32_t hr;
        if (movie_cocreate(c,a[0],a[3],a[4],&hr)) { c->eax=hr; c->esp+=24; return; }
    }
#endif
    uint32_t clsid; memcpy(&clsid,g_mem+a[0],4);
    if (clsid!=0xe436ebb3u) exit(30);
    uint32_t zero=0; memcpy(g_mem+a[4],&zero,4);
    fprintf(stderr,"[diag] DirectShow FilterGraph unavailable: REGDB_E_CLASSNOTREG (no video)\n");
    c->eax=0x80040154u; c->esp+=24;
}

#ifdef SH3_MOVIE_LAB
static void diag_movie_wait_audit(CPU *c, uint32_t handle, uint32_t timeout, int result, uint64_t start) {
    static unsigned lines;
    if (!start || __atomic_load_n(&lines,__ATOMIC_RELAXED) >= 80) return;
    uint64_t elapsed = SDL_GetTicks64() - start;
    if (elapsed < 2) return;
    uint32_t caller=0; memcpy(&caller,g_mem+c->esp,4);
    if (__atomic_fetch_add(&lines,1,__ATOMIC_RELAXED) >= 80) return;
    fprintf(stderr,"[movie-wait-audit] caller=%08x handle=%08x timeout=%u result=%d elapsed_ms=%llu\n",
            caller,handle,timeout,result,(unsigned long long)elapsed);
}
#endif
void imp_KERNEL32_WaitForSingleObject(CPU *c) {
    uint32_t a[2];
    if (c->esp>DIAG_MEM_SIZE-12) { fprintf(stderr,"[diag] invalid WaitForSingleObject stack\n"); exit(36); }
    memcpy(a,g_mem+c->esp+4,sizeof a);
#ifdef SH3_MOVIE_LAB
    extern int movie_lab_receive_active(CPU *);
    static int wait_audit=-1; if (wait_audit<0) wait_audit=getenv("SH3_DIAG_MOVIE_WAIT_AUDIT")!=NULL;   /* per wait: read once */
    uint64_t movie_wait_start = wait_audit && movie_lab_receive_active(c) ? SDL_GetTicks64() : 0;
#endif
    if (DIAG_FLAG("SH3_DIAG_VERBOSE")) fprintf(stderr,"[diag] WaitForSingleObject handle=%08x timeout=%u\n",a[0],a[1]);
#if defined(SH3_MOVIE_HW_LAB) && defined(__vita__)
    /* Film loop 00404491: WaitForSingleObject(sample-ready event [0072c098], 100) before copying a sample into
       its texture. In direct AvPlayer mode no sample is ever delivered, so the original would always time out
       (~10 presents/s). Wait for the next decoded frame (same timeout bound) and report WAIT_TIMEOUT: the game
       skips its copy and draws its quad, which samples the AvPlayer texture. */
    {   extern int movie_hw_frame_wait(uint32_t ms);
        uint32_t caller,ev; memcpy(&caller,g_mem+c->esp,4); memcpy(&ev,g_mem+0x72c098u,4);
        if (caller==0x4044a9u && a[0]==ev && movie_hw_frame_wait(a[1])) { c->eax=258u; c->esp+=12; return; }
    }
#endif
    if ((a[0]&0xff000000u)==0xf9000000u) {
        DiagEvent *event=diag_event_handle_acquire(a[0]);
        if (!event) { fprintf(stderr,"[diag] invalid event handle\n"); exit(30); }
        int r=diag_event_wait(event,a[1]);
#ifdef SH3_MOVIE_LAB
        diag_movie_wait_audit(c,a[0],a[1],r,movie_wait_start);
#endif
        diag_event_destroy(event);
        if (r<0) { fprintf(stderr,"[diag] event wait failed\n"); exit(36); }
        c->eax=(uint32_t)r; c->esp+=12; return;
    }
    if (a[0]>=0xf8000000u && a[0]-0xf8000000u<diag_thread_count) {
        uint32_t result; int r=diag_thread_wait(diag_threads[a[0]-0xf8000000u]->thread,a[1],&result);
        if (r<0) { fprintf(stderr,"[diag] guest thread wait failed\n"); exit(36); }
        c->eax=(uint32_t)r; c->esp+=12; return;
    }
    if (a[0]>=0xf7000000u && a[0]-0xf7000000u<diag_semaphore_count) {
        unsigned i=a[0]-0xf7000000u;
        int r=diag_sem_wait(diag_semaphores[i],a[1]);
#ifdef SH3_MOVIE_LAB
        diag_movie_wait_audit(c,a[0],a[1],r,movie_wait_start);
#endif
        if (r<0) { fprintf(stderr,"[diag] semaphore wait failed\n"); exit(36); }
        c->eax=(uint32_t)r; c->esp+=12; return;
    }
    if (a[0]<0xf1000000u || a[0]-0xf1000000u>=diag_mutex_count) {
        uint32_t caller=0; memcpy(&caller,g_mem+c->esp,4);
        fprintf(stderr,"[diag] unsupported wait object handle=%08x timeout=%u caller=%08x cpu=%p esp=%08x\n",
                a[0],a[1],caller,(void *)c,c->esp); exit(30);
    }
    int r=a[1]==UINT32_MAX ? pthread_mutex_lock(&diag_mutexes[a[0]-0xf1000000u]) :
                             pthread_mutex_trylock(&diag_mutexes[a[0]-0xf1000000u]);
#ifdef SH3_MOVIE_LAB
    diag_movie_wait_audit(c,a[0],a[1],r,movie_wait_start);
#endif
    if(DIAG_FLAG("SH3_DIAG_MUTEX_TRACE")) { uint32_t ret=0,tid=0; memcpy(&ret,g_mem+c->esp,4); if(c->fs_base<=DIAG_MEM_SIZE-0x28)memcpy(&tid,g_mem+c->fs_base+0x24,4); fprintf(stderr,"[diag] mutex wait h=%08x result=%d guest_tid=%u caller=%08x\n",a[0],r,tid,ret); }
    if (!r) c->eax=0;
    else if (r==EBUSY && a[1]==0) c->eax=258;
    else { fprintf(stderr,"[diag] waiting on busy object unsupported\n"); exit(30); }
    c->esp+=12;
}
void imp_KERNEL32_WaitForMultipleObjects(CPU *c) {
    uint32_t a[4],ret=0;
    if(c->esp>DIAG_MEM_SIZE-20) { fprintf(stderr,"[diag] invalid multiple-wait stack\n"); exit(36); }
    memcpy(a,g_mem+c->esp+4,sizeof a);memcpy(&ret,g_mem+c->esp,4);
    fprintf(stderr,"[diag] WaitForMultipleObjects count=%u ptr=%08x all=%u timeout=%u caller=%08x",a[0],a[1],a[2],a[3],ret);
    if(a[0]<=64 && a[1]<=DIAG_MEM_SIZE-4*a[0]) for(unsigned i=0;i<a[0];i++) { uint32_t h;memcpy(&h,g_mem+a[1]+4*i,4);fprintf(stderr," %08x",h); }
    fputc('\n',stderr);
#ifdef SH3_MOVIE_LAB
    if(DIAG_FLAG("SH3_DIAG_MULTIWAIT_POLL") && a[0]>0 && a[0]<=64 && a[1] && a[1]<=DIAG_MEM_SIZE-4*a[0] && !a[2]) {
        DiagEvent *events[64]; unsigned acquired=0;
        for(unsigned i=0;i<a[0];i++) {
            uint32_t h;memcpy(&h,g_mem+a[1]+4*i,4);
            if(!(events[i]=diag_event_handle_acquire(h)))break;
            acquired++;
        }
        if(acquired==a[0]) {
            int result=diag_multiwait_events(events,a[0],a[3]);
            for(unsigned i=0;i<acquired;i++)diag_event_destroy(events[i]);
            if(result<0) { fprintf(stderr,"[diag] experimental multiple-event wait failed\n");exit(36); }
            fprintf(stderr,"[diag] experimental multiple-event wait -> %d\n",result);
            c->eax=(uint32_t)result;c->esp+=20;return;
        }
        for(unsigned i=0;i<acquired;i++)diag_event_destroy(events[i]);
    }
#endif
    fprintf(stderr,"[diag] multiple-object wait unsupported; stopping before fake success\n");exit(30);
}

void w32_event_set(uint32_t h,int value) {
    DiagEvent *event=diag_event_handle_acquire(h);
    if(!event){fprintf(stderr,"[diag] invalid notification event %08x\n",h);exit(30);}
    int result=diag_event_set(event,value);diag_event_destroy(event);
    if(result){fprintf(stderr,"[diag] event signal failed\n");exit(36);}
}
void imp_KERNEL32_CreateEventA(CPU *c) {
    uint32_t a[4];
    if (c->esp>DIAG_MEM_SIZE-20 || c->fs_base>DIAG_MEM_SIZE-0x38) { fprintf(stderr,"[diag] invalid CreateEventA stack\n"); exit(36); }
    memcpy(a,g_mem+c->esp+4,sizeof a);
    if (a[0] || a[3]) { fprintf(stderr,"[diag] unsupported named/security event\n"); exit(30); }
    c->eax=diag_event_handle_create(!!a[1],!!a[2]);
    if(!c->eax){diag_event_report();fprintf(stderr,"[diag] event creation failed\n");exit(36);} c->esp+=20;
    if (DIAG_FLAG("SH3_DIAG_VERBOSE")) fprintf(stderr,"[diag] CreateEventA manual=%u initial=%u -> %08x\n",!!a[1],!!a[2],c->eax);
}
static void diag_change_event(CPU *c,int value) {
    uint32_t h;
    if (c->esp>DIAG_MEM_SIZE-8) { fprintf(stderr,"[diag] invalid event stack\n"); exit(36); }
    memcpy(&h,g_mem+c->esp+4,4); w32_event_set(h,value); c->eax=1; c->esp+=8;
}
void imp_KERNEL32_SetEvent(CPU *c) { diag_change_event(c,1); }
void imp_KERNEL32_ResetEvent(CPU *c) { diag_change_event(c,0); }
void imp_KERNEL32_PulseEvent(CPU *c) {
    uint32_t h;
    if (c->esp>DIAG_MEM_SIZE-8) { fprintf(stderr,"[diag] invalid PulseEvent stack\n"); exit(36); }
    memcpy(&h,g_mem+c->esp+4,4);
    DiagEvent *event=diag_event_handle_acquire(h);
    if(!event){fprintf(stderr,"[diag] invalid pulse event\n");exit(30);}
    int result=diag_event_pulse(event);diag_event_destroy(event);
    if(result){fprintf(stderr,"[diag] event pulse failed\n");exit(36);}
    c->eax=1; c->esp+=8;
}

void imp_KERNEL32_CreateSemaphoreA(CPU *c) {
    uint32_t a[4];
    if (c->esp>DIAG_MEM_SIZE-20) { fprintf(stderr,"[diag] invalid CreateSemaphoreA stack\n"); exit(36); }
    memcpy(a,g_mem+c->esp+4,sizeof a);
    if (a[0] || a[3] || (int32_t)a[1]<0 || (int32_t)a[2]<=0 || a[1]>a[2] || diag_semaphore_count>=128) {
        fprintf(stderr,"[diag] unsupported semaphore parameters\n"); exit(30);
    }
    unsigned i=diag_semaphore_count;
    diag_semaphores[i]=diag_sem_create(a[1],a[2]);
    if (!diag_semaphores[i]) { fprintf(stderr,"[diag] semaphore creation failed\n"); exit(36); }
    diag_semaphore_count++;
    c->eax=0xf7000000u+i; c->esp+=20;
    fprintf(stderr,"[diag] CreateSemaphoreA initial=%u max=%u -> %08x\n",a[1],a[2],c->eax);
}

void imp_KERNEL32_ReleaseSemaphore(CPU *c) {
    uint32_t a[3], previous;
    if (c->esp>DIAG_MEM_SIZE-16) { fprintf(stderr,"[diag] invalid ReleaseSemaphore stack\n"); exit(36); }
    memcpy(a,g_mem+c->esp+4,sizeof a);
    if (a[0]<0xf7000000u || a[0]-0xf7000000u>=diag_semaphore_count || (int32_t)a[1]<=0 ||
        (a[2] && a[2]>DIAG_MEM_SIZE-4)) { fprintf(stderr,"[diag] unsupported ReleaseSemaphore\n"); exit(30); }
    int r=diag_sem_release(diag_semaphores[a[0]-0xf7000000u],a[1],&previous);
    if(r==1) {
        if(c->fs_base>DIAG_MEM_SIZE-0x38) { fprintf(stderr,"[diag] invalid semaphore error TIB\n"); exit(36); }
        uint32_t error=298,ret=0; memcpy(g_mem+c->fs_base+0x34,&error,4); memcpy(&ret,g_mem+c->esp,4);
        fprintf(stderr,"[diag] ReleaseSemaphore exceeds maximum h=%08x caller=%08x\n",a[0],ret);
        c->eax=0;c->esp+=16;return;
    }
    if(r<0) { fprintf(stderr,"[diag] ReleaseSemaphore host failure\n"); exit(36); }
    if (a[2]) memcpy(g_mem+a[2],&previous,4);
    c->eax=1; c->esp+=16;
}

static uint32_t diag_guest_entry(void *arg) {
    DiagGuestThread *t=arg;
#ifdef __vita__
    rt_cpu_profile_thread("guest");
    if (g_diag_perf&PERF_PREPTHREAD) sceKernelChangeThreadCpuAffinityMask(sceKernelGetThreadId(),SCE_KERNEL_CPU_MASK_USER_1);
    if (getenv("SH4_CPU_TIME")) {
        fprintf(stderr,"[diag] guest tid %u has Vita thread %08x\n",t->tid,(unsigned)sceKernelGetThreadId());
        /* This executable's CRT trampoline reads its target and argument at
           +0x4c/+0x50. Diagnostic only; never use these values to dispatch. */
        if (t->start==0x00665097u && t->arg<=DIAG_MEM_SIZE-0x54u) {
            uint32_t target, arg;
            memcpy(&target,g_mem+t->arg+0x4c,4); memcpy(&arg,g_mem+t->arg+0x50,4);
            fprintf(stderr,"[diag] guest tid %u CRT target=%08x arg=%08x\n",t->tid,target,arg);
        }
    }
#endif
    fprintf(stderr,"[diag] experimental guest thread %u entering %08x\n",t->tid,t->start);
    rt_call(&t->cpu,t->start);
    fprintf(stderr,"[diag] experimental guest thread %u returned %08x\n",t->tid,t->cpu.eax);
    return t->cpu.eax;
}

void imp_KERNEL32_CreateThread(CPU *c) {
    uint32_t a[6];
    if (c->esp>DIAG_MEM_SIZE-28) { fprintf(stderr,"[diag] invalid CreateThread stack\n"); exit(36); }
    memcpy(a,g_mem+c->esp+4,sizeof a);
    fprintf(stderr,"[diag] CreateThread security=%08x stack=%u start=%08x arg=%08x flags=%x tidptr=%08x\n",a[0],a[1],a[2],a[3],a[4],a[5]);
    if (!getenv("SH3_DIAG_THREADS") || c!=diag_main_cpu || a[0] || (a[1] && a[1]!=(1u<<20)) || (a[4] && a[4]!=4) ||
        (a[5] && a[5]>DIAG_MEM_SIZE-4) || !rt_lookup(a[2])) {
        fprintf(stderr,"[diag] guest threading disabled or unsupported\n"); exit(30);
    }
    pthread_mutex_lock(&diag_object_lock);
    if (diag_thread_count>=16) { fprintf(stderr,"[diag] experimental mode permits at most sixteen guest workers\n"); exit(30); }
    DiagGuestThread *t=calloc(1,sizeof *t);
    if (!t) { fprintf(stderr,"[diag] guest thread allocation failed\n"); exit(36); }
    const uint32_t stack_size=1u<<20;
    t->stack=diag_heap_alloc(diag_guest_heap,stack_size,0); t->tib=diag_heap_alloc(diag_guest_heap,0x1000,1);
    if (!t->stack || !t->tib) { fprintf(stderr,"[diag] guest thread memory exhausted\n"); exit(36); }
    t->start=a[2]; t->arg=a[3]; t->tid=0x101+diag_thread_count; t->cpu.fs_base=t->tib; t->cpu.esp=t->stack+stack_size-8; t->cpu.fcw=0x37f;
    uint32_t sentinel=0xfffffff0u, empty=0xffffffffu, top=t->stack+stack_size;
    memcpy(g_mem+t->cpu.esp,&sentinel,4); memcpy(g_mem+t->cpu.esp+4,&t->arg,4);
    memcpy(g_mem+t->tib,&empty,4); memcpy(g_mem+t->tib+4,&top,4); memcpy(g_mem+t->tib+8,&t->stack,4);
    memcpy(g_mem+t->tib+0x18,&t->tib,4); memcpy(g_mem+t->tib+0x24,&t->tid,4);
    if (a[5]) memcpy(g_mem+a[5],&t->tid,4);
    t->thread=a[4]?diag_thread_create_suspended(diag_guest_entry,t):diag_thread_create(diag_guest_entry,t);
    if (!t->thread) { fprintf(stderr,"[diag] host thread creation failed\n"); exit(36); }
    diag_threads[diag_thread_count]=t;
    c->eax=0xf8000000u+diag_thread_count++; c->esp+=28;
    pthread_mutex_unlock(&diag_object_lock);
}

void imp_KERNEL32_ResumeThread(CPU *c) {
    uint32_t h;
    if (c->esp>DIAG_MEM_SIZE-8) { fprintf(stderr,"[diag] invalid ResumeThread stack\n"); exit(36); }
    memcpy(&h,g_mem+c->esp+4,4);
    if (h<0xf8000000u || h-0xf8000000u>=diag_thread_count) { fprintf(stderr,"[diag] invalid thread handle\n"); exit(30); }
    int previous=diag_thread_resume(diag_threads[h-0xf8000000u]->thread);
    if (previous<0) { fprintf(stderr,"[diag] ResumeThread failed\n"); exit(36); }
    c->eax=(uint32_t)previous; c->esp+=8;
    if (DIAG_FLAG("SH3_DIAG_VERBOSE")) fprintf(stderr,"[diag] ResumeThread %08x previous=%d\n",h,previous);
}

void imp_KERNEL32_SuspendThread(CPU *c) {
    uint32_t h;
    if (c->esp>DIAG_MEM_SIZE-8) { fprintf(stderr,"[diag] invalid SuspendThread stack\n"); exit(36); }
    memcpy(&h,g_mem+c->esp+4,4);
    uint32_t tid=0; if (c->fs_base<=DIAG_MEM_SIZE-0x28) memcpy(&tid,g_mem+c->fs_base+0x24,4);
    if (h<0xf8000000u || h-0xf8000000u>=diag_thread_count || diag_threads[h-0xf8000000u]->tid!=tid) {
        fprintf(stderr,"[diag] external SuspendThread handle=%08x caller=%u unsupported\n",h,tid); exit(30);
    }
    if (DIAG_FLAG("SH3_DIAG_VERBOSE")) fprintf(stderr,"[diag] guest worker %u suspends itself\n",tid);
    if (diag_thread_self_suspend(diag_threads[h-0xf8000000u]->thread)) { fprintf(stderr,"[diag] self suspension failed\n"); exit(36); }
    c->eax=0; c->esp+=8;
}

void imp_KERNEL32_ReleaseMutex(CPU *c) {
    uint32_t h;
    if (c->esp>DIAG_MEM_SIZE-8) { fprintf(stderr,"[diag] invalid ReleaseMutex stack\n"); exit(36); }
    memcpy(&h,g_mem+c->esp+4,4);
    int r=(h<0xf1000000u || h-0xf1000000u>=diag_mutex_count) ? EINVAL : pthread_mutex_unlock(&diag_mutexes[h-0xf1000000u]);
    if(DIAG_FLAG("SH3_DIAG_MUTEX_TRACE") || r) { uint32_t ret=0,tid=0; memcpy(&ret,g_mem+c->esp,4); if(c->fs_base<=DIAG_MEM_SIZE-0x28)memcpy(&tid,g_mem+c->fs_base+0x24,4); fprintf(stderr,"[diag] mutex release h=%08x result=%d guest_tid=%u caller=%08x\n",h,r,tid,ret); }
    if(r==EPERM || r==EINVAL) {
        if(c->fs_base>DIAG_MEM_SIZE-0x38) { fprintf(stderr,"[diag] invalid mutex error TIB\n"); exit(36); }
        uint32_t error=r==EPERM?288u:6u; /* ERROR_NOT_OWNER / ERROR_INVALID_HANDLE */
        memcpy(g_mem+c->fs_base+0x34,&error,4); c->eax=0; c->esp+=8; return;
    }
    if(r) { fprintf(stderr,"[diag] mutex release failed: %d\n",r); exit(30); }
    c->eax=1; c->esp+=8;
}

void imp_KERNEL32_GetFileAttributesA(CPU *c) {
    uint32_t p;
    if (c->esp>DIAG_MEM_SIZE-8) { fprintf(stderr,"[diag] invalid GetFileAttributesA stack\n"); exit(36); }
    memcpy(&p,g_mem+c->esp+4,4);
    if (!p || p>=DIAG_MEM_SIZE || !memchr(g_mem+p,0,DIAG_MEM_SIZE-p)) { fprintf(stderr,"[diag] invalid file path\n"); exit(36); }
    const char *guest=(char *)g_mem+p;
    if (!strcasecmp(guest,"C:\\SILENT HILL 3\\savedata\\") || !strcasecmp(guest,"C:\\SILENT HILL 3\\savedata")) {
        struct stat st;
        if (stat(DIAG_SAVE_SOURCE,&st) || !S_ISDIR(st.st_mode)) { fprintf(stderr,"[diag] savedata directory unavailable\n"); exit(30); }
        c->eax=0x10; c->esp+=8; fprintf(stderr,"[diag] GetFileAttributesA savedata -> directory\n"); return;
    }
    char resolved[4096]; struct stat st; uint32_t value;
    int r=diag_data_path(guest,resolved,sizeof resolved);
    if (r<0) { fprintf(stderr,"[diag] unsupported attributes path %.200s\n",guest); exit(30); }
    if (!r) { if (c->fs_base>DIAG_MEM_SIZE-0x38) exit(36); value=0xffffffffu; uint32_t err=2; memcpy(g_mem+c->fs_base+0x34,&err,4); }
    else { if (stat(resolved,&st)) { fprintf(stderr,"[diag] data stat failed\n"); exit(36); } value=S_ISDIR(st.st_mode)?0x10u:0x80u; }
    fprintf(stderr,"[diag] GetFileAttributesA %.200s -> %08x\n",guest,value);
    c->eax=value; c->esp+=8;
}

/* Single diagnostic wildcard enumeration; retained until FindClose. */
static DIR *diag_save_find_dir;
static int diag_save_find_zero_prefix;
static int diag_save_find_next(uint32_t out) {
    struct dirent *entry;
    while ((entry=readdir(diag_save_find_dir))) {
        if (strlen(entry->d_name)!=8 || strncasecmp(entry->d_name,"data.",5)) continue;
        if (diag_save_find_zero_prefix && entry->d_name[5]!='0') continue;
        char path[4096];
        if (snprintf(path,sizeof path,"%s/%s",DIAG_SAVE_SCRATCH,entry->d_name)>=(int)sizeof path) continue;
        struct stat st;
        if (stat(path,&st) || !S_ISREG(st.st_mode)) continue;
        uint32_t attr=0x80,hi=(uint32_t)((uint64_t)st.st_size>>32),lo=(uint32_t)st.st_size;
        uint64_t ft=((uint64_t)st.st_mtime+11644473600ULL)*10000000ULL;
        memset(g_mem+out,0,320); memcpy(g_mem+out,&attr,4);
        memcpy(g_mem+out+4,&ft,8); memcpy(g_mem+out+12,&ft,8); memcpy(g_mem+out+20,&ft,8);
        memcpy(g_mem+out+28,&hi,4); memcpy(g_mem+out+32,&lo,4); memcpy(g_mem+out+44,entry->d_name,9);
        fprintf(stderr,"[diag] save enumeration found %.8s\n",entry->d_name);
        return 1;
    }
    return 0;
}
void imp_KERNEL32_FindFirstFileA(CPU *c) {
    uint32_t a[2];
    if (c->esp>DIAG_MEM_SIZE-12) { fprintf(stderr,"[diag] invalid FindFirstFileA stack\n"); exit(36); }
    memcpy(a,g_mem+c->esp+4,sizeof a);
    if (!a[0] || a[0]>=DIAG_MEM_SIZE || !memchr(g_mem+a[0],0,DIAG_MEM_SIZE-a[0])) { fprintf(stderr,"[diag] invalid find path\n"); exit(36); }
    const char *path=(char *)g_mem+a[0]; size_t n=strlen(path);
    fprintf(stderr,"[diag] FindFirstFileA path=%.200s output=%08x\n",path,a[1]);
    const char *absolute_prefix="C:\\SILENT HILL 3\\savedata\\data.";
    size_t absolute_len=strlen(absolute_prefix);
    const char *digits=NULL; int absolute_slot=0;
    if (!strncasecmp(path,absolute_prefix,absolute_len)) { digits=path+absolute_len; absolute_slot=1; }
    else if (!strncasecmp(path,"data.",5)) digits=path+5;
    if (digits && strlen(digits)==3 && digits[0]=='0' && digits[1]>='0' && digits[1]<='9' && digits[2]>='0' && digits[2]<='9') {
        if (c->fs_base>DIAG_MEM_SIZE-0x38 || !a[1] || a[1]>DIAG_MEM_SIZE-320) { fprintf(stderr,"[diag] invalid save find output\n"); exit(36); }
        char filename[4096]; struct stat st;
        if (snprintf(filename,sizeof filename,"%s/data.%c%c%c",absolute_slot?DIAG_SAVE_SCRATCH:DIAG_ROOT_DIR,
                     digits[0],digits[1],digits[2])>=(int)sizeof filename) exit(36);
        if (lstat(filename,&st)) {
            if (errno!=ENOENT) { fprintf(stderr,"[diag] cannot inspect save slot\n"); exit(36); }
            uint32_t error=2; memcpy(g_mem+c->fs_base+0x34,&error,4); c->eax=0xffffffffu; c->esp+=12; return;
        }
        if (!absolute_slot || !S_ISREG(st.st_mode)) { fprintf(stderr,"[diag] unexpected root or nonregular save slot\n"); exit(30); }
        uint32_t attr=0x80,hi=(uint32_t)((uint64_t)st.st_size>>32),lo=(uint32_t)st.st_size;
        uint64_t ft=((uint64_t)st.st_mtime+11644473600ULL)*10000000ULL;
        memset(g_mem+a[1],0,320); memcpy(g_mem+a[1],&attr,4);
        memcpy(g_mem+a[1]+4,&ft,8); memcpy(g_mem+a[1]+12,&ft,8); memcpy(g_mem+a[1]+20,&ft,8);
        memcpy(g_mem+a[1]+28,&hi,4); memcpy(g_mem+a[1]+32,&lo,4);
        snprintf((char *)g_mem+a[1]+44,260,"data.%c%c%c",digits[0],digits[1],digits[2]);
        fprintf(stderr,"[diag] diagnostic save slot found: %s\n",filename);
        c->eax=0xfb000001u; c->esp+=12; return;
    }
    if (!strcasecmp(path,"C:\\SILENT HILL 3\\savedata\\data.sys")) {
        struct stat st;
        if (!a[1] || a[1]>DIAG_MEM_SIZE-320 || stat(DIAG_SAVE_WRITE_FILE("data.sys"),&st) || !S_ISREG(st.st_mode)) {
            fprintf(stderr,"[diag] scratch data.sys unavailable for enumeration\n"); exit(30);
        }
        uint32_t attr=0x80, hi=(uint32_t)((uint64_t)st.st_size>>32), lo=(uint32_t)st.st_size;
        uint64_t ft=((uint64_t)st.st_mtime+11644473600ULL)*10000000ULL;
        memset(g_mem+a[1],0,320); memcpy(g_mem+a[1],&attr,4); memcpy(g_mem+a[1]+4,&ft,8);
        memcpy(g_mem+a[1]+12,&ft,8); memcpy(g_mem+a[1]+20,&ft,8); memcpy(g_mem+a[1]+28,&hi,4);
        memcpy(g_mem+a[1]+32,&lo,4); memcpy(g_mem+a[1]+44,"data.sys",9);
        fprintf(stderr,"[diag] scratch data.sys found (all times use mtime)\n");
        c->eax=0xfb000001u; c->esp+=12; return;
    }
    if (!strcasecmp(path,"data.sys")) {
        struct stat st;
        if (c->fs_base>DIAG_MEM_SIZE-0x38 || !a[1] || a[1]>DIAG_MEM_SIZE-320) { fprintf(stderr,"[diag] invalid find output\n"); exit(36); }
        if (!stat(DIAG_ROOT_FILE,&st)) { fprintf(stderr,"[diag] existing root data.sys enumeration unsupported\n"); exit(30); }
        if (errno!=ENOENT) { fprintf(stderr,"[diag] cannot inspect root data.sys\n"); exit(36); }
        uint32_t error=2; memcpy(g_mem+c->fs_base+0x34,&error,4); c->eax=0xffffffffu; c->esp+=12; return;
    }
    if ((!strcasecmp(path,"C:\\SILENT HILL 3\\savedata\\data.???") ||
         !strcasecmp(path,"C:\\SILENT HILL 3\\savedata\\data.0??")) && c->fs_base<=DIAG_MEM_SIZE-0x38) {
        if (!a[1] || a[1]>DIAG_MEM_SIZE-320 || diag_save_find_dir) { fprintf(stderr,"[diag] unsupported concurrent save enumeration or invalid output\n"); exit(30); }
        if (SH3_MKDIR(DIAG_SAVE_SCRATCH,0700) && errno!=EEXIST) { fprintf(stderr,"[diag] cannot create diagnostic save directory\n"); exit(36); }
        struct stat ds;
        if (lstat(DIAG_SAVE_SCRATCH,&ds) || !S_ISDIR(ds.st_mode)) { fprintf(stderr,"[diag] invalid diagnostic save directory\n"); exit(36); }
        diag_save_find_dir=opendir(DIAG_SAVE_SCRATCH);
        if (!diag_save_find_dir) { fprintf(stderr,"[diag] cannot inspect savedata\n"); exit(36); }
        diag_save_find_zero_prefix=!strcasecmp(path,"C:\\SILENT HILL 3\\savedata\\data.0??");
        if (diag_save_find_next(a[1])) { c->eax=0xfb000002u; c->esp+=12; return; }
        closedir(diag_save_find_dir); diag_save_find_dir=NULL;
        uint32_t error=2; memcpy(g_mem+c->fs_base+0x34,&error,4); c->eax=0xffffffffu; c->esp+=12; return;
    }
    if (n && path[n-1]=='\\' && c->fs_base<=DIAG_MEM_SIZE-0x38) {
        uint32_t error=3; memcpy(g_mem+c->fs_base+0x34,&error,4);
        c->eax=0xffffffffu; c->esp+=12; return;
    }
    fprintf(stderr,"[diag] FindFirstFileA pattern not implemented\n"); exit(30);
}
void imp_KERNEL32_FindClose(CPU *c) {
    uint32_t h;
    if (c->esp>DIAG_MEM_SIZE-8) { fprintf(stderr,"[diag] invalid FindClose stack\n"); exit(36); }
    memcpy(&h,g_mem+c->esp+4,4);
    if (h==0xfb000002u && diag_save_find_dir) { closedir(diag_save_find_dir); diag_save_find_dir=NULL; }
    else if (h!=0xfb000001u) { fprintf(stderr,"[diag] unsupported FindClose %08x\n",h); exit(30); }
    c->eax=1; c->esp+=8;
}
void imp_KERNEL32_FindNextFileA(CPU *c) {
    uint32_t a[2];
    if (c->esp>DIAG_MEM_SIZE-12 || c->fs_base>DIAG_MEM_SIZE-0x38) { fprintf(stderr,"[diag] invalid FindNextFileA stack\n"); exit(36); }
    memcpy(a,g_mem+c->esp+4,sizeof a);
    if ((a[0]!=0xfb000001u && (a[0]!=0xfb000002u || !diag_save_find_dir)) || !a[1] || a[1]>DIAG_MEM_SIZE-320) { fprintf(stderr,"[diag] unsupported FindNextFileA\n"); exit(30); }
    if (a[0]==0xfb000002u && diag_save_find_next(a[1])) { c->eax=1; c->esp+=12; return; }
    uint32_t error=18; memcpy(g_mem+c->fs_base+0x34,&error,4); c->eax=0; c->esp+=12;
}

void imp_KERNEL32_CreateDirectoryA(CPU *c) {
    uint32_t a[2]; struct stat st;
    if (c->esp>DIAG_MEM_SIZE-12 || c->fs_base>DIAG_MEM_SIZE-0x38) { fprintf(stderr,"[diag] invalid CreateDirectoryA stack\n"); exit(36); }
    memcpy(a,g_mem+c->esp+4,sizeof a);
    if (!a[0] || a[0]>=DIAG_MEM_SIZE || !memchr(g_mem+a[0],0,DIAG_MEM_SIZE-a[0])) { fprintf(stderr,"[diag] invalid directory name\n"); exit(36); }
    fprintf(stderr,"[diag] CreateDirectoryA %.200s\n",g_mem+a[0]);
    if (a[1] || (strcasecmp((char *)g_mem+a[0],"C:\\SILENT HILL 3\\savedata\\") &&
                 strcasecmp((char *)g_mem+a[0],"C:\\SILENT HILL 3\\savedata")) ||
        stat(DIAG_SAVE_SOURCE,&st) || !S_ISDIR(st.st_mode)) {
        fprintf(stderr,"[diag] unsupported directory creation\n"); exit(30);
    }
    uint32_t error=183; memcpy(g_mem+c->fs_base+0x34,&error,4); c->eax=0; c->esp+=12;
}

/* Every write of the save slots, data.sys and disp.ini goes to NAME.tmp and is renamed to NAME on CloseHandle, only if
   something was written (audit 2026-11-17): the game opens these files with CREATE_ALWAYS, so a crash or a power loss
   during a save (e.g. the open sleep/resume crash) used to leave a truncated save. Indexed by file slot. */
static char diag_commit_path[64][256];
/* Boot: a crash between "old file renamed to .bak" and "new file renamed in" leaves NAME missing and NAME.bak intact. */
static void diag_recover_commits(void) {
    const char *dirs[]={DIAG_SAVE_SCRATCH}; (void)dirs;
    DIR *d=opendir(DIAG_SAVE_SCRATCH); if (!d) return;
    struct dirent *e;
    while ((e=readdir(d))) {
        size_t n=strlen(e->d_name); if (n<5 || strcmp(e->d_name+n-4,".bak")) continue;
        char bak[512],fin[512]; snprintf(bak,sizeof bak,"%s/%s",DIAG_SAVE_SCRATCH,e->d_name);
        snprintf(fin,sizeof fin,"%s/%.*s",DIAG_SAVE_SCRATCH,(int)(n-4),e->d_name);
        struct stat st; if (stat(fin,&st)) { if (!rename(bak,fin)) fprintf(stderr,"[diag] recovered %s from .bak\n",fin); }
        else remove(bak);
    }
    closedir(d);
}
static int diag_open_tmp(const char *final) {
    char tmp[272]; snprintf(tmp,sizeof tmp,"%s.tmp",final);
    return open(tmp,O_WRONLY|O_CREAT|O_TRUNC|O_NOFOLLOW,0600);
}
void imp_KERNEL32_CreateFileA(CPU *c) {
    uint32_t a[7];
    if (c->esp>DIAG_MEM_SIZE-32) { fprintf(stderr,"[diag] invalid CreateFileA stack\n"); exit(36); }
    memcpy(a,g_mem+c->esp+4,sizeof a);
    if (!a[0] || a[0]>=DIAG_MEM_SIZE || !memchr(g_mem+a[0],0,DIAG_MEM_SIZE-a[0])) { fprintf(stderr,"[diag] invalid CreateFileA path\n"); exit(36); }
    fprintf(stderr,"[diag] CreateFileA path=%.200s access=%08x share=%x security=%x disposition=%u flags=%08x template=%08x\n",g_mem+a[0],a[1],a[2],a[3],a[4],a[5],a[6]);
    const char *guest_path=(char *)g_mem+a[0];
#ifdef SH3_MOVIE_LAB
    if (getenv("SH3_DIAG_MOVIE_PREP_SOURCE") && a[1]==0x80000000u && a[4]==3) {
        extern void movie_lab_note_path(const char *);
        movie_lab_note_path(guest_path);
    }
#endif
    const char *slot_prefix="C:\\SILENT HILL 3\\savedata\\data.";
    size_t prefix_len=strlen(slot_prefix);
    const char *slot=NULL;
    if (!strncasecmp(guest_path,slot_prefix,prefix_len) && strlen(guest_path+prefix_len)==3 &&
        guest_path[prefix_len]=='0' && guest_path[prefix_len+1]>='0' && guest_path[prefix_len+1]<='9' &&
        guest_path[prefix_len+2]>='0' && guest_path[prefix_len+2]<='9') slot=guest_path+prefix_len;
    if (slot && !a[6] && ((a[1]==0x80000000u && a[4]==3) || (a[1]==0x40000000u && a[4]==2))) {
        if (c->fs_base>DIAG_MEM_SIZE-0x38) { fprintf(stderr,"[diag] invalid save TIB\n"); exit(36); }
        if (SH3_MKDIR(DIAG_SAVE_SCRATCH,0700) && errno!=EEXIST) { fprintf(stderr,"[diag] cannot create diagnostic save directory\n"); exit(36); }
        struct stat ds;
        if (lstat(DIAG_SAVE_SCRATCH,&ds) || !S_ISDIR(ds.st_mode)) { fprintf(stderr,"[diag] invalid diagnostic save directory\n"); exit(36); }
        char name[4096];
        if (snprintf(name,sizeof name,"%s/data.%c%c%c",DIAG_SAVE_SCRATCH,slot[0],slot[1],slot[2])>=(int)sizeof name) exit(36);
        int writing=a[1]==0x40000000u;
        int fd=writing?diag_open_tmp(name):open(name,O_RDONLY|O_NOFOLLOW);
        if (fd<0 && !writing && errno==ENOENT) {
            uint32_t error=2; memcpy(g_mem+c->fs_base+0x34,&error,4); c->eax=0xffffffffu; c->esp+=32; return;
        }
        struct stat st;
        if (fd<0 || fstat(fd,&st) || !S_ISREG(st.st_mode)) { if(fd>=0)close(fd); fprintf(stderr,"[diag] cannot safely open diagnostic save slot\n"); exit(36); }
        FILE *f=fdopen(fd,writing?"wb":"rb"); if (!f) { close(fd); exit(36); }
        pthread_mutex_lock(&diag_object_lock); unsigned i=0; while(i<64 && diag_files[i]) i++;
        if (i==64) { pthread_mutex_unlock(&diag_object_lock); fclose(f); exit(36); }
        diag_files[i]=f; if (writing) snprintf(diag_commit_path[i],sizeof diag_commit_path[i],"%s",name);
        pthread_mutex_unlock(&diag_object_lock);
        fprintf(stderr,"[diag] diagnostic save slot %s -> %s\n",writing?"write":"read",name);
        c->eax=0xf5000000u+i; c->esp+=32; return;
    }
    if (a[1]==0x80000000u && a[4]==3 && !a[6]) {
        char path[4096]; int r=diag_data_path((char *)g_mem+a[0],path,sizeof path);
        if (r==0 && c->fs_base<=DIAG_MEM_SIZE-0x38) {
            uint32_t error=2; memcpy(g_mem+c->fs_base+0x34,&error,4); c->eax=0xffffffffu; c->esp+=32; return;
        }
        if (r==1) {
            uint64_t open_t0=c==diag_main_cpu?SDL_GetPerformanceCounter():0;
            FILE *f=NULL; size_t path_len=strlen(path);
            if ((g_diag_perf&PERF_FILECACHE) && path_len<sizeof diag_file_cache[0].path) {
                pthread_mutex_lock(&diag_object_lock);
                for (unsigned k=0;k<8 && !f;k++) if (diag_file_cache[k].f && !strcmp(diag_file_cache[k].path,path)) {
                    f=diag_file_cache[k].f; diag_file_cache[k].f=NULL;
                }
                pthread_mutex_unlock(&diag_object_lock);
                if (f) {
                    if (fseeko(f,0,SEEK_SET)) { fprintf(stderr,"[diag] cached game data rewind failed\n"); exit(36); }
                    clearerr(f); if (c==diag_main_cpu) ct_open_hit++;
                }
            }
            if (!f) {
                int fd=open(path,O_RDONLY|O_NOFOLLOW); struct stat st;
                if (fd<0 || fstat(fd,&st) || !S_ISREG(st.st_mode)) { if(fd>=0)close(fd); fprintf(stderr,"[diag] cannot safely open game data\n"); exit(36); }
                f=fdopen(fd,"rb"); if (!f) { close(fd); fprintf(stderr,"[diag] fdopen failed\n"); exit(36); }
            }
            pthread_mutex_lock(&diag_object_lock); unsigned i=0; while(i<64 && diag_files[i]) i++;
            if (i==64) { pthread_mutex_unlock(&diag_object_lock); fclose(f); fprintf(stderr,"[diag] file table full\n"); exit(36); }
            diag_files[i]=f; diag_file_game[i]=1;
            if (path_len<sizeof diag_file_path[0]) memcpy(diag_file_path[i],path,path_len+1); else diag_file_path[i][0]=0;
            pthread_mutex_unlock(&diag_object_lock);
            if (c==diag_main_cpu) { ct_open++; ct_open_ticks+=SDL_GetPerformanceCounter()-open_t0; }
            fprintf(stderr,"[diag] opened game data read-only -> %08x\n",0xf5000000u+i);
            c->eax=0xf5000000u+i; c->esp+=32; return;
        }
    }
    if ((!strcasecmp((char *)g_mem+a[0],"C:\\SILENT HILL 3\\savedata\\disp.ini") ||
         !strcasecmp((char *)g_mem+a[0],"C:\\SILENT HILL 3\\savedata\\key.ini") ||
         !strcasecmp((char *)g_mem+a[0],"C:\\SILENT HILL 3\\savedata\\data.sys")) && a[1]==0x80000000u && a[4]==3 && c->fs_base<=DIAG_MEM_SIZE-0x38) {
        const char *guest=(char *)g_mem+a[0];
        const char *file=!strcasecmp(guest,"C:\\SILENT HILL 3\\savedata\\key.ini") ? DIAG_SAVE_READ_FILE("key.ini") :
            !strcasecmp(guest,"C:\\SILENT HILL 3\\savedata\\data.sys") ? DIAG_SAVE_WRITE_FILE("data.sys") : DIAG_SAVE_READ_FILE("disp.ini");
        struct stat st;
        if (!stat(file,&st)) {
#ifndef __vita__
            if (strcasecmp(guest,"C:\\SILENT HILL 3\\savedata\\data.sys")) {
                fprintf(stderr,"[diag] existing settings access not yet implemented\n"); exit(30);
            }
#endif
            if (!S_ISREG(st.st_mode)) { fprintf(stderr,"[diag] invalid settings file\n"); exit(36); }
            FILE *f=fopen(file,"rb"); if (!f) { fprintf(stderr,"[diag] settings open failed\n"); exit(36); }
            pthread_mutex_lock(&diag_object_lock); unsigned i=0; while (i<64 && diag_files[i]) i++;
            if (i==64) { pthread_mutex_unlock(&diag_object_lock); fclose(f); exit(36); }
            diag_files[i]=f; pthread_mutex_unlock(&diag_object_lock);
            c->eax=0xf5000000u+i; c->esp+=32; return;
        }
        if (errno!=ENOENT) { fprintf(stderr,"[diag] cannot inspect settings\n"); exit(36); }
        uint32_t error=2; memcpy(g_mem+c->fs_base+0x34,&error,4); c->eax=0xffffffffu; c->esp+=32; return;
    }
    if ((!strcasecmp((char *)g_mem+a[0],"C:\\SILENT HILL 3\\savedata\\disp.ini") ||
         !strcasecmp((char *)g_mem+a[0],"C:\\SILENT HILL 3\\savedata\\data.sys")) && a[1]==0x40000000u && a[4]==2 && !a[6]) {
        if (SH3_MKDIR(DIAG_SAVE_SCRATCH,0700) && errno!=EEXIST) { fprintf(stderr,"[diag] cannot create scratch savedata\n"); exit(36); }
        struct stat ds;
        if (lstat(DIAG_SAVE_SCRATCH,&ds) || !S_ISDIR(ds.st_mode)) { fprintf(stderr,"[diag] invalid scratch savedata\n"); exit(36); }
        pthread_mutex_lock(&diag_object_lock);
        unsigned i=0; while (i<64 && diag_files[i]) i++;
        if (i==64) { fprintf(stderr,"[diag] file table full\n"); exit(36); }
        const char *scratch=!strcasecmp((char *)g_mem+a[0],"C:\\SILENT HILL 3\\savedata\\data.sys") ? DIAG_SAVE_WRITE_FILE("data.sys") : DIAG_SAVE_WRITE_FILE("disp.ini");
        /* disp.ini (Brightness gamma_r/g/b) goes through a .tmp renamed on close only if something was written: the
           game truncates it at every boot ("wt" open/close in F_005f09f0) and only writes it on a clean exit or an
           Brightness screen commit (console_reference_unit.py, F_005ed0f0); a killed app must keep the last written one. */
        int fd=diag_open_tmp(scratch);
        diag_files[i]=fd<0?NULL:fdopen(fd,"wb");
        if (diag_files[i]) snprintf(diag_commit_path[i],sizeof diag_commit_path[i],"%s",scratch);
        pthread_mutex_unlock(&diag_object_lock);
        if (!diag_files[i]) { fprintf(stderr,"[diag] cannot open scratch disp.ini\n"); exit(36); }
        fprintf(stderr,"[diag] write redirected to %s\n",scratch);
        c->eax=0xf5000000u+i; c->esp+=32; return;
    }
    fprintf(stderr,"[diag] unsupported CreateFileA request\n"); exit(30);
}

void imp_KERNEL32_CloseHandle(CPU *c) {
    uint32_t h;
    if (c->esp>DIAG_MEM_SIZE-8) { fprintf(stderr,"[diag] invalid CloseHandle stack\n"); exit(36); }
    memcpy(&h,g_mem+c->esp+4,4);
    if (h==0xffffffffu && c->fs_base<=DIAG_MEM_SIZE-0x38) {
        uint32_t error=6; memcpy(g_mem+c->fs_base+0x34,&error,4);
        { uint32_t ret=0; memcpy(&ret,g_mem+c->esp,4); fprintf(stderr,"[diag] CloseHandle invalid handle -> ERROR_INVALID_HANDLE caller=%08x\n",ret); } c->eax=0; c->esp+=8; return;
    }
    pthread_mutex_lock(&diag_object_lock);
    if (h>=0xfc000000u && h-0xfc000000u<diag_mapping_count && diag_mappings[h-0xfc000000u].active) {
        DiagMapping *m=&diag_mappings[h-0xfc000000u]; close(m->fd); m->active=0;
        pthread_mutex_unlock(&diag_object_lock); c->eax=1; c->esp+=8; return;
    }
    if ((h&0xff000000u)==0xf9000000u) {
        pthread_mutex_unlock(&diag_object_lock);
        if(diag_event_handle_close(h)){fprintf(stderr,"[diag] invalid CloseHandle event %08x\n",h);exit(30);}
        c->eax=1;c->esp+=8;return;
    }
    if (h<0xf5000000u || h-0xf5000000u>=64 || !diag_files[h-0xf5000000u]) {
        pthread_mutex_unlock(&diag_object_lock);
        fprintf(stderr,"[diag] unsupported CloseHandle %08x\n",h); exit(30);
    }
    unsigned i=h-0xf5000000u;
    if ((g_diag_perf&PERF_FILECACHE) && diag_file_game[i] && diag_file_path[i][0]) {
        /* Keep the read-only stream for the next open of the same file; evict round-robin when full. */
        unsigned k=0; while (k<8 && diag_file_cache[k].f) k++;
        if (k==8) { static unsigned victim; k=victim++&7u; fclose(diag_file_cache[k].f); }
        strcpy(diag_file_cache[k].path,diag_file_path[i]); diag_file_cache[k].f=diag_files[i];
    } else if (fclose(diag_files[i])) { pthread_mutex_unlock(&diag_object_lock); fprintf(stderr,"[diag] file close failed\n"); exit(36); }
    if (diag_commit_path[i][0]) {
        char tmp[272]; struct stat ts; snprintf(tmp,sizeof tmp,"%s.tmp",diag_commit_path[i]);
        if (!stat(tmp,&ts) && ts.st_size>0) {
            /* Vita rename does not replace: the old file is kept as NAME.bak until the new one is in place (a crash in
               between is repaired at the next boot by diag_recover_commits), never deleted first. */
            char bak[272]; snprintf(bak,sizeof bak,"%s.bak",diag_commit_path[i]);
            remove(bak); int had=!rename(diag_commit_path[i],bak);
            if (rename(tmp,diag_commit_path[i])) { fprintf(stderr,"[diag] %s rename failed\n",diag_commit_path[i]); if (had) rename(bak,diag_commit_path[i]); }
            else { if (had) remove(bak); fprintf(stderr,"[diag] %s saved (%ld bytes)\n",diag_commit_path[i],(long)ts.st_size); }
        } else remove(tmp);
        diag_commit_path[i][0]=0;
    }
    diag_files[i]=NULL; diag_file_game[i]=0; diag_file_path[i][0]=0; pthread_mutex_unlock(&diag_object_lock); c->eax=1; c->esp+=8;
}

void imp_WINMM_mmioOpenA(CPU *c) {
    uint32_t a[3], info[19];
    if (c->esp>DIAG_MEM_SIZE-16) { fprintf(stderr,"[diag] invalid mmioOpenA stack\n"); exit(36); }
    memcpy(a,g_mem+c->esp+4,sizeof a);
    if (a[0] || !a[1] || a[1]>DIAG_MEM_SIZE-sizeof info || a[2]!=0x10000) goto unsupported;
    memcpy(info,g_mem+a[1],sizeof info);
    if (info[1]!=0x204d454d || info[2] || !info[5] || !info[6] ||
        info[6]>=DIAG_MEM_SIZE || info[5]>DIAG_MEM_SIZE-info[6]) goto unsupported;
    for (unsigned i=0;i<16;i++) if (i!=1 && i!=5 && i!=6 && info[i]) goto unsupported;
    pthread_mutex_lock(&diag_object_lock);
    if (diag_mmio_count>=DIAG_MMIO_LIMIT) { pthread_mutex_unlock(&diag_object_lock); goto unsupported; }
    unsigned i=diag_mmio_count++;
    diag_mmio[i]=(DiagMmio){info[6],info[5],0};
    pthread_mutex_unlock(&diag_object_lock);
    c->eax=0xfa000000u+i; c->esp+=16;
    if (DIAG_FLAG("SH3_DIAG_VERBOSE")) fprintf(stderr,"[diag] mmioOpenA memory=%08x size=%u -> %08x (experimental)\n",info[6],info[5],c->eax);
    return;
unsupported:
    fprintf(stderr,"[diag] unsupported mmioOpenA filename=%08x info=%08x flags=%08x\n",a[0],a[1],a[2]); exit(30);
}

void imp_WINMM_mmioDescend(CPU *c) {
    uint32_t a[4], chunk[5]={0};
    if (c->esp>DIAG_MEM_SIZE-20) { fprintf(stderr,"[diag] invalid mmioDescend stack\n"); exit(36); }
    memcpy(a,g_mem+c->esp+4,sizeof a);
    if (a[0]<0xfa000000u || a[0]-0xfa000000u>=diag_mmio_count || !a[1] ||
        a[1]>DIAG_MEM_SIZE-20 || (a[3]!=0 && a[3]!=0x10) ||
        (a[2] && a[2]>DIAG_MEM_SIZE-20)) goto unsupported;
    pthread_mutex_lock(&diag_object_lock);
    DiagMmio *m=&diag_mmio[a[0]-0xfa000000u];
    uint32_t wanted=0, limit=m->size;
    if (a[3]==0x10) memcpy(&wanted,g_mem+a[1],4);
    if (a[2]) {
        uint32_t parent[5]; memcpy(parent,g_mem+a[2],sizeof parent);
        if (parent[3]>limit || parent[1]>limit-parent[3]) { pthread_mutex_unlock(&diag_object_lock); goto unsupported; }
        limit=parent[3]+parent[1];
    }
again:
    if (m->pos>limit || limit-m->pos<8) { pthread_mutex_unlock(&diag_object_lock); goto unsupported; }
    if (m->pos>m->size || m->size-m->pos<8) { pthread_mutex_unlock(&diag_object_lock); goto unsupported; }
    memcpy(chunk,g_mem+m->base+m->pos,8); m->pos+=8; chunk[3]=m->pos;
    if (a[3]==0x10 && chunk[0]!=wanted) {
        uint64_t end=(uint64_t)m->pos+chunk[1]+(chunk[1]&1u);
        if (end>limit) { pthread_mutex_unlock(&diag_object_lock); goto unsupported; }
        m->pos=(uint32_t)end; goto again;
    }
    if (chunk[0]==0x46464952u || chunk[0]==0x5453494cu) {
        if (m->size-m->pos<4 || chunk[1]<4) { pthread_mutex_unlock(&diag_object_lock); goto unsupported; }
        memcpy(&chunk[2],g_mem+m->base+m->pos,4); m->pos+=4;
    }
    memcpy(g_mem+a[1],chunk,sizeof chunk);
    pthread_mutex_unlock(&diag_object_lock);
    if (DIAG_FLAG("SH3_DIAG_VERBOSE")) fprintf(stderr,"[diag] mmioDescend chunk=%08x size=%u type=%08x offset=%u\n",chunk[0],chunk[1],chunk[2],chunk[3]);
    c->eax=0; c->esp+=20; return;
unsupported:
    fprintf(stderr,"[diag] unsupported mmioDescend handle=%08x chunk=%08x parent=%08x flags=%08x\n",a[0],a[1],a[2],a[3]); exit(30);
}

void imp_WINMM_mmioRead(CPU *c) {
    uint32_t a[3];
    if (c->esp>DIAG_MEM_SIZE-16) { fprintf(stderr,"[diag] invalid mmioRead stack\n"); exit(36); }
    memcpy(a,g_mem+c->esp+4,sizeof a);
    if (a[0]<0xfa000000u || a[0]-0xfa000000u>=diag_mmio_count || (int32_t)a[2]<0 ||
        a[1]>DIAG_MEM_SIZE || a[2]>DIAG_MEM_SIZE-a[1]) {
        fprintf(stderr,"[diag] unsupported mmioRead\n"); exit(30);
    }
    pthread_mutex_lock(&diag_object_lock);
    DiagMmio *m=&diag_mmio[a[0]-0xfa000000u];
    uint32_t n=a[2]; if (n>m->size-m->pos) n=m->size-m->pos;
    memmove(g_mem+a[1],g_mem+m->base+m->pos,n); m->pos+=n;
    pthread_mutex_unlock(&diag_object_lock);
    if (DIAG_FLAG("SH3_DIAG_VERBOSE")) fprintf(stderr,"[diag] mmioRead requested=%u read=%u\n",a[2],n);
    c->eax=n; c->esp+=16;
}

void imp_WINMM_mmioAscend(CPU *c) {
    uint32_t a[3], chunk[5];
    if (c->esp>DIAG_MEM_SIZE-16) { fprintf(stderr,"[diag] invalid mmioAscend stack\n"); exit(36); }
    memcpy(a,g_mem+c->esp+4,sizeof a);
    if (a[0]<0xfa000000u || a[0]-0xfa000000u>=diag_mmio_count || !a[1] ||
        a[1]>DIAG_MEM_SIZE-20 || a[2]) goto unsupported;
    memcpy(chunk,g_mem+a[1],sizeof chunk);
    if (chunk[4]) goto unsupported;
    pthread_mutex_lock(&diag_object_lock);
    DiagMmio *m=&diag_mmio[a[0]-0xfa000000u];
    uint64_t end=(uint64_t)chunk[3]+chunk[1]+(chunk[1]&1u);
    if (end>m->size) { pthread_mutex_unlock(&diag_object_lock); goto unsupported; }
    m->pos=(uint32_t)end; pthread_mutex_unlock(&diag_object_lock);
    if (DIAG_FLAG("SH3_DIAG_VERBOSE")) fprintf(stderr,"[diag] mmioAscend next=%u\n",(unsigned)end);
    c->eax=0; c->esp+=16; return;
unsupported:
    fprintf(stderr,"[diag] unsupported mmioAscend handle=%08x chunk=%08x flags=%08x\n",a[0],a[1],a[2]); exit(30);
}

void imp_WINMM_mmioSeek(CPU *c) {
    uint32_t a[3];
    if (c->esp>DIAG_MEM_SIZE-16) { fprintf(stderr,"[diag] invalid mmioSeek stack\n"); exit(36); }
    memcpy(a,g_mem+c->esp+4,sizeof a);
    if (a[0]<0xfa000000u || a[0]-0xfa000000u>=diag_mmio_count || a[2]>2) goto unsupported;
    pthread_mutex_lock(&diag_object_lock);
    DiagMmio *m=&diag_mmio[a[0]-0xfa000000u];
    int64_t origin=a[2]==0?0:a[2]==1?m->pos:m->size;
    int64_t pos=origin+(int32_t)a[1];
    if (pos<0 || pos>m->size) { pthread_mutex_unlock(&diag_object_lock); goto unsupported; }
    m->pos=(uint32_t)pos; pthread_mutex_unlock(&diag_object_lock);
    if (DIAG_FLAG("SH3_DIAG_VERBOSE")) fprintf(stderr,"[diag] mmioSeek offset=%d origin=%u -> %u\n",(int32_t)a[1],a[2],(unsigned)pos);
    c->eax=(uint32_t)pos; c->esp+=16; return;
unsupported:
    fprintf(stderr,"[diag] unsupported mmioSeek handle=%08x offset=%d origin=%u\n",a[0],(int32_t)a[1],a[2]); exit(30);
}

static void diag_mmio_info(CPU *c,int set) {
    uint32_t a[3], info[19]={0};
    if (c->esp>DIAG_MEM_SIZE-16) { fprintf(stderr,"[diag] invalid mmio info stack\n"); exit(36); }
    memcpy(a,g_mem+c->esp+4,sizeof a);
    if (a[0]<0xfa000000u || a[0]-0xfa000000u>=diag_mmio_count || !a[1] ||
        a[1]>DIAG_MEM_SIZE-sizeof info || a[2]) goto unsupported;
    pthread_mutex_lock(&diag_object_lock);
    DiagMmio *m=&diag_mmio[a[0]-0xfa000000u];
    if (set) {
        memcpy(info,g_mem+a[1],sizeof info);
        if (info[6]!=m->base || info[7]<m->base+m->pos || info[7]>m->base+m->size || info[0]) {
            pthread_mutex_unlock(&diag_object_lock); goto unsupported;
        }
        m->pos=info[7]-m->base;
    } else {
        info[1]=0x204d454d; info[5]=m->size; info[6]=m->base;
        info[7]=m->base+m->pos; info[8]=m->base+m->size; info[9]=m->base+m->size;
        info[11]=m->pos; info[18]=a[0]; memcpy(g_mem+a[1],info,sizeof info);
    }
    if (DIAG_FLAG("SH3_DIAG_VERBOSE")) fprintf(stderr,"[diag] mmio%sInfo position=%u\n",set?"Set":"Get",m->pos);
    pthread_mutex_unlock(&diag_object_lock);
    c->eax=0; c->esp+=16; return;
unsupported:
    fprintf(stderr,"[diag] unsupported mmio%sInfo handle=%08x info=%08x flags=%08x\n",set?"Set":"Get",a[0],a[1],a[2]); exit(30);
}
void imp_WINMM_mmioGetInfo(CPU *c) { diag_mmio_info(c,0); }
void imp_WINMM_mmioSetInfo(CPU *c) { diag_mmio_info(c,1); }
void imp_WINMM_mmioClose(CPU *c) {
    uint32_t a[2];
    if (c->esp>DIAG_MEM_SIZE-12) { fprintf(stderr,"[diag] invalid mmioClose stack\n"); exit(36); }
    memcpy(a,g_mem+c->esp+4,sizeof a);
    pthread_mutex_lock(&diag_object_lock);
    if (a[0]<0xfa000000u || a[0]-0xfa000000u>=diag_mmio_count || a[1] ||
        !diag_mmio[a[0]-0xfa000000u].size) {
        pthread_mutex_unlock(&diag_object_lock); fprintf(stderr,"[diag] unsupported mmioClose\n"); exit(30);
    }
    diag_mmio[a[0]-0xfa000000u].size=0; pthread_mutex_unlock(&diag_object_lock);
    if (DIAG_FLAG("SH3_DIAG_VERBOSE")) fprintf(stderr,"[diag] mmioClose %08x\n",a[0]); c->eax=0; c->esp+=12;
}

void imp_KERNEL32_GetFileSize(CPU *c) {
    uint32_t a[2]; struct stat st;
    if (c->esp>DIAG_MEM_SIZE-12) { fprintf(stderr,"[diag] invalid GetFileSize stack\n"); exit(36); }
    memcpy(a,g_mem+c->esp+4,sizeof a);
    if (a[0]<0xf5000000u || a[0]-0xf5000000u>=64 || (a[1] && a[1]>DIAG_MEM_SIZE-4)) {
        fprintf(stderr,"[diag] unsupported GetFileSize arguments\n"); exit(30);
    }
    pthread_mutex_lock(&diag_object_lock); FILE *f=diag_files[a[0]-0xf5000000u];
    int r=f?fstat(fileno(f),&st):-1; pthread_mutex_unlock(&diag_object_lock);
    if (r || st.st_size<0) { fprintf(stderr,"[diag] GetFileSize failed\n"); exit(36); }
    uint64_t size=(uint64_t)st.st_size; uint32_t high=(uint32_t)(size>>32);
    if (a[1]) memcpy(g_mem+a[1],&high,4);
    fprintf(stderr,"[diag] GetFileSize handle=%08x -> %llu\n",a[0],(unsigned long long)size);
    c->eax=(uint32_t)size; c->esp+=12;
}

void imp_KERNEL32_SetFilePointer(CPU *c) {
    uint32_t a[4];
    if (c->esp>DIAG_MEM_SIZE-20 || c->fs_base>DIAG_MEM_SIZE-0x38) { fprintf(stderr,"[diag] invalid SetFilePointer stack\n"); exit(36); }
    memcpy(a,g_mem+c->esp+4,sizeof a);
    if (a[0]<0xf5000000u || a[0]-0xf5000000u>=64 || a[3]>2 || (a[2] && a[2]>DIAG_MEM_SIZE-4)) {
        fprintf(stderr,"[diag] unsupported SetFilePointer\n"); exit(30);
    }
    int64_t distance=(int32_t)a[1];
    if (a[2]) { uint32_t high; uint64_t bits; memcpy(&high,g_mem+a[2],4); bits=((uint64_t)high<<32)|a[1]; memcpy(&distance,&bits,8); }
    static const int origins[3]={SEEK_SET,SEEK_CUR,SEEK_END};
    pthread_mutex_lock(&diag_object_lock);
    FILE *f=diag_files[a[0]-0xf5000000u]; int r=f?fseeko(f,(off_t)distance,origins[a[3]]):-1;
    off_t position=r?-1:ftello(f); pthread_mutex_unlock(&diag_object_lock);
    if (position<0) { uint32_t error=87; memcpy(g_mem+c->fs_base+0x34,&error,4); c->eax=0xffffffffu; }
    else {
        uint64_t pos=(uint64_t)position; uint32_t high=(uint32_t)(pos>>32),zero=0;
        if (a[2]) memcpy(g_mem+a[2],&high,4);
        c->eax=(uint32_t)pos; if(c->eax==0xffffffffu)memcpy(g_mem+c->fs_base+0x34,&zero,4);
    }
    c->esp+=20;
}

void imp_KERNEL32_ReadFile(CPU *c) {
    uint32_t a[5];
    if (c->esp>DIAG_MEM_SIZE-24) { fprintf(stderr,"[diag] invalid ReadFile stack\n"); exit(36); }
    memcpy(a,g_mem+c->esp+4,sizeof a);
    if (a[0]<0xf5000000u || a[0]-0xf5000000u>=64 || a[4] || (a[2] && (!a[1] || a[1]>=DIAG_MEM_SIZE || a[2]>DIAG_MEM_SIZE-a[1])) ||
        (a[3] && a[3]>DIAG_MEM_SIZE-4)) { fprintf(stderr,"[diag] unsupported ReadFile arguments\n"); exit(30); }
    pthread_mutex_lock(&diag_object_lock); FILE *f=diag_files[a[0]-0xf5000000u];
    if (!f) { pthread_mutex_unlock(&diag_object_lock); fprintf(stderr,"[diag] invalid ReadFile handle\n"); exit(36); }
    unsigned index=a[0]-0xf5000000u;
    int measured=diag_io_profile && diag_file_game[index];
    uint64_t t0=SDL_GetPerformanceCounter();   /* always: slow reads are reported below (loading-freeze diagnostic) */
    size_t n=0; int err=0; static int readlog=-1; if (readlog<0) readlog=getenv("SH3_DIAG_READLOG")!=NULL; off_t pos0=readlog?ftello(f):0;
    if (a[2] && diag_direct_io && diag_file_game[index]) err=diag_direct_read(f,g_mem+a[1],a[2],&n);
    else { n=a[2]?fread(g_mem+a[1],1,a[2],f):0; err=ferror(f); }
    if (measured) {
        diag_io_ticks+=SDL_GetPerformanceCounter()-t0; diag_io_bytes+=n;
        if (++diag_io_reads==128) {
            fprintf(stderr,"[diag-io] mode=%s reads=%u bytes=%llu elapsed_ms=%.3f\n",
                    diag_direct_io?"direct":"stdio",diag_io_reads,(unsigned long long)diag_io_bytes,
                    1000.0*(double)diag_io_ticks/(double)SDL_GetPerformanceFrequency());
            diag_io_reads=0; diag_io_ticks=diag_io_bytes=0;
        }
    }
    if (readlog) { uint32_t ret=0; memcpy(&ret,g_mem+c->esp,4); fprintf(stderr,"[read] h=%08x pos=%llx req=%x got=%zx dst=%08x caller=%08x",a[0],(unsigned long long)pos0,a[2],n,a[1],ret); putc(10,stderr); }
    {   /* bounded: reads longer than 30 ms, done while holding diag_object_lock */
        static unsigned slow_reads; uint64_t dt=(SDL_GetPerformanceCounter()-t0)*1000/SDL_GetPerformanceFrequency();
        if (dt>30 && slow_reads<400) { uint32_t ret=0; memcpy(&ret,g_mem+c->esp,4); slow_reads++;
            fprintf(stderr,"[slow-read] %llu ms req=%u got=%u main=%d caller=%08x at=%llu ms\n",(unsigned long long)dt,a[2],(unsigned)n,c==diag_main_cpu,ret,(unsigned long long)SDL_GetTicks64()); }
    }
    pthread_mutex_unlock(&diag_object_lock);
    if (err) { fprintf(stderr,"[diag] host ReadFile failed\n"); exit(36); }
    uint32_t got=(uint32_t)n; if (a[3]) memcpy(g_mem+a[3],&got,4);
    if (got!=a[2] || DIAG_ENV(env_verbose,"SH3_DIAG_VERBOSE"))
        fprintf(stderr,"[diag] ReadFile %08x requested=%u read=%u\n",a[0],a[2],got);
    c->eax=1; c->esp+=24;
}
void imp_KERNEL32_WriteFile(CPU *c) {
    uint32_t a[5];
    if (c->esp>DIAG_MEM_SIZE-24) { fprintf(stderr,"[diag] invalid WriteFile stack\n"); exit(36); }
    memcpy(a,g_mem+c->esp+4,sizeof a);
    if (a[0]<0xf5000000u || a[0]-0xf5000000u>=64 || a[4] || (a[2] && (!a[1] || a[1]>=DIAG_MEM_SIZE || a[2]>DIAG_MEM_SIZE-a[1])) ||
        (a[3] && a[3]>DIAG_MEM_SIZE-4)) { fprintf(stderr,"[diag] unsupported WriteFile arguments\n"); exit(30); }
    pthread_mutex_lock(&diag_object_lock); FILE *f=diag_files[a[0]-0xf5000000u];
    if (!f) { pthread_mutex_unlock(&diag_object_lock); fprintf(stderr,"[diag] invalid WriteFile handle\n"); exit(36); }
    size_t n=a[2]?fwrite(g_mem+a[1],1,a[2],f):0; int err=ferror(f);
    pthread_mutex_unlock(&diag_object_lock);
    if (err || n!=a[2]) { fprintf(stderr,"[diag] host WriteFile failed\n"); exit(36); }
    uint32_t written=(uint32_t)n; if (a[3]) memcpy(g_mem+a[3],&written,4);
    fprintf(stderr,"[diag] WriteFile %08x bytes=%u\n",a[0],written); c->eax=1; c->esp+=24;
}
void imp_KERNEL32_CreateFileMappingA(CPU *c) {
    uint32_t a[6];
    if (c->esp>DIAG_MEM_SIZE-28) { fprintf(stderr,"[diag] invalid CreateFileMappingA stack\n"); exit(36); }
    memcpy(a,g_mem+c->esp+4,sizeof a);
    if (a[0]<0xf5000000u || a[0]-0xf5000000u>=64 || a[1] || a[2]!=2 || a[3] || a[4] || a[5]) goto unsupported;
    pthread_mutex_lock(&diag_object_lock);
    FILE *f=diag_files[a[0]-0xf5000000u]; struct stat st;
    if (!f || diag_mapping_count>=128 || fstat(fileno(f),&st) || st.st_size<=0 || (uint64_t)st.st_size>DIAG_MEM_SIZE) {
        pthread_mutex_unlock(&diag_object_lock); goto unsupported;
    }
    int fd=dup(fileno(f)); if (fd<0) { pthread_mutex_unlock(&diag_object_lock); goto unsupported; }
    unsigned i=diag_mapping_count++; diag_mappings[i]=(DiagMapping){fd,(uint32_t)st.st_size,1};
    pthread_mutex_unlock(&diag_object_lock);
    fprintf(stderr,"[diag] read-only file mapping size=%u handle=%08x\n",diag_mappings[i].size,0xfc000000u+i);
    c->eax=0xfc000000u+i; c->esp+=28; return;
unsupported:
    fprintf(stderr,"[diag] CreateFileMappingA file=%08x security=%08x protect=%08x max=%08x:%08x name=%08x unsupported\n",a[0],a[1],a[2],a[3],a[4],a[5]); exit(30);
}
void imp_KERNEL32_MapViewOfFile(CPU *c) {
    uint32_t a[5];
    if (c->esp>DIAG_MEM_SIZE-24) { fprintf(stderr,"[diag] invalid MapViewOfFile stack\n"); exit(36); }
    memcpy(a,g_mem+c->esp+4,sizeof a);
    pthread_mutex_lock(&diag_object_lock);
    if (a[0]<0xfc000000u || a[0]-0xfc000000u>=diag_mapping_count || !diag_mappings[a[0]-0xfc000000u].active || a[1]!=4 || a[2] || a[3]) goto unsupported;
    DiagMapping *m=&diag_mappings[a[0]-0xfc000000u]; uint32_t n=a[4]?a[4]:m->size;
    if (!n || n>m->size) goto unsupported;
    unsigned slot=0; while (slot<128 && diag_mapping_views[slot]) slot++;
    if (slot==128) goto unsupported;
    uint32_t p=diag_heap_alloc(diag_guest_heap,n,0);
    if (!p) goto unsupported;
    size_t done=0;
    while (done<n) { ssize_t got=pread(m->fd,g_mem+p+done,n-done,(off_t)done); if (got<=0) { diag_heap_free(diag_guest_heap,p); goto unsupported; } done+=(size_t)got; }
    diag_mapping_views[slot]=p; pthread_mutex_unlock(&diag_object_lock);
    fprintf(stderr,"[diag] copied read-only mapped view size=%u -> %08x\n",n,p); c->eax=p; c->esp+=24; return;
unsupported:
    pthread_mutex_unlock(&diag_object_lock); fprintf(stderr,"[diag] unsupported MapViewOfFile %08x %08x %08x:%08x size=%u\n",a[0],a[1],a[2],a[3],a[4]); exit(30);
}
void imp_KERNEL32_UnmapViewOfFile(CPU *c) {
    uint32_t p;
    if (c->esp>DIAG_MEM_SIZE-8) { fprintf(stderr,"[diag] invalid UnmapViewOfFile stack\n"); exit(36); }
    memcpy(&p,g_mem+c->esp+4,4); pthread_mutex_lock(&diag_object_lock);
    unsigned i=0; while (i<128 && (!p || diag_mapping_views[i]!=p)) i++;
    if (i==128 || diag_heap_free(diag_guest_heap,p)) { pthread_mutex_unlock(&diag_object_lock); fprintf(stderr,"[diag] invalid mapped view\n"); exit(30); }
    diag_mapping_views[i]=0; pthread_mutex_unlock(&diag_object_lock); c->eax=1; c->esp+=8;
}

void imp_ADVAPI32_RegOpenKeyExA(CPU *c) {
    uint32_t a[5];
    if (c->esp>DIAG_MEM_SIZE-24) { fprintf(stderr,"[diag] invalid registry open stack\n"); exit(36); }
    memcpy(a,g_mem+c->esp+4,sizeof a);
    if (!a[1] || a[1]>=DIAG_MEM_SIZE || !memchr(g_mem+a[1],0,DIAG_MEM_SIZE-a[1])) { fprintf(stderr,"[diag] invalid registry key\n"); exit(36); }
    fprintf(stderr,"[diag] RegOpenKeyExA root=%08x key=%.200s options=%x access=%x result=%08x\n",a[0],g_mem+a[1],a[2],a[3],a[4]);
    if (a[0]==0x80000002u && !strcasecmp((char *)g_mem+a[1],"SOFTWARE\\Konami\\Silent Hill2") && !a[2] && a[4] && a[4]<=DIAG_MEM_SIZE-4) {
        fprintf(stderr,"[diag] optional SH2 registry key absent\n"); c->eax=2; c->esp+=24; return;
    }
    if (a[0]!=0x80000002u || strcasecmp((char *)g_mem+a[1],"SOFTWARE\\KONAMI\\SILENT HILL 3") || a[2] || !a[4] || a[4]>DIAG_MEM_SIZE-4) {
        fprintf(stderr,"[diag] unsupported registry key\n"); exit(30);
    }
    uint32_t handle=0xf4000001u;
    memcpy(g_mem+a[4],&handle,4); diag_registry_open++; c->eax=0; c->esp+=24;
}

void imp_ADVAPI32_RegOpenKeyA(CPU *c) {
    uint32_t a[3];
    if (c->esp>DIAG_MEM_SIZE-16) { fprintf(stderr,"[diag] invalid RegOpenKeyA stack\n"); exit(36); }
    memcpy(a,g_mem+c->esp+4,sizeof a);
    if (!a[1] || a[1]>=DIAG_MEM_SIZE || !memchr(g_mem+a[1],0,DIAG_MEM_SIZE-a[1])) { fprintf(stderr,"[diag] invalid RegOpenKeyA key\n"); exit(36); }
    if (DIAG_FLAG("SH3_DIAG_VERBOSE")) fprintf(stderr,"[diag] RegOpenKeyA root=%08x path=%.200s result=%08x\n",a[0],g_mem+a[1],a[2]);
    if (a[0]==0x80000002u && !strcasecmp((char *)g_mem+a[1],"SOFTWARE\\Microsoft\\Direct3D")) {
        c->eax=2; c->esp+=16; return; /* optional key absent in synthetic registry */
    }
    fprintf(stderr,"[diag] unsupported RegOpenKeyA\n"); exit(30);
}

void imp_ADVAPI32_RegQueryValueExA(CPU *c) {
    uint32_t a[6];
    if (c->esp>DIAG_MEM_SIZE-28) { fprintf(stderr,"[diag] invalid registry query stack\n"); exit(36); }
    memcpy(a,g_mem+c->esp+4,sizeof a);
    if (a[0]!=0xf4000001u || !diag_registry_open || !a[1] || a[1]>=DIAG_MEM_SIZE || !memchr(g_mem+a[1],0,DIAG_MEM_SIZE-a[1]) || a[2] ||
        (a[3] && a[3]>DIAG_MEM_SIZE-4) || (a[5] && a[5]>DIAG_MEM_SIZE-4)) {
        fprintf(stderr,"[diag] unsupported registry query\n"); exit(30);
    }
    const char *name=(char *)g_mem+a[1], *value=NULL;
    if (!strcasecmp(name,"data") || !strcasecmp(name,"installdir") || !strcasecmp(name,"movie") || !strcasecmp(name,"sound")) value="C:\\SILENT HILL 3\\";
    if (!strcasecmp(name,"save")) value="C:\\SILENT HILL 3\\savedata\\";
    fprintf(stderr,"[diag] RegQueryValueExA %s -> %s\n",name,value?value:"not found");
    if (!value) { c->eax=2; c->esp+=28; return; }
    uint32_t n=(uint32_t)strlen(value)+1, type=1, cap=0;
    if (a[5]) memcpy(&cap,g_mem+a[5],4);
    if (a[3]) memcpy(g_mem+a[3],&type,4);
    if (a[5]) memcpy(g_mem+a[5],&n,4);
    if (a[4] && (!a[5] || cap<n)) { c->eax=234; c->esp+=28; return; }
    if (a[4] && (a[4]>=DIAG_MEM_SIZE || n>DIAG_MEM_SIZE-a[4])) { fprintf(stderr,"[diag] invalid registry output\n"); exit(36); }
    if (a[4]) memcpy(g_mem+a[4],value,n);
    c->eax=0; c->esp+=28;
}

void imp_ADVAPI32_RegCloseKey(CPU *c) {
    uint32_t key;
    if (c->esp>DIAG_MEM_SIZE-8) { fprintf(stderr,"[diag] invalid registry close stack\n"); exit(36); }
    memcpy(&key,g_mem+c->esp+4,4);
    if (key!=0xf4000001u || !diag_registry_open) { fprintf(stderr,"[diag] invalid registry close handle\n"); exit(30); }
    diag_registry_open--; c->eax=0; c->esp+=8;
}

void imp_KERNEL32_GetTickCount(CPU *c) {
    struct timespec now;
    if (c->esp > DIAG_MEM_SIZE-4 || clock_gettime(CLOCK_MONOTONIC,&now)) { fprintf(stderr,"[diag] invalid tick count\n"); exit(36); }
    c->eax=(uint32_t)((uint64_t)now.tv_sec*1000u+(uint64_t)now.tv_nsec/1000000u); c->esp+=4;
}
void imp_WINMM_timeGetTime(CPU *c) { imp_KERNEL32_GetTickCount(c); }

static void diag_performance_value(CPU *c, int frequency) {
    uint32_t p; struct timespec now;
    if (c->esp>DIAG_MEM_SIZE-8) { fprintf(stderr,"[diag] invalid performance timer stack\n"); exit(36); }
    memcpy(&p,g_mem+c->esp+4,4);
    if (!p || p>DIAG_MEM_SIZE-8 || clock_gettime(CLOCK_MONOTONIC,&now)) { fprintf(stderr,"[diag] invalid performance timer destination\n"); exit(36); }
    uint64_t value=frequency?1000000000ull:(uint64_t)now.tv_sec*1000000000ull+(uint64_t)now.tv_nsec;
    memcpy(g_mem+p,&value,8); c->eax=1; c->esp+=8;
}
void imp_KERNEL32_QueryPerformanceCounter(CPU *c) { diag_performance_value(c,0); }
void imp_KERNEL32_QueryPerformanceFrequency(CPU *c) { diag_performance_value(c,1); }

void imp_KERNEL32_CreateMutexA(CPU *c) {
    uint32_t a[3];
    if (c->esp>DIAG_MEM_SIZE-16) { fprintf(stderr,"[diag] invalid CreateMutexA stack\n"); exit(36); }
    memcpy(a,g_mem+c->esp+4,sizeof a);
    const char *name=NULL;
    if (a[2]) {
        if (a[2]>=DIAG_MEM_SIZE || !memchr(g_mem+a[2],0,DIAG_MEM_SIZE-a[2])) { fprintf(stderr,"[diag] invalid mutex name\n"); exit(36); }
        name=(char *)g_mem+a[2];
        if (strlen(name)>=sizeof diag_mutex_names[0]) { fprintf(stderr,"[diag] mutex name too long\n"); exit(30); }
        for (unsigned i=0; i<diag_mutex_count; i++) if (!strcmp(name,diag_mutex_names[i])) {
            fprintf(stderr,"[diag] reopening named mutex not implemented\n"); exit(30);
        }
    }
    if (a[0] || a[1]>1 || diag_mutex_count>=128 || c->fs_base>DIAG_MEM_SIZE-0x38) {
        fprintf(stderr,"[diag] unsupported CreateMutexA security=%08x owner=%u name=%08x\n",a[0],a[1],a[2]); exit(30);
    }
    pthread_mutexattr_t attr;
    if (pthread_mutexattr_init(&attr) || pthread_mutexattr_settype(&attr,PTHREAD_MUTEX_RECURSIVE) ||
        pthread_mutex_init(&diag_mutexes[diag_mutex_count],&attr)) {
        fprintf(stderr,"[diag] CreateMutexA initialization failed\n"); exit(36);
    }
    pthread_mutexattr_destroy(&attr);
    if (a[1] && pthread_mutex_lock(&diag_mutexes[diag_mutex_count])) { fprintf(stderr,"[diag] CreateMutexA lock failed\n"); exit(36); }
    if (name) strcpy(diag_mutex_names[diag_mutex_count],name);
    memset(g_mem+c->fs_base+0x34,0,4);
    c->eax=0xf1000000u+diag_mutex_count++; c->esp+=16;
    fprintf(stderr,"[diag] CreateMutexA %s -> %08x\n",name?name:"(unnamed)",c->eax);
}

void imp_KERNEL32_GetFullPathNameA(CPU *c) {
    uint32_t a[4];
    if (c->esp>DIAG_MEM_SIZE-20) { fprintf(stderr,"[diag] invalid GetFullPathNameA stack\n"); exit(36); }
    memcpy(a,g_mem+c->esp+4,sizeof a);
    if (!a[0] || a[0]>=DIAG_MEM_SIZE || !memchr(g_mem+a[0],0,DIAG_MEM_SIZE-a[0])) {
        fprintf(stderr,"[diag] invalid GetFullPathNameA input\n"); exit(36);
    }
    fprintf(stderr,"[diag] GetFullPathNameA path=%.200s cap=%u buffer=%08x filepart=%08x\n",g_mem+a[0],a[1],a[2],a[3]);
    const char *s=(char *)g_mem+a[0]; char absolute[512];
    if (!strchr(s,'\\') && !strchr(s,'/') && !strchr(s,':') && strcmp(s,".") && strcmp(s,"..")) {
        int z=snprintf(absolute,sizeof absolute,"C:\\SILENT HILL 3\\%s",s);
        if (z<0 || (size_t)z>=sizeof absolute) { fprintf(stderr,"[diag] relative path too long\n"); exit(30); }
        s=absolute;
    }
    size_t n=strlen(s);
    if (n<3 || !((s[0]>='A' && s[0]<='Z') || (s[0]>='a' && s[0]<='z')) || s[1]!=':' || s[2]!='\\' ||
        strstr(s,"\\..") || strstr(s,"\\.") || strchr(s,'/') || !a[2] || a[1]<=n ||
        a[2]>=DIAG_MEM_SIZE || a[1]>DIAG_MEM_SIZE-a[2] || (a[3] && a[3]>DIAG_MEM_SIZE-4)) {
        fprintf(stderr,"[diag] unsupported GetFullPathNameA arguments\n"); exit(30);
    }
    const char *last=strrchr(s,'\\'); uint32_t part=a[2]+(uint32_t)(last-s)+1;
    memmove(g_mem+a[2],s,n+1);
    if (a[3]) memcpy(g_mem+a[3],&part,4);
    c->eax=(uint32_t)n; c->esp+=20;
}

void imp_KERNEL32_SetCurrentDirectoryA(CPU *c) {
    uint32_t p;
    if (c->esp>DIAG_MEM_SIZE-8) { fprintf(stderr,"[diag] invalid SetCurrentDirectoryA stack\n"); exit(36); }
    memcpy(&p,g_mem+c->esp+4,4);
    if (!p || p>=DIAG_MEM_SIZE || !memchr(g_mem+p,0,DIAG_MEM_SIZE-p)) { fprintf(stderr,"[diag] invalid directory\n"); exit(36); }
    fprintf(stderr,"[diag] SetCurrentDirectoryA %.200s\n",g_mem+p);
    if (strcasecmp((char *)g_mem+p,"C:\\SILENT HILL 3\\") && strcasecmp((char *)g_mem+p,"C:\\SILENT HILL 3")) {
        fprintf(stderr,"[diag] unsupported synthetic directory\n"); exit(30);
    }
    c->eax=1; c->esp+=8; /* fixed guest directory; host cwd is not changed */
}
void imp_KERNEL32_GetCurrentDirectoryA(CPU *c) {
    uint32_t a[2]; static const char path[]="C:\\SILENT HILL 3";
    if (c->esp>DIAG_MEM_SIZE-12) { fprintf(stderr,"[diag] invalid GetCurrentDirectoryA stack\n"); exit(36); }
    memcpy(a,g_mem+c->esp+4,sizeof a);
    if (a[0]>=sizeof path) {
        if (!a[1] || a[1]>DIAG_MEM_SIZE-sizeof path) { fprintf(stderr,"[diag] invalid directory buffer\n"); exit(36); }
        memcpy(g_mem+a[1],path,sizeof path); c->eax=sizeof path-1;
    } else c->eax=sizeof path;
    c->esp+=12;
}

void imp_KERNEL32_GetACP(CPU *c) {
    if (c->esp > DIAG_MEM_SIZE-4) { fprintf(stderr,"[diag] invalid GetACP stack\n"); exit(36); }
    c->eax=1252; c->esp+=4;
}

/* Initial game language (used only when no data.sys exists yet: first launch, F_0060d190 via F_005f12c0). Like sh4vita, the
   preparer writes "language": "French|English|German|Italian|Spanish" in sh3vita.json next to the game data; without it
   the game starts in English. PC bench: env SH3_LANGUAGE overrides (default French, the development configuration). */
static uint32_t diag_language_id(void) {
    static const struct { const char *name; uint32_t id; } map[]={{"French",0x040c},{"English",0x0409},{"German",0x0407},{"Italian",0x0410},{"Spanish",0x0c0a}};
    char value[32]={0};
#ifdef __vita__
    FILE *f=fopen("ux0:data/sh3/sh3vita.json","rb");
    if (f) {
        char buf[2048]; size_t n=fread(buf,1,sizeof buf-1,f); fclose(f); buf[n]=0;
        const char *k=strstr(buf,"\"language\""); const char *q=k?strchr(k+10,'"'):NULL;
        if (q) { q++; size_t i=0; while (q[i] && q[i]!='"' && i<sizeof value-1) { value[i]=q[i]; i++; } value[i]=0; }
    }
    if (!value[0]) strcpy(value,"English");
#else
    const char *e=getenv("SH3_LANGUAGE"); snprintf(value,sizeof value,"%s",e&&*e?e:"French");
#endif
    for (unsigned i=0;i<sizeof map/sizeof *map;i++) if (!strcmp(value,map[i].name)) { fprintf(stderr,"[diag] initial game language: %s\n",value); return map[i].id; }
    fprintf(stderr,"[diag] unknown initial language \"%s\", English used\n",value); return 0x0409;
}
void imp_KERNEL32_GetUserDefaultLangID(CPU *c) {
    if (c->esp>DIAG_MEM_SIZE-4) { fprintf(stderr,"[diag] invalid language stack\n"); exit(36); }
    c->eax=diag_language_id(); c->esp+=4;
}

void imp_KERNEL32_IsProcessorFeaturePresent(CPU *c) {
    uint32_t feature;
    if (c->esp>DIAG_MEM_SIZE-8) { fprintf(stderr,"[diag] invalid processor feature stack\n"); exit(36); }
    memcpy(&feature,g_mem+c->esp+4,4);
    if (feature!=3 && feature!=6 && feature!=7 && feature!=10) { fprintf(stderr,"[diag] unsupported processor feature %u\n",feature); exit(30); }
    fprintf(stderr,"[diag] IsProcessorFeaturePresent %u -> false (matches diagnostic CPUID)\n",feature);
    c->eax=0; c->esp+=8;
}

void imp_KERNEL32_GetCPInfo(CPU *c) {
    uint32_t a[2], max_char=1;
    if (c->esp > DIAG_MEM_SIZE-12) { fprintf(stderr,"[diag] invalid GetCPInfo stack\n"); exit(36); }
    memcpy(a,g_mem+c->esp+4,sizeof a);
    fprintf(stderr,"[diag] GetCPInfo cp=%u output=%08x\n",a[0],a[1]);
    if ((a[0]!=0 && a[0]!=1252 && a[0]!=437) || !a[1] || a[1]>DIAG_MEM_SIZE-20) {
        fprintf(stderr,"[diag] unsupported GetCPInfo arguments\n"); exit(30);
    }
    memset(g_mem+a[1],0,20); memcpy(g_mem+a[1],&max_char,4); g_mem[a[1]+4]='?';
    c->eax=1; c->esp+=12;
}

void imp_KERNEL32_SetUnhandledExceptionFilter(CPU *c) {
    uint32_t handler;
    if (c->esp > DIAG_MEM_SIZE-8) { fprintf(stderr,"[diag] invalid exception filter stack\n"); exit(36); }
    memcpy(&handler,g_mem+c->esp+4,4);
    c->eax=diag_exception_filter; diag_exception_filter=handler; c->esp+=8;
    fprintf(stderr,"[diag] SetUnhandledExceptionFilter=%08x (dispatch not implemented)\n",handler);
}

void imp_KERNEL32_GetSystemTimeAsFileTime(CPU *c) {
    uint32_t p; struct timeval now;
    if (c->esp > DIAG_MEM_SIZE-8) { fprintf(stderr,"[diag] invalid time stack\n"); exit(36); }
    memcpy(&p,g_mem+c->esp+4,4);
    if (!p || p>DIAG_MEM_SIZE-8 || gettimeofday(&now,NULL)) { fprintf(stderr,"[diag] invalid time destination\n"); exit(36); }
    uint64_t ticks=((uint64_t)now.tv_sec+11644473600ull)*10000000ull+(uint64_t)now.tv_usec*10ull;
    memcpy(g_mem+p,&ticks,8); c->esp+=8;
}

void imp_KERNEL32_GetStringTypeW(CPU *c) {
    uint32_t a[4];
    if (c->esp > DIAG_MEM_SIZE-20) { fprintf(stderr,"[diag] invalid GetStringTypeW stack\n"); exit(36); }
    memcpy(a,g_mem+c->esp+4,sizeof a);
    fprintf(stderr,"[diag] GetStringTypeW type=%u src=%08x len=%d dst=%08x\n",a[0],a[1],(int32_t)a[2],a[3]);
    if (a[0]!=1 || (int32_t)a[2]<=0 || a[1]>=DIAG_MEM_SIZE || a[3]>=DIAG_MEM_SIZE ||
        a[2]>(DIAG_MEM_SIZE-a[1])/2 || a[2]>(DIAG_MEM_SIZE-a[3])/2) {
        fprintf(stderr,"[diag] unsupported GetStringTypeW arguments\n"); exit(30);
    }
    unsigned approximated=0;
    for (uint32_t i=0; i<a[2]; i++) {
        uint16_t ch, f=0; memcpy(&ch,g_mem+a[1]+2*i,2);
        if (ch>=128) approximated++;
        if (ch>='A' && ch<='Z') f|=0x001|0x100;
        if (ch>='a' && ch<='z') f|=0x002|0x100;
        if (ch>='0' && ch<='9') f|=0x004;
        if (ch==' ' || (ch>=9 && ch<=13)) f|=0x008;
        if ((ch>=33 && ch<=47) || (ch>=58 && ch<=64) || (ch>=91 && ch<=96) || (ch>=123 && ch<=126)) f|=0x010;
        if (ch<32 || ch==127) f|=0x020;
        if (ch==' ' || ch=='\t') f|=0x040;
        if ((ch>='0' && ch<='9') || (ch>='A' && ch<='F') || (ch>='a' && ch<='f')) f|=0x080;
        memcpy(g_mem+a[3]+2*i,&f,2);
    }
    if (approximated) fprintf(stderr,"[diag] SH4 approximation: %u non-ASCII classifications set to zero\n",approximated);
    c->eax=1; c->esp+=20;
}

void imp_KERNEL32_GetStartupInfoA(CPU *c) {
    uint32_t p, size=68;
    if (c->esp > DIAG_MEM_SIZE-8) { fprintf(stderr,"[diag] invalid GetStartupInfoA stack\n"); exit(36); }
    memcpy(&p,g_mem+c->esp+4,4);
    if (p > DIAG_MEM_SIZE-size) { fprintf(stderr,"[diag] invalid GetStartupInfoA buffer\n"); exit(36); }
    memset(g_mem+p,0,size); memcpy(g_mem+p,&size,4); c->esp+=8;
}

void imp_KERNEL32_GetStdHandle(CPU *c) {
    uint32_t which;
    if (c->esp > DIAG_MEM_SIZE-8) { fprintf(stderr,"[diag] invalid GetStdHandle stack\n"); exit(36); }
    memcpy(&which,g_mem+c->esp+4,4);
    if (which<0xfffffff4u || which>0xfffffff6u) {
        fprintf(stderr,"[diag] unsupported GetStdHandle selector=%08x\n",which); exit(30);
    }
    c->eax=0; c->esp+=8; /* no attached standard streams, as in SH4 */
    fprintf(stderr,"[diag] GetStdHandle %d -> no stream\n",(int32_t)which);
}

void imp_KERNEL32_GetFileType(CPU *c) {
    uint32_t handle;
    if (c->esp > DIAG_MEM_SIZE-8) { fprintf(stderr,"[diag] invalid GetFileType stack\n"); exit(36); }
    memcpy(&handle,g_mem+c->esp+4,4);
    pthread_mutex_lock(&diag_object_lock);
    if (!handle) c->eax=0;
    else if (handle>=0xf5000000u && handle-0xf5000000u<64 && diag_files[handle-0xf5000000u]) c->eax=1;
    else { pthread_mutex_unlock(&diag_object_lock); fprintf(stderr,"[diag] GetFileType handle %08x unsupported\n",handle); exit(30); }
    pthread_mutex_unlock(&diag_object_lock);
    c->esp+=8;
}

void imp_KERNEL32_SetHandleCount(CPU *c) {
    uint32_t n;
    if (c->esp > DIAG_MEM_SIZE-8) { fprintf(stderr,"[diag] invalid SetHandleCount stack\n"); exit(36); }
    memcpy(&n,g_mem+c->esp+4,4); c->eax=n; c->esp+=8;
}

void imp_KERNEL32_GetCommandLineA(CPU *c) {
    static const char cmd[]="\"C:\\SILENT HILL 3\\sh3.exe\"";
    if (c->esp > DIAG_MEM_SIZE-4) { fprintf(stderr,"[diag] invalid GetCommandLineA stack\n"); exit(36); }
    if (!diag_command_line) {
        diag_command_line=diag_heap_alloc(diag_guest_heap,sizeof cmd,0);
        if (!diag_command_line) { fprintf(stderr,"[diag] no memory for command line\n"); exit(36); }
        memcpy(g_mem+diag_command_line,cmd,sizeof cmd);
    }
    c->eax=diag_command_line; c->esp+=4;
}

void imp_KERNEL32_GetModuleFileNameA(CPU *c) {
    static const char path[]="C:\\SILENT HILL 3\\sh3.exe";
    uint32_t a[3];
    if (c->esp > DIAG_MEM_SIZE-16) { fprintf(stderr,"[diag] invalid GetModuleFileNameA stack\n"); exit(36); }
    memcpy(a,g_mem+c->esp+4,sizeof a);
    if ((a[0]!=0 && a[0]!=0x400000) || !a[1] || !a[2] || a[1]>=DIAG_MEM_SIZE || a[2]>DIAG_MEM_SIZE-a[1]) {
        fprintf(stderr,"[diag] unsupported GetModuleFileNameA arguments\n"); exit(30);
    }
    size_t n=sizeof path-1;
    if (n>=a[2]) { fprintf(stderr,"[diag] truncated GetModuleFileNameA unsupported\n"); exit(30); }
    memcpy(g_mem+a[1],path,n+1); c->eax=(uint32_t)n; c->esp+=16;
}

void imp_KERNEL32_GetEnvironmentStringsW(CPU *c) {
    if (c->esp > DIAG_MEM_SIZE-4) { fprintf(stderr,"[diag] invalid GetEnvironmentStringsW stack\n"); exit(36); }
    if (!diag_env_w) {
        diag_env_w=diag_heap_alloc(diag_guest_heap,4,1);
        if (!diag_env_w) { fprintf(stderr,"[diag] no memory for environment\n"); exit(36); }
    }
    c->eax=diag_env_w; c->esp+=4;
}

void imp_KERNEL32_FreeEnvironmentStringsW(CPU *c) {
    uint32_t p;
    if (c->esp > DIAG_MEM_SIZE-8) { fprintf(stderr,"[diag] invalid FreeEnvironmentStringsW stack\n"); exit(36); }
    memcpy(&p,g_mem+c->esp+4,4);
    if (!p || p!=diag_env_w) { fprintf(stderr,"[diag] unexpected FreeEnvironmentStringsW address\n"); exit(30); }
    if (diag_heap_free(diag_guest_heap,p)) { fprintf(stderr,"[diag] invalid environment allocation\n"); exit(36); }
    diag_env_w=0; c->eax=1; c->esp+=8;
}

void imp_KERNEL32_WideCharToMultiByte(CPU *c) {
    uint32_t a[8];
    if (c->esp > DIAG_MEM_SIZE-36) { fprintf(stderr,"[diag] invalid WideCharToMultiByte stack\n"); exit(36); }
    memcpy(a,g_mem+c->esp+4,sizeof a);
    fprintf(stderr,"[diag] WideCharToMultiByte cp=%u flags=%x src=%08x len=%d dst=%08x cap=%u default=%08x used=%08x\n",a[0],a[1],a[2],(int32_t)a[3],a[4],a[5],a[6],a[7]);
    if ((a[0]!=0 && a[0]!=1252) || a[1]!=0 || a[6] || a[7] || !a[2] || (int32_t)a[3]<=0 ||
        a[2]>=DIAG_MEM_SIZE || a[3]>(DIAG_MEM_SIZE-a[2])/2 ||
        (a[5] && (a[4]>=DIAG_MEM_SIZE || a[5]>DIAG_MEM_SIZE-a[4]))) {
        fprintf(stderr,"[diag] unsupported WideCharToMultiByte arguments\n"); exit(30);
    }
    for (uint32_t i=0; i<a[3]; i++) {
        uint16_t ch; memcpy(&ch,g_mem+a[2]+2*i,2);
        if (diag_cp1252_encode(ch)<0) { fprintf(stderr,"[diag] unmappable WideCharToMultiByte character %04x\n",ch); exit(30); }
    }
    if (a[5] && a[5]<a[3]) { fprintf(stderr,"[diag] WideCharToMultiByte buffer too small\n"); exit(30); }
    if (a[5]) for (uint32_t i=0; i<a[3]; i++) {
        uint16_t ch; memcpy(&ch,g_mem+a[2]+2*i,2); g_mem[a[4]+i]=(uint8_t)diag_cp1252_encode(ch);
    }
    c->eax=a[3]; c->esp+=36;
}

void imp_KERNEL32_MultiByteToWideChar(CPU *c) {
    uint32_t a[6];
    if (c->esp > DIAG_MEM_SIZE-28) { fprintf(stderr,"[diag] invalid MultiByteToWideChar stack\n"); exit(36); }
    memcpy(a,g_mem+c->esp+4,sizeof a);
    fprintf(stderr,"[diag] MultiByteToWideChar cp=%u flags=%x src=%08x len=%d dst=%08x cap=%u\n",a[0],a[1],a[2],(int32_t)a[3],a[4],a[5]);
    if ((int32_t)a[3]==-1 && a[2] && a[2]<DIAG_MEM_SIZE) {
        uint8_t *end=memchr(g_mem+a[2],0,DIAG_MEM_SIZE-a[2]);
        if (!end) { fprintf(stderr,"[diag] unterminated MultiByteToWideChar input\n"); exit(36); }
        a[3]=(uint32_t)(end-(g_mem+a[2]))+1;
    }
    if ((a[0]!=0 && a[0]!=1252) || (a[1]&~1u) || !a[2] || a[2]>=DIAG_MEM_SIZE || (int32_t)a[3]<=0 || a[3]>DIAG_MEM_SIZE-a[2] ||
        (a[5] && (a[4]>=DIAG_MEM_SIZE || a[5]>(DIAG_MEM_SIZE-a[4])/2))) {
        fprintf(stderr,"[diag] unsupported MultiByteToWideChar arguments\n"); exit(30);
    }
    if (a[5] && a[5]<a[3]) { fprintf(stderr,"[diag] MultiByteToWideChar buffer too small\n"); exit(30); }
    if (a[5]) for (uint32_t i=0; i<a[3]; i++) {
        uint16_t ch=g_mem[a[2]+i]; if (ch>=0x80 && ch<=0x9f) ch=cp1252_extra[ch-0x80];
        memcpy(g_mem+a[4]+2*i,&ch,2);
    }
    c->eax=a[3]; c->esp+=28;
}

void imp_KERNEL32_LCMapStringW(CPU *c) {
    uint32_t a[6];
    if (c->esp > DIAG_MEM_SIZE-28) { fprintf(stderr,"[diag] invalid LCMapStringW stack\n"); exit(36); }
    memcpy(a,g_mem+c->esp+4,sizeof a);
    fprintf(stderr,"[diag] LCMapStringW locale=%x flags=%x src=%08x len=%d dst=%08x cap=%u\n",a[0],a[1],a[2],(int32_t)a[3],a[4],a[5]);
    if ((a[1]!=0x100 && a[1]!=0x200) || (int32_t)a[3]<=0 || !a[2] || a[2]>=DIAG_MEM_SIZE || a[3]>(DIAG_MEM_SIZE-a[2])/2 ||
        (a[5] && (!a[4] || a[4]>=DIAG_MEM_SIZE || a[5]>(DIAG_MEM_SIZE-a[4])/2 || a[5]<a[3]))) {
        fprintf(stderr,"[diag] unsupported LCMapStringW arguments\n"); exit(30);
    }
    unsigned approximate=0;
    if (a[5]) for (uint32_t i=0; i<a[3]; i++) {
        uint16_t ch; memcpy(&ch,g_mem+a[2]+2*i,2);
        if (ch>=128) approximate++;
        else if (a[1]==0x100 && ch>='A' && ch<='Z') ch+=32;
        else if (a[1]==0x200 && ch>='a' && ch<='z') ch-=32;
        memcpy(g_mem+a[4]+2*i,&ch,2);
    }
    if (approximate) fprintf(stderr,"[diag] SH4 approximation: %u non-ASCII case mappings unchanged\n",approximate);
    c->eax=a[3]; c->esp+=28;
}

gfn rt_lookup(uint32_t target) {
    unsigned lo = 0, hi = x86_functab_n;
    while (lo < hi) {
        unsigned mid = lo + (hi - lo) / 2;
        if (x86_functab[mid].a < target) lo = mid + 1;
        else hi = mid;
    }
    return lo < x86_functab_n && x86_functab[lo].a == target ? x86_functab[lo].f : NULL;
}
#ifdef SH3_PROFILE
void *rt_prof_host(void *f); void rt_prof_start(void);
#define PROF_CALL(p, stmt) do { void *pv_ = rt_prof_host((void *)(p)); stmt; rt_prof_host(pv_); } while (0)
#else
#define PROF_CALL(p, stmt) stmt
#endif
#ifdef SH3_RENDERSTATE_LAB
extern uint32_t g_diag_renderstate_target;
extern int diag_d3d_renderstate(uint32_t object,uint32_t state,uint32_t value);
static unsigned ct_renderstate_shortcuts;
int diag_renderstate_shortcut(CPU *c,uint32_t target,uint32_t object,uint32_t state,uint32_t value) {
    if (c!=diag_main_cpu || !(g_diag_perf&PERF_FASTCOM) || diag_com_profile ||
        !target || target!=g_diag_renderstate_target) return 0;
    unsigned fast=target-DIAG_HOST_BASE;
    if (fast>=__atomic_load_n(&diag_host_count,__ATOMIC_ACQUIRE) || !diag_hosts[fast].d3d) return 0;
    int sample=!((ct_com+1u)&15u);
    uint64_t t0=sample?SDL_GetPerformanceCounter():0;
    if (!diag_d3d_renderstate(object,state,value)) return 0;
    ++ct_com;
    if (++ct_renderstate_shortcuts==1u)
        fprintf(stderr,"[renderstate-shortcut] first verified native state call executed\n");
    if (sample) {
        uint64_t dt=SDL_GetPerformanceCounter()-t0;
        ct_com_ticks+=dt;ct_com_sampled++;ct_method_ticks[fast]+=dt;ct_method_n[fast]++;
    }
    return 1;
}
#endif
void rt_call(CPU *c, uint32_t target) {
#ifdef SH3_MOVIE_LAB
    if(diag_movie_trace_callbacks() && (target==0x00403be0u || target==0x00403c30u || target==0x00403c70u)) {
        uint32_t ret=0; if(c->esp<=DIAG_MEM_SIZE-4)memcpy(&ret,g_mem+c->esp,4);
        uint32_t state=0; if(g_mem && g_mem_size>0x72c0bfu)memcpy(&state,g_mem+0x72c0bcu,4);
        fprintf(stderr,"[movie-lab] movie callback target=%08x caller=%08x state_72c0bc=%08x\n",target,ret,state);
    }
#endif
    if (target>=DIAG_HOST_BASE && target-DIAG_HOST_BASE<8192) {
        if (c==diag_main_cpu) ct_com++;
        if (c==diag_main_cpu) {   /* freeze watchdog: last host function of the game thread (registry entries immutable once counted) */
            unsigned hi=target-DIAG_HOST_BASE;
            if (hi<__atomic_load_n(&diag_host_count,__ATOMIC_ACQUIRE)) diag_main_last_host=diag_hosts[hi].name;
            diag_main_host_calls++;
            if (c->esp<=DIAG_MEM_SIZE-4) memcpy((void *)&diag_main_last_ret,g_mem+c->esp,4);
        }
        if ((g_diag_perf&PERF_FASTCOM) && c==diag_main_cpu && !diag_com_profile) {
            /* Registry entries are immutable once the count is published (rt_host_addr). Direct3D interfaces are
               reachable from the game thread only (any other caller is rejected below) and share no state with the
               DirectSound methods the worker may call: neither the registry lock nor the COM lock is needed. */
            unsigned fast=target-DIAG_HOST_BASE;
            if (fast<__atomic_load_n(&diag_host_count,__ATOMIC_ACQUIRE) && diag_hosts[fast].d3d) {
                gfn fast_fn=diag_hosts[fast].f;
                /* every 16th host Direct3D call is timed (inclusive of prep recording and any wait); the report scales it by 16 */
                if (!(ct_com&15u)) {
                    uint64_t t0=SDL_GetPerformanceCounter();
                    diag_main_in_host=1; PROF_CALL(fast_fn, fast_fn(c)); diag_main_in_host=0;
                    { uint64_t dt=SDL_GetPerformanceCounter()-t0; ct_com_ticks+=dt; ct_com_sampled++; ct_method_ticks[fast]+=dt; ct_method_n[fast]++; }
                    return;
                }
                diag_main_in_host=1; PROF_CALL(fast_fn, fast_fn(c)); diag_main_in_host=0; return;
            }
        }
        if (c==diag_main_cpu) ct_com_locked++;
        pthread_mutex_lock(&diag_host_lock);
        unsigned i=target-DIAG_HOST_BASE; gfn fn=i<diag_host_count?diag_hosts[i].f:NULL;
        const char *name=fn?diag_hosts[i].name:NULL;
        pthread_mutex_unlock(&diag_host_lock);
        if (fn) {
#ifdef SH3_MOVIE_LAB
            /* Stop joins movie workers while holding the COM lock. Only the
               exact stateless stubs identified by the movie lab bypass it. */
            extern int movie_lab_is_pure_worker_stub(gfn fn);
            if (c != diag_main_cpu && diag_movie_allow_run() &&
                movie_lab_is_pure_worker_stub(fn)) {
                static unsigned qi_reported;
                if (!__atomic_exchange_n(&qi_reported, 1u, __ATOMIC_RELAXED))
                    fprintf(stderr,"[movie-lab] pure worker stub bypass active\n");
                PROF_CALL(fn, fn(c));
                return;
            }
            extern int movie_lab_try_eos_notify(gfn fn, CPU *c);
            if (c != diag_main_cpu && diag_movie_allow_run() &&
                movie_lab_try_eos_notify(fn,c)) {
                static unsigned eos_reported;
                if (!__atomic_exchange_n(&eos_reported,1u,__ATOMIC_RELAXED))
                    fprintf(stderr,"[movie-lab] owned worker EndOfStream Notify active\n");
                return;
            }
            extern int movie_lab_try_active_sample(gfn fn, CPU *c);
            int sample_kind = c != diag_main_cpu && diag_movie_allow_run() ?
                movie_lab_try_active_sample(fn, c) : 0;
            if (sample_kind) {
                static unsigned sample_reported;
                unsigned bit=1u << (sample_kind-1);
                if (!(__atomic_fetch_or(&sample_reported, bit, __ATOMIC_RELAXED)&bit))
                    fprintf(stderr,"[movie-lab] owned worker sample %s active\n",sample_kind==8?"Release":sample_kind==7?"AddRef":sample_kind==6?"GetSize":sample_kind==5?"GetActualDataLength":sample_kind==4?"GetPointer":sample_kind==3?"GetMediaType":sample_kind==2?"GetTime":"IsSync");
                return;
            }
#endif
            pthread_once(&diag_com_once,diag_init_com_lock);
            int sample=diag_com_profile && !diag_com_depth && !(++diag_com_sequence & 127u);
            uint64_t t0=sample?SDL_GetPerformanceCounter():0;
            pthread_mutex_lock(&diag_com_lock);
            uint64_t t1=sample?SDL_GetPerformanceCounter():0;
            if (diag_com_profile) ++diag_com_depth;
            if (c!=diag_main_cpu) {
                uint32_t obj=0,vt=0,slot=~0u;
                if (c->esp<=DIAG_MEM_SIZE-8) memcpy(&obj,g_mem+c->esp+4,4);
                if (obj && obj<=DIAG_MEM_SIZE-4) memcpy(&vt,g_mem+obj,4);
                if (vt && vt<=DIAG_MEM_SIZE-24*4) for (unsigned j=0;j<24;j++) {
                    uint32_t entry; memcpy(&entry,g_mem+vt+j*4,4); if (entry==target) { slot=j; break; }
                }
                if (strcmp(name,"IDirectSoundBuffer8") || (slot!=4 && slot!=9 && slot!=11 && slot!=12 && slot!=13 && slot!=18 && slot!=19 && (slot<15 || slot>17))) {
                    fprintf(stderr,"[diag] COM %s method=%u slot=%u from guest worker unsupported\n",name,i,slot); exit(30);
                }
                if (DIAG_ENV(env_verbose,"SH3_DIAG_VERBOSE")) fprintf(stderr,"[diag] experimental worker COM %s slot=%u\n",name,slot);
            }
            uint32_t ret=0; if (c->esp<=DIAG_MEM_SIZE-4) memcpy(&ret,g_mem+c->esp,4);
            if (DIAG_ENV(env_comlog,"SH3_DIAG_COMLOG")) fprintf(stderr,"[diag] COM %s method=%u caller=%08x\n",name,i,ret);
            PROF_CALL(fn, fn(c));
            if (diag_com_profile) --diag_com_depth;
            if (sample) {
                uint64_t t2=SDL_GetPerformanceCounter(), wait=t1-t0;
                diag_com_wait_ticks+=wait; diag_com_hold_ticks+=t2-t1;
                if (wait>diag_com_wait_max) diag_com_wait_max=wait;
                diag_com_worker_samples+=(c!=diag_main_cpu);
                if (++diag_com_samples==1024) {
                    double scale=1000000.0/(double)SDL_GetPerformanceFrequency();
                    fprintf(stderr,"[com-lock] samples=%u worker=%u wait_avg_us=%.3f wait_max_us=%.3f held_avg_us=%.3f\n",
                            diag_com_samples,diag_com_worker_samples,
                            scale*diag_com_wait_ticks/diag_com_samples,scale*diag_com_wait_max,
                            scale*diag_com_hold_ticks/diag_com_samples);
                    diag_com_wait_ticks=diag_com_hold_ticks=diag_com_wait_max=0;
                    diag_com_samples=diag_com_worker_samples=0;
                }
            }
            pthread_mutex_unlock(&diag_com_lock); return;
        }
    }
    if (target >= DIAG_IMPORT_BASE && target - DIAG_IMPORT_BASE < x86_iat_n) {
        PROF_CALL(x86_iat[target-DIAG_IMPORT_BASE].f, x86_iat[target-DIAG_IMPORT_BASE].f(c));
        return;
    }
    gfn f;
    if (c==diag_main_cpu) {
        ct_ind++;
        if (g_diag_perf&PERF_LOOKUP) {
            /* Direct-mapped cache in front of the binary search, as sh4vita rt_call; single writer = game thread. */
            static struct { uint32_t a; gfn f; } lc[4096];
            __typeof__(lc[0]) *e=&lc[(target^(target>>4)^(target>>13))&4095u];
            if (e->a==target && e->f) f=e->f;
            else { ct_ind_miss++; f=rt_lookup(target); if (f) { e->a=target; e->f=f; } }
        } else f=rt_lookup(target);
    } else f=rt_lookup(target);
    if (!f) {
        pthread_mutex_lock(&diag_jump_lock);
        for (DiagJmpSlot *s=diag_jumps; s; s=s->next)
            if (s->cpu==c && s->ret==target) { pthread_mutex_unlock(&diag_jump_lock); longjmp(s->jb,1); }
        pthread_mutex_unlock(&diag_jump_lock);
        uint32_t ret=0;
        if (c->esp<=DIAG_MEM_SIZE-4) memcpy(&ret,g_mem+c->esp,4);
#ifdef SH3_BAD_TARGET_LAB
        if (getenv("SH3_DIAG_BAD_TARGET_DETAIL") && ret==0x00461df5u) {
            uint32_t obj=0,vt=0,slot=0;
            if (c->esp<=DIAG_MEM_SIZE-8) memcpy(&obj,g_mem+c->esp+4,4);
            if (obj && obj<=DIAG_MEM_SIZE-4) memcpy(&vt,g_mem+obj,4);
            if (vt && vt<=DIAG_MEM_SIZE-0x44) memcpy(&slot,g_mem+vt+0x40,4);
            fprintf(stderr,"[bad-target] eax=%08x ecx=%08x object=%08x vtable=%08x slot16=%08x\n",
                    c->eax,c->ecx,obj,vt,slot);
        }
#endif
        fprintf(stderr, "[diag] unknown target %08x return=%08x esp=%08x\n", target,ret,c->esp); exit(31);
    }
#ifdef SH3_MOVIE_LAB
    if(diag_movie_trace_callbacks() && target==0x00403be0u) {
        f(c);
        fprintf(stderr,"[movie-lab] movie buffer callback result=%08x\n",c->eax);
        if(c->eax && c->eax<=g_mem_size-8) {
            uint32_t pitch,ptr;memcpy(&pitch,g_mem+c->eax,4);memcpy(&ptr,g_mem+c->eax+4,4);
            fprintf(stderr,"[movie-lab] movie buffer pitch=%u pointer=%08x\n",pitch,ptr);
        }
        return;
    }
#endif
#ifdef __vita__
    /* Diagnostic omission only: keep the outer worker scheduler and its
       completion signalling alive. Never omit callbacks on the game thread.
       The independent clock bound restores execution even without swaps. */
    extern int g_diag_worker_budget;
    extern unsigned g_diag_worker_eligible;
    extern unsigned g_diag_worker_skipped[2];
    if (g_diag_worker_budget && c!=diag_main_cpu &&
        (target==0x0060f740u || target==0x00613370u)) {
        Uint64 now=SDL_GetTicks64();
        unsigned phase=now>=120000u && now<150000u?1u:
                       now>=160000u && now<190000u?2u:0u;
        if (phase && __atomic_load_n(&g_diag_worker_eligible,__ATOMIC_RELAXED) &&
            (target==0x00613370u || phase==2u)) {
            if (c->esp>DIAG_MEM_SIZE-4) rt_fatal("[diag] invalid budget worker stack");
            __atomic_fetch_add(&g_diag_worker_skipped[target==0x00613370u?1:0],1u,__ATOMIC_RELAXED);
            c->eax=0; c->esp+=4; return;
        }
    }
    /* Optional wall-time samples of the two callbacks observed in worker 258.
       The calls still execute normally; elapsed time can include blocking. */
    if ((target==0x0060f740u || target==0x00613370u) && getenv("SH3_DIAG_WORKER_TIMING")) {
        static uint64_t ticks[2]; static unsigned counts[2];
        unsigned i=target==0x0060f740u?0u:1u;
        uint64_t before=SDL_GetPerformanceCounter();
        f(c);
        uint64_t elapsed=SDL_GetPerformanceCounter()-before;
        __atomic_fetch_add(&ticks[i],elapsed,__ATOMIC_RELAXED);
        unsigned count=__atomic_add_fetch(&counts[i],1u,__ATOMIC_RELAXED);
        if (!(count%1024u)) {
            uint64_t total=__atomic_load_n(&ticks[i],__ATOMIC_RELAXED);
            fprintf(stderr,"[worker-time] target=%08x calls=%u total_ms=%.2f avg_us=%.2f\n",
                    target,count,(double)total*1000.0/SDL_GetPerformanceFrequency(),
                    (double)total*1000000.0/SDL_GetPerformanceFrequency()/count);
        }
        return;
    }
#endif
    f(c);
}

uint32_t rt_host_addr(gfn f, const char *name) {
    pthread_mutex_lock(&diag_host_lock);
    unsigned count=diag_host_count;
    for (unsigned i=0; i<count; i++) if (diag_hosts[i].f==f) { pthread_mutex_unlock(&diag_host_lock); return DIAG_HOST_BASE+i; }
    if (count>=8192) { fprintf(stderr,"[diag] host function table full\n"); exit(36); }
    unsigned i=count; diag_hosts[i].f=f; diag_hosts[i].name=name;
    diag_hosts[i].d3d=name && !strncmp(name,"IDirect3D",9);
    __atomic_store_n(&diag_host_count,count+1,__ATOMIC_RELEASE);   /* lock-free readers see a complete entry */
    pthread_mutex_unlock(&diag_host_lock);
    return DIAG_HOST_BASE+i;
}
uint32_t rt_guest_call(CPU *c, uint32_t fn, int nargs, ...) {
    if (nargs<0 || nargs>16 || c->esp>DIAG_MEM_SIZE || c->esp<4u*(unsigned)(nargs+1))
        rt_fatal("[diag] invalid guest callback stack");
    uint32_t saved=c->esp,args[16]; va_list ap;
    va_start(ap,nargs); for (int i=0;i<nargs;i++) args[i]=va_arg(ap,uint32_t); va_end(ap);
    for (int i=nargs-1;i>=0;i--) { c->esp-=4; memcpy(g_mem+c->esp,&args[i],4); }
    uint32_t sentinel=0xfffffff0u; c->esp-=4; memcpy(g_mem+c->esp,&sentinel,4);
    rt_call(c,fn); c->esp=saved; return c->eax;
}
void rt_log(const char *fmt, ...) {
    va_list ap; va_start(ap,fmt); vfprintf(stderr,fmt,ap); va_end(ap);
}
/* Check build of the selective write-back pass (docs: C5). reg 0..7 = epilogue of F_fn, 8..15 = reload after a call made by F_fn. */
void rt_c5_violation(CPU *c, uint32_t fn, int reg) {
    static unsigned n; (void)c;
    unsigned k=__atomic_add_fetch(&n,1u,__ATOMIC_RELAXED);
    if (k<=64u || !(k&(k-1u))) fprintf(stderr,"[c5-violation] #%u function F_%08x kind=%s register=%d\n",k,fn,reg<8?"epilogue":"reload",reg&7);
}
unsigned g_ct_rb, g_ct_rb_tiny; uint64_t g_ct_rb_ticks;   /* surf_readback calls (all / 1x1) and wall ticks the game thread spent in them */
/* Counters of the game thread since the previous report, per presented frame (called from the SH4_STATS block). */
void diag_perf_report(double frames) {
#ifdef SH3_RENDERSTATE_LAB
    fprintf(stderr,"[renderstate-shortcut] cumulative=%u\n",ct_renderstate_shortcuts);
#endif
    if (frames<=0) return;
    double ms=1000.0/(double)SDL_GetPerformanceFrequency()/frames;
    fprintf(stderr,"[perfct] perf=%02x per frame: com %.0f (locked %.0f) indirect %.0f (lookup miss %.0f) critsec %.0f; "
            "sleep %.2f calls %.2f ms asked %.2f ms spent; opens %.3f (cached %.3f) %.2f ms\n",
            g_diag_perf,ct_com/frames,ct_com_locked/frames,ct_ind/frames,ct_ind_miss/frames,ct_cs/frames,
            ct_sleep/frames,ct_sleep_ms/frames,(double)ct_sleep_ticks*ms,ct_open/frames,ct_open_hit/frames,(double)ct_open_ticks*ms);
    if (ct_pace) fprintf(stderr,"[perfct] pace: %.2f limiter calls per frame, %.1f%% late (wait skipped), %.2f ticks per call, target %u ticks\n",
                         ct_pace/frames,100.0*ct_pace_late/ct_pace,(double)ct_pace_ticks/ct_pace,ct_pace_count);
    if (ct_pace) fprintf(stderr,"[perfct] limiter: %.2f ms per frame inside the game limiter (waits and Sleep(0) spin, counted as CPU by cpu-thread)\n",ct_pace_ns/1e6/frames);
    ct_pace=ct_pace_late=ct_pace_ticks=0; ct_pace_ns=0;
    for (unsigned k=0;k<8;k++) if (ct_sleep_site[k].n)
        fprintf(stderr,"[perfct] sleep site return=%08x: %.2f calls %.2f ms asked per frame%s\n",ct_sleep_site[k].ret,
                ct_sleep_site[k].n/frames,ct_sleep_site[k].ms/frames,k==7?" (last slot aggregates other sites)":"");
    memset(ct_sleep_site,0,sizeof ct_sleep_site);
    fprintf(stderr,"[perfct] readbacks %.2f per frame (1x1 %.2f), %.2f ms per frame waiting in them\n",g_ct_rb/frames,g_ct_rb_tiny/frames,(double)g_ct_rb_ticks*ms);
    if (ct_com_sampled) fprintf(stderr,"[perfct] host D3D calls (sampled 1/16): %.2f ms per frame inclusive, %.2f us per call\n",(double)ct_com_ticks*((double)ct_com/(double)ct_com_sampled)*ms,(double)ct_com_ticks*1000000.0/(double)SDL_GetPerformanceFrequency()/ct_com_sampled);
    if (ct_com_sampled) { /* top host D3D methods by sampled time (x16 = estimated per-frame cost) */
        for (unsigned r=0;r<12;r++) { unsigned best=0; uint64_t bt=0; for (unsigned k=0;k<8192;k++) if (ct_method_ticks[k]>bt) { bt=ct_method_ticks[k]; best=k; }
            if (!bt) break; { unsigned first=best; while (first && diag_hosts[first-1].name==diag_hosts[best].name) first--; fprintf(stderr,"[perfct] d3d slot %u ",best-first); }
            fprintf(stderr,"[perfct] d3d method %s: %.3f ms/frame, %.1f calls/frame, %.2f us/call\n",diag_hosts[best].name?diag_hosts[best].name:"?",
                (double)bt*16.0*ms,ct_method_n[best]*16.0/frames,(double)bt*1e6/(double)SDL_GetPerformanceFrequency()/ct_method_n[best]); ct_method_ticks[best]=0; }
        memset(ct_method_ticks,0,sizeof ct_method_ticks); memset(ct_method_n,0,sizeof ct_method_n); }
    ct_com_ticks=0; ct_com_sampled=0;
    g_ct_rb=g_ct_rb_tiny=0; g_ct_rb_ticks=0;
    ct_com=ct_com_locked=ct_ind=ct_ind_miss=ct_cs=ct_sleep=ct_sleep_ms=ct_open=ct_open_hit=0; ct_sleep_ticks=ct_open_ticks=0;
}
/* One-shot cost of the primitives on the call paths, to turn call counts into time (profilecpu only). */
static void diag_calibrate(void) {
    enum { N=20000 };
    double us=1000000.0/(double)SDL_GetPerformanceFrequency()/N;
    pthread_mutex_t plain=PTHREAD_MUTEX_INITIALIZER; volatile unsigned sink=0;
    pthread_once(&diag_com_once,diag_init_com_lock);
    uint64_t t0=SDL_GetPerformanceCounter();
    for (int i=0;i<N;i++) { pthread_mutex_lock(&plain); pthread_mutex_unlock(&plain); }
    uint64_t t1=SDL_GetPerformanceCounter();
    for (int i=0;i<N;i++) { pthread_mutex_lock(&diag_com_lock); pthread_mutex_unlock(&diag_com_lock); }
    uint64_t t2=SDL_GetPerformanceCounter();
    for (int i=0;i<N;i++) sink+=getenv("SH3_DIAG_COMLOG")!=NULL;
    uint64_t t3=SDL_GetPerformanceCounter();
    for (int i=0;i<N;i++) sink+=rt_lookup(x86_functab[((unsigned)i*37u)%x86_functab_n].a)!=NULL;
    uint64_t t4=SDL_GetPerformanceCounter();
    for (int i=0;i<N;i++) sink+=(unsigned)SDL_GetPerformanceCounter();
    uint64_t t5=SDL_GetPerformanceCounter();
    fprintf(stderr,"[calib] us per op: mutex pair %.3f, recursive mutex pair %.3f, getenv %.3f, rt_lookup %.3f, timer read %.3f (n=%d, sink=%u)\n",
            (double)(t1-t0)*us,(double)(t2-t1)*us,(double)(t3-t2)*us,(double)(t4-t3)*us,(double)(t5-t4)*us,(int)N,sink);
}
#include "diag_bench.inc"
#ifdef SH3_PARTICLE_NATIVE_LAB
static void diag_all_native_set(int on) {
    extern int g_particle_native, g_particle_draw_native, g_qsort_native, g_collide_native,
        g_texfill_native, g_prand_native, g_blur_native, g_pplanes_native, g_pbox_native,
        g_pcyl_native, g_pupdate_native, g_wall_native, g_copy64_native;
    g_particle_native=g_particle_draw_native=g_qsort_native=g_collide_native=on;
    g_texfill_native=g_prand_native=g_blur_native=g_pplanes_native=g_pbox_native=on;
    g_pcyl_native=g_pupdate_native=g_wall_native=g_copy64_native=on;
#ifdef SH3_SHADOW_NATIVE_LAB
    extern int g_shadow_native,g_shadow_strip;
    g_shadow_native=g_shadow_strip=on;
#endif
}
#endif
#ifdef __vita__
/* latepace. F_0041b2c0 is the game's frame limiter (this executable only; linked with --wrap). State: 0x72c7e8 target
   frame length in ticks, 0x72c790 ticks per second (0 = 60), 0x72c808 last deadline and 0x72c810 division remainder
   (64-bit, QueryPerformanceCounter units), 0x72c818 counter frequency, 0x72c820 counter available. It advances the
   deadline one tick at a time; past deadlines are skipped, the first future one is waited for (Sleep(ms-1), then a
   Sleep(0) spin), and it returns once at least `target` ticks were consumed and one wait happened. A frame that is
   already late therefore still waits for the next 1/60 s boundary. Outputs: 0x72c7f4 ticks consumed (the game's time
   step), 0x72c7f8 waits-1, 0x72c7f0 running tick total.
   When the frame is late this wrapper performs the same deadline arithmetic and claims that first future deadline
   without waiting: tick counts still add up to wall time (the next frame starts earlier and consumes fewer ticks),
   only the presentation is no longer rounded up to 1/60 s. Every other case runs the original function. */
extern void __real_F_0041b2c0(CPU *);
static void diag_pace(CPU *c);
/* Same-run A/B: a lab switch whose value is "ab" follows g_ab_on, which alternates every 300 frames; window boundaries are
   logged as "[ab] frame F ms T on S" (tools/ab_windows.py). Removes the run-to-run drift of separate measurements. */
int g_ab_on, g_ab_probe, g_ab_shadow, g_ab_particle, g_ab_texeager, g_ab_pdraw, g_ab_rand, g_ab_qsort, g_ab_collide, g_ab_sstrip, g_ab_texfill, g_ab_prand, g_ab_blur, g_ab_all, g_ab_pplanes, g_ab_pbox, g_ab_pcyl, g_ab_pupdate, g_ab_wall, g_ab_copy64;
int g_pace60_park;
static int g_pace60_clamp;
static int g_pace60_all_rooms;
/* Even 30 Hz stepping (2026-11-16, Valentin: Heather's run is not smooth): the forced one-tick target made every frame
   late by a different amount, so the logic advanced 1, 2 or 3 ticks (11.7 / 23.3 / 35 units for Heather) between
   presented frames of 24-36 ms (bench run/analyze.py: 232x1, 337x2, 15x3 ticks): constant speed in time but uneven
   in picture. The engine's own target (2 ticks) is left alone unless it asks for 3 or more (heavy scenes keep the
   forced one-tick bounded policy). Marker pace60force restores the previous behaviour for A/B. */
static int g_pace60_even=1;
static int g_ab_clip,g_ab_pace60_park; static unsigned long long ab_pace_ns;
static int diag_pace60_scene(CPU *c) {
    uint32_t room;uint64_t freq;
    if (c!=diag_main_cpu || bench_state_top()!=5) return 0;
    unsigned stage=g_mem[0x70e66d9u];
    if (stage<1 || stage>39) return 0;
    memcpy(&room,g_mem+0x897e48u,4);memcpy(&freq,g_mem+0x72c818u,8);
    return (g_pace60_all_rooms || (stage==33 && room==210)) &&
           freq==1000000000ull && g_mem[0x72c820u];
}
int g_fps_mode_override=-1;   /* frame-rate setting override (marker fpsmode), see diag_pace60_policy */
#ifdef SH3_FRAME_POLICY_LAB
extern void F_005f15e0_translated(CPU *c);
/* Frame-rate setting (Options row 9, saved control-type byte 0x70e66cd; Valentin 2026-11-16 - the Vita screen is 60 Hz,
   never above 60):
   0 = "60" (default): fixed steps only, 60, 30 or 20 images/s - the engine's own adaptive target of 1, 2 or 3 ticks
       (F_005f15e0, limiter waits whole ticks, so frames are exactly 16.7 / 33.3 / 50 ms; below 20 it simply drops).
   1 = "30": same, but the target never goes below 2 ticks: 30 or 20.
   2 = "unlimited": variable, at most 60 - the original force-60 path (target one tick, the logic consumes the ticks
       actually elapsed, 1..3) in gameplay; menus/loadings keep the engine's adaptive target, also at most 60.
   Marker fpsmode (0/1/2) / env SH3_FPS_MODE overrides the saved value for benches. */
static unsigned diag_fps_mode(void) {
    return g_fps_mode_override>=0?(unsigned)g_fps_mode_override:g_mem[0x70e66cdu];
}
static void diag_pace_force60(CPU *c) {
    /* Original force-60 path also publishes the ACTUAL consumed ticks to
       bcc630, which F_0060da50 uses for simulation dt. Changing only the
       low-level target would leave adaptive requested ticks driving physics. */
    uint8_t saved=g_mem[0xbcc63eu];g_mem[0xbcc63eu]=1;
    F_005f15e0_translated(c);
    g_mem[0xbcc63eu]=saved;
    /* The original adaptive path caps its logical step at three ticks, unlike force-60 after a long loading stall. */
    uint32_t raw,selected;memcpy(&raw,g_mem+0xbcc630u,4);selected=raw;
    if (g_pace60_clamp && raw>3) { selected=3;memcpy(g_mem+0xbcc630u,&selected,4); }
    { static unsigned reported;
      if (raw>3 && reported++<20)
          fprintf(stderr,"[pace-policy] ms=%llu raw_ticks=%u selected_ticks=%u clamp=%d\n",
              (unsigned long long)SDL_GetTicks64(),raw,selected,g_pace60_clamp);
    }
}
int diag_pace60_policy(CPU *c) {
    unsigned mode=c==diag_main_cpu?diag_fps_mode():0;
    if (mode==2 && diag_pace60_scene(c)) { diag_pace_force60(c); return 1; }
    F_005f15e0_translated(c);
    if (mode==1 && c==diag_main_cpu) {
        uint32_t target; memcpy(&target,g_mem+0x72c7e8u,4);
        if (target<2) { target=2; memcpy(g_mem+0x72c7e8u,&target,4); }
    }
    return 1;
}
#endif
static void diag_ab_tick(void) {
    static unsigned last=~0u; extern unsigned g_frame; extern int g_diag_native_clip;
#ifdef SH3_FUNCTIME_LAB
    const int functime=1;
#else
    const int functime=0;
#endif
    extern int g_glstatecache_ab,g_glstatecache_phase;
    if (!g_ab_clip && !g_ab_probe && !g_ab_shadow && !g_ab_particle && !g_ab_texeager && !g_ab_pdraw && !g_ab_rand && !g_ab_qsort && !g_ab_collide && !g_ab_sstrip && !g_ab_texfill && !g_ab_prand && !g_ab_blur && !g_ab_all && !g_ab_pplanes && !g_ab_pbox && !g_ab_pcyl && !g_ab_pupdate && !g_ab_wall && !g_ab_copy64 && !g_glstatecache_ab && !g_ab_pace60_park && !functime) return;
    unsigned w=g_frame/300; if (w==last) return; last=w; g_ab_on=(int)(w&1u);
    if (g_glstatecache_ab) __atomic_store_n(&g_glstatecache_phase,g_ab_on,__ATOMIC_RELEASE);
    if (g_ab_pace60_park) g_pace60_park=g_ab_on;
    if (g_ab_clip) g_diag_native_clip=g_ab_on;
    if (g_ab_texeager) { extern int g_tex_eager; g_tex_eager=g_ab_on; }
#ifdef SH3_RAND_LAB
    if (g_ab_rand) { extern int diag_rand_enabled; diag_rand_enabled=g_ab_on; fprintf(stderr,"[native-rand] window before frame %u, native %d\n",g_frame,g_ab_on); }
#endif
#ifdef SH3_PARTICLE_NATIVE_LAB
    { extern int g_particle_native; extern unsigned long long g_pu_ns; extern unsigned g_pu_n;
      fprintf(stderr,"[particle-native] window before frame %u: %.2f ms in F_0047abb0 (%u calls), native %d\n",g_frame,g_pu_ns/1e6,g_pu_n,g_particle_native);
      g_pu_ns=0; g_pu_n=0;
      if (g_ab_particle) g_particle_native=g_ab_on; }
    { extern int g_particle_draw_native; extern unsigned long long g_pd_ns; extern unsigned g_pd_n, g_pd_native, g_pd_refused;
      fprintf(stderr,"[particle-draw] window before frame %u: %.2f ms in F_00479e30 (%u calls), native %d, loops native %u refused %u\n",
              g_frame,g_pd_ns/1e6,g_pd_n,g_particle_draw_native,g_pd_native,g_pd_refused);
      g_pd_ns=0; g_pd_n=0; g_pd_native=g_pd_refused=0;
      if (g_ab_pdraw) g_particle_draw_native=g_ab_on; }
    { extern int g_qsort_native; extern unsigned long long g_qs_ns; extern unsigned g_qs_n, g_qs_native;
      fprintf(stderr,"[qsort-native] window before frame %u: %.2f ms in qsort (%u calls, %u native), native %d\n",g_frame,g_qs_ns/1e6,g_qs_n,g_qs_native,g_qsort_native);
      g_qs_ns=0; g_qs_n=g_qs_native=0;
      if (g_ab_qsort) g_qsort_native=g_ab_on; }
    { extern int g_collide_native; extern unsigned long long g_co_ns; extern unsigned g_co_n;
      fprintf(stderr,"[collide-native] window before frame %u: %.2f ms in F_004f3300 (%u calls), native %d\n",g_frame,g_co_ns/1e6,g_co_n,g_collide_native);
      g_co_ns=0; g_co_n=0;
      if (g_ab_collide) g_collide_native=g_ab_on; }
    { extern int g_texfill_native; if (g_ab_texfill) g_texfill_native=g_ab_on; }
    { extern int g_prand_native; if (g_ab_prand) g_prand_native=g_ab_on; }
    { extern int g_blur_native; if (g_ab_blur) g_blur_native=g_ab_on; }
    { extern int g_pplanes_native; if (g_ab_pplanes) g_pplanes_native=g_ab_on; }
    { extern int g_pbox_native; if (g_ab_pbox) g_pbox_native=g_ab_on; }
    { extern int g_pcyl_native; if (g_ab_pcyl) g_pcyl_native=g_ab_on; }
    { extern int g_pupdate_native; if (g_ab_pupdate) g_pupdate_native=g_ab_on; }
    { extern int g_wall_native; if (g_ab_wall) g_wall_native=g_ab_on; }
    { extern int g_copy64_native; if (g_ab_copy64) g_copy64_native=g_ab_on; }
    if (g_ab_all) {   /* every native kernel of the labs at once (marker allnative=ab) */
        diag_all_native_set(g_ab_on);
        fprintf(stderr,"[allnative] window before frame %u, native %d\n",g_frame,g_ab_on);
    }
#endif
#ifdef SH3_SHADOW_NATIVE_LAB
    { extern int g_shadow_native; extern unsigned long long g_sn_ns; extern unsigned g_sn_native, g_sn_translated, g_sn_refused;
      fprintf(stderr,"[shadow-native] window before frame %u: %.2f ms in F_0048e140, native %u translated %u refused %u\n",
              g_frame,g_sn_ns/1e6,g_sn_native,g_sn_translated,g_sn_refused);
      { extern unsigned long long g_sn_part_ns[3]; fprintf(stderr,"[shadow-native] parts: GetTransform %.2f ms, setup %.2f ms, loop %.2f ms\n",g_sn_part_ns[0]/1e6,g_sn_part_ns[1]/1e6,g_sn_part_ns[2]/1e6); g_sn_part_ns[0]=g_sn_part_ns[1]=g_sn_part_ns[2]=0; }
      { extern unsigned long long g_sf_ns; extern unsigned g_sf_native, g_sf_translated;
        fprintf(stderr,"[shadow-native] window before frame %u: %.2f ms in F_0048f040, native %u translated %u\n",g_frame,g_sf_ns/1e6,g_sf_native,g_sf_translated);
        g_sf_ns=0; g_sf_native=g_sf_translated=0; }
      g_sn_ns=0; g_sn_native=g_sn_translated=g_sn_refused=0;
      if (g_ab_shadow) g_shadow_native=g_ab_on; }
    { extern int g_shadow_strip; extern unsigned g_ss_native;
      fprintf(stderr,"[shadow-strip] window before frame %u: native calls %u, native %d\n",g_frame,g_ss_native,g_shadow_strip);
      g_ss_native=0; if (g_ab_sstrip) g_shadow_strip=g_ab_on; }
#endif
#ifdef SH3_FUNCTIME_LAB
    { extern unsigned long long g_ft_ns[4]; extern unsigned g_ft_n[4]; static const char *nm[4]={"F_0048f040","F_0047abb0","F_00479e30","F_00471420"};
      for (int k=0;k<4;k++) { fprintf(stderr,"[functime] window before frame %u: %s %.2f ms, %u calls\n",g_frame,nm[k],g_ft_ns[k]/1e6,g_ft_n[k]); g_ft_ns[k]=0; g_ft_n[k]=0; } }
#endif
    struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t);
    fprintf(stderr,"[ab] frame %u ms %llu on %d pace_ms %llu\n",g_frame,(unsigned long long)t.tv_sec*1000ull+t.tv_nsec/1000000,g_ab_on,ab_pace_ns/1000000ull);
}
void __wrap_F_0041b2c0(CPU *c) {
    if (c==diag_main_cpu) { bench_tick(c); diag_ab_tick(); }
    /* Park-only lab: reuse the engine's existing one-tick/60 Hz limiter mode.
       Restore its requested target immediately afterwards. Deadline/remainder,
       elapsed simulation ticks and the original register path stay in diag_pace.
       This changes scheduling, not the game's measured tick progression. */
    uint32_t requested=0,room=0; int pace60=0,lab_active=0;
    if (c==diag_main_cpu && (g_pace60_park || g_ab_pace60_park)) {
        uint64_t freq; memcpy(&freq,g_mem+0x72c818u,8); memcpy(&room,g_mem+0x897e48u,4);
        memcpy(&requested,g_mem+0x72c7e8u,4);
        if (g_pace60_park && diag_pace60_scene(c) && requested>=1 && requested<=60 && freq==1000000000ull) {
            lab_active=1;
            if (requested>1 && (!g_pace60_even || (g_fps_mode_override>=0?(unsigned)g_fps_mode_override:g_mem[0x70e66cdu])==2u)) { uint32_t one=1; memcpy(g_mem+0x72c7e8u,&one,4); pace60=1; }
        }
    }
    { static int previous=-1;
      if (c==diag_main_cpu && (g_pace60_park || g_ab_pace60_park) && previous!=lab_active) {
          previous=lab_active; uint32_t total;memcpy(&total,g_mem+0x72c7f0u,4);
          fprintf(stderr,"[pace60park] ms=%llu phase=%d stage=%u room=%u target_ticks=%u total_ticks=%u\n",
                  (unsigned long long)SDL_GetTicks64(),lab_active,g_mem[0x70e66d9u],room,requested,total);
      } }
    /* The on-time path spins on Sleep(0): cpu_ms_est of the game thread saturates at the frame length and the rate sits on
       the 3-tick plateau, so neither shows a gain between 2 and 3 ticks. This is the only direct measure of the work. */
    struct timespec a,b; int ok=c==diag_main_cpu && !clock_gettime(CLOCK_MONOTONIC,&a);
    diag_pace(c);
    if (pace60) memcpy(g_mem+0x72c7e8u,&requested,4);
    if (ok && !clock_gettime(CLOCK_MONOTONIC,&b)) { unsigned long long d=(unsigned long long)((long long)(b.tv_sec-a.tv_sec)*1000000000ll+(b.tv_nsec-a.tv_nsec)); ct_pace_ns+=d; ab_pace_ns+=d; }
}
static void diag_pace(CPU *c) {
    uint32_t count, rate; uint64_t freq, deadline, rem; struct timespec ts;
    memcpy(&count,g_mem+0x72c7e8u,4); memcpy(&rate,g_mem+0x72c790u,4);
    if (c!=diag_main_cpu || !count || !g_mem[0x72c820u]) { __real_F_0041b2c0(c); return; }
    if (!rate) rate=60;
    if ((int32_t)count>(int32_t)rate) count=rate;
    ct_pace++; ct_pace_count=count;
    memcpy(&freq,g_mem+0x72c818u,8); memcpy(&deadline,g_mem+0x72c808u,8); memcpy(&rem,g_mem+0x72c810u,8);
    if (!(g_diag_perf&PERF_LATEPACE) || freq!=1000000000ull || (int32_t)rate<=0 || (int32_t)count<=0 || clock_gettime(CLOCK_MONOTONIC,&ts)) {
        __real_F_0041b2c0(c);
        uint32_t used; memcpy(&used,g_mem+0x72c7f4u,4); ct_pace_ticks+=used; return;
    }
    uint64_t now=(uint64_t)ts.tv_sec*1000000000ull+(uint64_t)ts.tv_nsec;
    uint32_t ticks=0;
    if (now>=deadline && now-deadline<=freq)   /* otherwise early, or the original one-second resynchronisation */
        do { rem+=freq; deadline+=rem/rate; rem%=rate; ticks++; } while (deadline<=now && ticks<=rate);
    uint32_t wait_count=count;
    if (g_diag_perf&PERF_PACEPROBE) {
        /* Contribution probe only. Keep deadline/remainder and simulation tick
           accounting; omit the extra wait for at most 30s in this one scene. */
        static int old_phase=-1;
        uint64_t app_ms=SDL_GetTicks64();uint32_t room,total;
        memcpy(&room,g_mem+0x897e48u,4);memcpy(&total,g_mem+0x72c7f0u,4);
        unsigned stage=g_mem[0x70e66d9u];
        int phase=stage==33 && room==210 && app_ms>=150000 && app_ms<180000;
        if (phase!=old_phase) {
            fprintf(stderr,"[pace-probe] ms=%llu phase=%d stage=%u room=%u target_ticks=%u total_ticks=%u\n",
                    (unsigned long long)app_ms,phase,stage,room,count,total);
            old_phase=phase;
        }
        if (phase && count>1) wait_count=1;
    }
    if (ticks<=wait_count || deadline<=now) {       /* on time: the original paces the frame to its target */
        __real_F_0041b2c0(c);
        uint32_t used; memcpy(&used,g_mem+0x72c7f4u,4); ct_pace_ticks+=used; return;
    }
    uint32_t total, zero=0;
    memcpy(&total,g_mem+0x72c7f0u,4); total+=ticks;
    memcpy(g_mem+0x72c810u,&rem,8); memcpy(g_mem+0x72c808u,&deadline,8);
    memcpy(g_mem+0x72c7f4u,&ticks,4); memcpy(g_mem+0x72c7f8u,&zero,4); memcpy(g_mem+0x72c7f0u,&total,4);
    c->eax=ticks; c->edx=0; c->ecx=total; c->esp+=4;
    ct_pace_late++; ct_pace_ticks+=ticks;
}
/* logasync: stderr is fully buffered and written by this thread, so a log line costs the caller no ux0 write.
   A hard fault can lose up to the last period of lines; use perf= without logasync to debug crashes. */
static void *diag_log_flusher(void *arg) {
    (void)arg;
    for (;;) { usleep(200000); fflush(stderr); }
    return NULL;
}
#else
extern void __real_F_0041b2c0(CPU *);
int g_ab_on, g_ab_probe, g_ab_shadow, g_ab_particle, g_ab_texeager, g_ab_pdraw, g_ab_rand, g_ab_qsort, g_ab_collide, g_ab_sstrip, g_ab_texfill, g_ab_prand, g_ab_blur, g_ab_all, g_ab_pplanes, g_ab_pbox, g_ab_pcyl, g_ab_pupdate, g_ab_wall, g_ab_copy64; static int g_ab_clip; /* A/B windows are Vita-only; the switches still parse */
void __wrap_F_0041b2c0(CPU *c) { if (c==diag_main_cpu) bench_tick(c); __real_F_0041b2c0(c); }
#endif
/* Per-call wall time of the lab wrappers: off unless marker labtiming (SH3_LAB_TIMING=1); two clock_gettime per call
   cost ~2 ms/frame on the Vita at ~1500 wrapped calls per frame and would hide the gains being measured. */
int g_lab_timing;
#ifdef SH3_PARTICLE_NATIVE_LAB
#ifdef SH3_FUNCTIME_LAB
#error "SH3_FUNCTIME_LAB wraps F_0047abb0 too"
#endif
/* Wall time of the particle update per A/B window (the native kernel itself is switched inside unit f040). */
extern void __real_F_0047abb0(CPU *); unsigned long long g_pu_ns; unsigned g_pu_n;
int g_pupdate_native; int particle_update_native(CPU *c);   /* native body of the update (particle_native.c) */
void __wrap_F_0047abb0(CPU *c) {
    struct timespec a = {0, 0}, b = {0, 0}; if (g_lab_timing) clock_gettime(CLOCK_MONOTONIC, &a);
    if (!(g_pupdate_native && particle_update_native(c))) __real_F_0047abb0(c);
    if (g_lab_timing) clock_gettime(CLOCK_MONOTONIC, &b);
    g_pu_n++; g_pu_ns += (unsigned long long)((long long)(b.tv_sec - a.tv_sec) * 1000000000ll + (b.tv_nsec - a.tv_nsec));
}
/* Native CRT qsort for the two hot comparators (qsort_native.c), timed per A/B window. */
extern void __real_F_00663a30(CPU *); int qsort_native(CPU *c); extern int g_qsort_native; unsigned long long g_qs_ns; unsigned g_qs_n, g_qs_native;
void __wrap_F_00663a30(CPU *c) {
    struct timespec a = {0, 0}, b = {0, 0}; if (g_lab_timing) clock_gettime(CLOCK_MONOTONIC, &a);
    if (g_qsort_native && qsort_native(c)) { if (!g_qs_native++) { static int once; if (!once) { once = 1; fprintf(stderr, "[qsort-native] native qsort running\n"); } } }
    else __real_F_00663a30(c);
    if (g_lab_timing) clock_gettime(CLOCK_MONOTONIC, &b);
    g_qs_n++; g_qs_ns += (unsigned long long)((long long)(b.tv_sec - a.tv_sec) * 1000000000ll + (b.tv_nsec - a.tv_nsec));
}
/* Native particle / cylinder collision F_00479340: switched inside the f040 copy (particle_unit.py). */
int g_pcyl_native;
/* Native particle box wrap F_00476090 (particle_native.c). */
extern void __real_F_00476090(CPU *); int particle_box_native(CPU *c); int g_pbox_native;
void __wrap_F_00476090(CPU *c) { if (!(g_pbox_native && particle_box_native(c))) __real_F_00476090(c); }
/* Native particle / plane collision F_00478850 (particle_native.c). */
extern void __real_F_00478850(CPU *); int particle_planes_native(CPU *c); int g_pplanes_native;
void __wrap_F_00478850(CPU *c) { if (!(g_pplanes_native && particle_planes_native(c))) __real_F_00478850(c); }
/* Native toroidal image smoothing F_00446b30 (blur_native.c). */
extern void __real_F_00446b30(CPU *); int blur_native(CPU *c); extern int g_blur_native;
void __wrap_F_00446b30(CPU *c) {
    static int once;
    if (g_blur_native && blur_native(c)) { if (!once) { once = 1; fprintf(stderr, "[blur-native] native image smoothing running\n"); } return; }
    __real_F_00446b30(c);
}
/* Native particle randomizer F_00475fb0 (particle_native.c, with F_0060bfa0 and rand inline). */
extern void __real_F_00475fb0(CPU *); int particle_rand_native(CPU *c); int g_prand_native;
void __wrap_F_00475fb0(CPU *c) {
    static int once;
    if (g_prand_native && particle_rand_native(c)) { if (!once) { once = 1; fprintf(stderr, "[prand-native] native particle randomizer running\n"); } return; }
    __real_F_00475fb0(c);
}
/* Native 64-byte copy F_00678070 (texfill_native.c). */
extern void __real_F_00678070(CPU *); int copy64_native(CPU *c); extern int g_copy64_native;
void __wrap_F_00678070(CPU *c) { if (!(g_copy64_native && copy64_native(c))) __real_F_00678070(c); }
/* Native early exit of the wall-edge test F_004ae5c0 (collide_native.c); other cases run the translation. */
extern void __real_F_004ae5c0(CPU *); int wall_prefilter_native(CPU *c); extern int g_wall_native;
void __wrap_F_004ae5c0(CPU *c) { if (!(g_wall_native && wall_prefilter_native(c))) __real_F_004ae5c0(c); }
/* Native segment / quad test (collide_native.c), timed per A/B window. */
extern void __real_F_004f3300(CPU *); int collide_native(CPU *c); extern int g_collide_native; unsigned long long g_co_ns; unsigned g_co_n;
void __wrap_F_004f3300(CPU *c) {
    struct timespec a = {0, 0}, b = {0, 0}; if (g_lab_timing) clock_gettime(CLOCK_MONOTONIC, &a);
    if (!(g_collide_native && collide_native(c))) __real_F_004f3300(c);
    if (g_lab_timing) clock_gettime(CLOCK_MONOTONIC, &b);
    g_co_n++; g_co_ns += (unsigned long long)((long long)(b.tv_sec - a.tv_sec) * 1000000000ll + (b.tv_nsec - a.tv_nsec));
}
/* Same for the particle draw (its per-particle loop is switched inside unit f040, particle_draw_native.c). */
extern void __real_F_00479e30(CPU *); unsigned long long g_pd_ns; unsigned g_pd_n;
unsigned g_audit_particle_draw_skipped;
static int audit_skip_particle_draw(CPU *c) {
    static int enabled = -1;
    static unsigned skipped;
    if (enabled < 0) enabled = getenv("SH3_AUDIT_PARTICLE_DRAW_OFF") != NULL;
    if (!enabled) return 0;
    ++skipped;
    __atomic_add_fetch(&g_audit_particle_draw_skipped, 1u, __ATOMIC_RELAXED);
    if (skipped <= 5 || !(skipped % 1000)) fprintf(stderr,"[audit-particle-draw] omitted=%u\n",skipped);
    c->esp += 4; /* F_00479e30 returns with cdecl; caller owns arguments. */
    return 1;
}
void __wrap_F_00479e30(CPU *c) {
    if (audit_skip_particle_draw(c)) return;
    struct timespec a = {0, 0}, b = {0, 0}; if (g_lab_timing) clock_gettime(CLOCK_MONOTONIC, &a); __real_F_00479e30(c); if (g_lab_timing) clock_gettime(CLOCK_MONOTONIC, &b);
    g_pd_n++; g_pd_ns += (unsigned long long)((long long)(b.tv_sec - a.tv_sec) * 1000000000ll + (b.tv_nsec - a.tv_nsec));
}
#endif
#ifdef SH3_SHADOW_NATIVE_LAB
#ifdef SH3_SHADOW_PROBE
#error "SH3_SHADOW_NATIVE_LAB and SH3_SHADOW_PROBE both wrap F_0048e140"
#endif
/* Native PC24 shadow-volume builder (shadow_native.c); falls back to the translation when its preconditions fail. */
extern void __real_F_0048e140(CPU *); int shadow_e140_native(CPU *c); extern int g_shadow_native;
unsigned long long g_sn_ns; unsigned g_sn_native, g_sn_translated, g_sn_refused;   /* per A/B window, logged by diag_ab_tick */
static int audit_skip_shadow(CPU *c) {
    static int enabled = -1;
    static unsigned skipped;
    if (enabled < 0) {
        const char *override = getenv("SH3_AUDIT_SHADOW_OFF");
#if defined(__vita__) && defined(SH3_PLAYTEST_PROFILE)
        enabled = override ? strcmp(override, "0") != 0 : 1;
#else
        enabled = override && strcmp(override, "0") != 0;
#endif
        fprintf(stderr, "[audit-shadow] default volume suppression=%d\n", enabled);
    }
    if (!enabled) return 0;
    if (!(skipped++ % 1000))
        fprintf(stderr, "[audit-shadow] omitted=%u\n", skipped);
    c->esp += 4;
    return 1;
}
void __wrap_F_0048e140(CPU *c) {
    if (audit_skip_shadow(c)) return;
    static int logged; struct timespec a = {0, 0}, b = {0, 0}; if (g_lab_timing) clock_gettime(CLOCK_MONOTONIC, &a);
    if (g_shadow_native && shadow_e140_native(c)) {
        if (!logged) { logged=1; fprintf(stderr,"[shadow-native] first native shadow volume built\n"); }
        g_sn_native++;
    } else {
        if (g_shadow_native) g_sn_refused++;
        __real_F_0048e140(c); g_sn_translated++;
    }
    if (g_lab_timing) clock_gettime(CLOCK_MONOTONIC, &b);
    g_sn_ns += (unsigned long long)((long long)(b.tv_sec - a.tv_sec) * 1000000000ll + (b.tv_nsec - a.tv_nsec));
}
#ifdef SH3_FUNCTIME_LAB
#error "SH3_FUNCTIME_LAB wraps F_0048f040 too; measure one or the other"
#endif
extern void __real_F_0048f040(CPU *); int shadow_f040_native(CPU *c);
unsigned long long g_sf_ns; unsigned g_sf_native, g_sf_translated;
void __wrap_F_0048f040(CPU *c) {
    if (audit_skip_shadow(c)) return;
    struct timespec a = {0, 0}, b = {0, 0}; if (g_lab_timing) clock_gettime(CLOCK_MONOTONIC, &a);
    if (g_shadow_native && shadow_f040_native(c)) g_sf_native++;
    else { __real_F_0048f040(c); g_sf_translated++; }
    if (g_lab_timing) clock_gettime(CLOCK_MONOTONIC, &b);
    g_sf_ns += (unsigned long long)((long long)(b.tv_sec - a.tv_sec) * 1000000000ll + (b.tv_nsec - a.tv_nsec));
}
#endif
#ifdef SH3_FUNCTIME_LAB
/* Wall time per 300-frame window spent in four hot guest functions (inclusive). Link with --wrap for each. */
unsigned long long g_ft_ns[4]; unsigned g_ft_n[4];
#define FT_WRAP(k, F) extern void __real_##F(CPU *); void __wrap_##F(CPU *c) { struct timespec a, b; clock_gettime(CLOCK_MONOTONIC, &a);     __real_##F(c); clock_gettime(CLOCK_MONOTONIC, &b); g_ft_n[k]++;     g_ft_ns[k] += (unsigned long long)((long long)(b.tv_sec - a.tv_sec) * 1000000000ll + (b.tv_nsec - a.tv_nsec)); }
FT_WRAP(0, F_0048f040) FT_WRAP(1, F_0047abb0) FT_WRAP(2, F_00479e30) FT_WRAP(3, F_00471420)
#endif
#ifdef SH3_SHADOW_PROBE
/* Diagnostic upper bound, not a feature: skip the CPU shadow-volume builders F_0048e140/F_0048f040 (cdecl, void; the caller
   pops its arguments) so their cost can be measured. SH3_SHADOWPROBE=1 always skips, =ab alternates every 300 frames and logs
   the window boundaries ("[shadowprobe] frame F ms T skip S"). Shadows disappear in skipped windows. Link with
   -Wl,--wrap=F_0048e140 -Wl,--wrap=F_0048f040. */
extern void __real_F_0048e140(CPU *); extern void __real_F_0048f040(CPU *); extern void __real_F_00471420(CPU *);
/* SH3_SHADOWPROBE: 1 = always skip shadows, ab = skip shadows in the "on" A/B windows, abp = also skip the particle/effect
   render callback F_00471420 (cdecl, 1 argument popped by the caller). Windows logged by diag_ab_tick ("[ab] ..."). */
static int shadow_probe_mode(void) {
    static int mode=-1;
    if (mode<0) { const char *e=getenv("SH3_SHADOWPROBE"); mode=!e?0:!strcmp(e,"abp")?3:!strcmp(e,"ab")?2:atoi(e)?1:0; }
    return mode;
}
static int shadow_probe_skip(void) { int m=shadow_probe_mode(); return m==1 || (m>=2 && g_ab_on); }
void __wrap_F_0048e140(CPU *c) { if (shadow_probe_skip()) { c->esp+=4; return; } __real_F_0048e140(c); }
void __wrap_F_0048f040(CPU *c) { if (shadow_probe_skip()) { c->esp+=4; return; } __real_F_0048f040(c); }
void __wrap_F_00471420(CPU *c) { if (shadow_probe_mode()==3 && g_ab_on) { c->esp+=4; return; } __real_F_00471420(c); }
#endif
_Noreturn void rt_fatal(const char *fmt, ...) {
    va_list ap; va_start(ap,fmt); vfprintf(stderr,fmt,ap); va_end(ap); fputc('\n',stderr); exit(36);
}
uint32_t gheap_alloc(uint32_t size) {
    return diag_heap_alloc(diag_guest_heap,size,0);
}
uint32_t gheap_calloc(uint32_t size) {
    uint32_t p=gheap_alloc(size); if (p) memset(g_mem+p,0,size); return p;
}
void gheap_free(uint32_t p) {
    if (p && diag_heap_free(diag_guest_heap,p)) { fprintf(stderr,"[diag] invalid internal heap address\n"); exit(36); }
}
_Noreturn void rt_unimpl(CPU *c, uint32_t pc, const char *what) {
    (void)c; fprintf(stderr, "[diag] unsupported instruction %s at %08x\n", what, pc); exit(32);
}
_Noreturn void rt_bad_jump(CPU *c, uint32_t pc, uint32_t target) {
    (void)c; fprintf(stderr, "[diag] bad jump from %08x to %08x\n", pc, target); exit(33);
}
_Noreturn void rt_div0(CPU *c, uint32_t pc) {
    (void)c; fprintf(stderr, "[diag] division exception at %08x\n", pc); exit(34);
}
jmp_buf *rt_setjmp_slot(CPU *c, uint32_t ret) {
    pthread_mutex_lock(&diag_jump_lock);
    for (DiagJmpSlot *s=diag_jumps; s; s=s->next) if (s->cpu==c && s->ret==ret) { pthread_mutex_unlock(&diag_jump_lock); return &s->jb; }
    DiagJmpSlot *s=calloc(1,sizeof *s);
    if (!s) { fprintf(stderr,"[diag] cannot allocate jump slot\n"); exit(35); }
    s->cpu=c; s->ret=ret; s->next=diag_jumps; diag_jumps=s;
    pthread_mutex_unlock(&diag_jump_lock);
    return &s->jb;
}
void rt_cpuid(uint32_t *a, uint32_t *b, uint32_t *c, uint32_t *d) {
    switch (*a) {
    case 0: *a=1; *b=0x756e6547; *d=0x49656e69; *c=0x6c65746e; break;
    case 1: *a=0x673; *b=0; *c=0; *d=(1u<<0)|(1u<<4)|(1u<<8); break;
    case 0x80000000u: *a=0x80000000u; *b=*c=*d=0; break;
    default: *a=*b=*c=*d=0;
    }
}
uint32_t rt_flags_pack(CPU *c, uint32_t cf, uint32_t pf, uint32_t zf, uint32_t sf, uint32_t of) {
    return cf|2u|(pf<<2)|(zf<<6)|(sf<<7)|(of<<11)|(1u<<9)|(c->df<<10)|c->eflags_ext;
}
double rt_fround(CPU *c, double v) {
    switch ((c->fcw >> 10) & 3) { case 0: return nearbyint(v); case 1: return floor(v); case 2: return ceil(v); default: return trunc(v); }
}
int32_t rt_fist32(CPU *c, double v) { double r=rt_fround(c,v); return r>=-2147483648.0 && r<=2147483647.0 ? (int32_t)r : INT32_MIN; }
int16_t rt_fist16(CPU *c, double v) { double r=rt_fround(c,v); return r>=-32768.0 && r<=32767.0 ? (int16_t)r : INT16_MIN; }
static int64_t diag_fist64_retained(CPU *c, double v) { double r=rt_fround(c,v); return r>=-9223372036854775808.0 && r<9223372036854775808.0 ? (int64_t)r : INT64_MIN; }
int64_t rt_fist64(CPU *c, double v) {
    unsigned mode=(c->fcw>>10)&3u;
    if ((g_diag_perf&131072u) && (mode!=0 || fegetround()==FE_TONEAREST)) return diag_fist_bits(v,mode);
    return diag_fist64_retained(c,v);
}
double rt_f80_load(const uint8_t *p) {
    uint64_t m; uint16_t se; memcpy(&m,p,8); memcpy(&se,p+8,2); int e=se&0x7fff; double v;
    if (e && !(m>>63)) v=NAN; else if (e==0x7fff) v=(m<<1)?NAN:INFINITY; else v=ldexp((double)m,e-16383-63);
    return (se&0x8000)?-v:v;
}
void rt_f80_store(uint8_t *p, double v) {
    uint64_t m=0; uint16_t se=signbit(v)?0x8000:0;
    if (isnan(v)) { se|=0x7fff; m=0xc000000000000000ull; }
    else if (isinf(v)) { se|=0x7fff; m=0x8000000000000000ull; }
    else if (v!=0) { int e; double f=frexp(fabs(v),&e); m=(uint64_t)ldexp(f,64); se|=(uint16_t)(e-1+16383); }
    memcpy(p,&m,8); memcpy(p+8,&se,2);
}
void rt_fsave(CPU *c, uint32_t addr) {
    uint8_t *const mb=g_mem; memset(mb+addr,0,108); W32(addr,c->fcw); W32(addr+4,rt_fnstsw(c));
    for (int i=0;i<8;i++) rt_f80_store(mb+addr+28+10*i,ST(i)); c->fsw=0; c->ftop=0; c->fcw=0x37f;
}
void rt_frstor(CPU *c, uint32_t addr) {
    uint8_t *const mb=g_mem; c->fcw=R32(addr)&0xffff; uint32_t sw=R32(addr+4); c->ftop=(sw>>11)&7; c->fsw=sw&~0x3800u;
    for (int i=0;i<8;i++) ST(i)=rt_f80_load(mb+addr+28+10*i);
}

/* Reject oversized/binary markers instead of silently truncating an option. */
static int diag_read_perf_options(FILE *f, char *list, size_t capacity) {
    if (capacity < 2) return 0;
    size_t got = fread(list, 1, capacity - 1, f);
    list[got] = 0;
    int extra = fgetc(f);
    return extra == EOF && !ferror(f) && !memchr(list, 0, got);
}

#ifdef __vita__
#include "dbgfont.h"
#include <psp2/display.h>
#include <psp2/io/stat.h>
#define SH3_EXE_SIZE 3338240ull
/* Readable startup errors: text (8x16 font enlarged x2) written straight into the display buffer, before vitaGL. */
static void boot_error(const char *const *lines, int n) {
    fprintf(stderr, "[boot] %s\n", lines[0]); fflush(stderr);
    SceUID id = sceKernelAllocMemBlock("boot_msg", SCE_KERNEL_MEMBLOCK_TYPE_USER_CDRAM_RW, 0x240000, NULL);
    void *base = NULL;
    if (id < 0 || sceKernelGetMemBlockBase(id, &base) < 0) return;
    uint32_t *px = base;
    for (int i = 0; i < 960 * 544; i++) px[i] = 0xff301010;
    for (int l = 0; l < n; l++)
        for (int c = 0; lines[l][c] && c < 58; c++) {
            unsigned ch = (unsigned char)lines[l][c];
            if (ch < 32 || ch > 126) continue;
            for (int y = 0; y < 32; y++)
                for (int x = 0; x < 16; x++)
                    if (dbg_font[ch - 32][y / 2] & (0x80 >> (x / 2)))
                        px[(48 + l * 40 + y) * 960 + 16 + c * 16 + x] = l == 0 ? 0xff40c0ff : 0xffffffff;
        }
    SceDisplayFrameBuf fb = {.size = sizeof fb, .base = base, .pitch = 960, .pixelformat = SCE_DISPLAY_PIXELFORMAT_A8B8G8R8, .width = 960, .height = 544};
    sceDisplaySetFrameBuf(&fb, SCE_DISPLAY_SETBUF_NEXTFRAME);
    sceKernelDelayThread(8 * 1000 * 1000);
}
static int boot_stat(const char *path, SceIoStat *st) { return sceIoGetstat(path, st) >= 0; }
static void boot_checks(void) {
    diag_recover_commits();
    SceIoStat st;
    if (!boot_stat("ux0:data/sh3/sh3.exe", &st) || !boot_stat("ux0:data/sh3/data", &st)) {
        static const char *const m[] = {"GAME FILES NOT FOUND", "", "Expected in ux0:data/sh3/", "", "Run sh3prep on your PC with your own copy", "of the game, then copy the sh3 folder", "to ux0:data/ on the Vita (see README)."};
        boot_error(m, 7); sceKernelExitProcess(0);
    }
    if (!boot_stat("ux0:data/sh3/sh3.exe", &st) || (unsigned long long)st.st_size != SH3_EXE_SIZE) {
        static const char *const m[] = {"UNSUPPORTED sh3.exe", "", "The executable in ux0:data/sh3/ is not the", "supported version (3,338,240 bytes).", "See the README for the exact file, then", "prepare your files again with sh3prep."};
        boot_error(m, 6); sceKernelExitProcess(0);
    }
    if (!boot_stat("ux0:data/sh3/movie/op.mp4", &st) && !boot_stat("ux0:data/sh3/movie/op.mpg", &st)) {
        static const char *const m[] = {"MOVIES NOT PREPARED", "", "ux0:data/sh3/movie/ is missing or incomplete.", "Run sh3prep again and copy the whole sh3 folder."};
        boot_error(m, 4); sceKernelExitProcess(0);
    }
    if (!boot_stat("ur0:data/libshacccg.suprx", &st) && !boot_stat("ur0:/data/external/libshacccg.suprx", &st) && !boot_stat("vs0:sys/external/libshacccg.suprx", &st)) {
        static const char *const m[] = {"SHADER COMPILER MISSING", "", "libshacccg.suprx was not found.", "Install it on your own console (ShaRKF00D),", "then restart the game (see README)."};
        boot_error(m, 5);   /* we try anyway: the message stays displayed long enough to read it */
    }
}
#endif
int main(int argc, char **argv) {
#ifdef __vita__
    (void)argc; (void)argv;
    sceIoMkdir("ux0:data/sh3",0777);
    sceIoMkdir("ux0:data/sh3/diag",0777);
    sceIoMkdir("ux0:data/sh3/movie",0777);
    sceIoMkdir("ux0:data/sh3/cache",0777);
    sceIoMkdir("ux0:data/sh3/savedata",0777);
    /* Test build logs: keep the current session and the 10 previous ones, diag-1.log (latest) .. diag-10.log, so a tester who
       met several problems can send them all (as sh4vita). On a rename error append to diag.log rather than truncate it. */
    int rotated=1;
    { char a[64],b[64]; SceIoStat st; remove("ux0:data/sh3/diag/diag-10.log");
      for (int i=9;i>=0 && rotated;i--) {
          if (i) snprintf(a,sizeof a,"ux0:data/sh3/diag/diag-%d.log",i); else snprintf(a,sizeof a,"ux0:data/sh3/diag/diag.log");
          snprintf(b,sizeof b,"ux0:data/sh3/diag/diag-%d.log",i+1);
          if (sceIoGetstat(a,&st)>=0 && rename(a,b)!=0) rotated=0;
      } }
    freopen("ux0:data/sh3/diag/diag.log",rotated?"w":"a",stderr);
    fprintf(stderr,"[session] sh3vita 1.0.1 build " __DATE__ " " __TIME__ "; previous sessions in diag-1.log (latest) .. diag-10.log%s\n",rotated?"":" (rotation failed: appended)");
    boot_checks();
    extern int diag_tls_verify(void);
    if (diag_tls_verify()) return 5;
    /* Diagnostic builds favor the last recorded operation over throughput.
       A process killed by a Vita fault otherwise loses buffered log lines. */
    FILE *log_buffered=fopen("ux0:data/sh3/diag/logbuffer","rb");
    if (log_buffered) {
        static char log_buffer[65536];
        fclose(log_buffered);
        if (unlink("ux0:data/sh3/diag/logbuffer")) return 5;
        if (setvbuf(stderr,log_buffer,_IOFBF,sizeof log_buffer)) return 5;
        fprintf(stderr,"[diag] experimental buffered log enabled; flush at present-rate report and capture\n");
    } else {
        FILE *perf=fopen("ux0:data/sh3/diag/perf","rb");
        int perf_bad=0;
        if (perf) {
            char list[1024]={0}; int complete=diag_read_perf_options(perf,list,sizeof list); fclose(perf);
            if (!complete) {
                fprintf(stderr,"[diag] perf marker oversized or binary (maximum %u bytes); preserved for inspection\n",(unsigned)sizeof list-1);
                return 5;
            }
            if (unlink("ux0:data/sh3/diag/perf")) return 5;
            static const struct { const char *name; unsigned bit; } opts[]={{"fastcom",PERF_FASTCOM},{"lookup",PERF_LOOKUP},
                {"logasync",PERF_LOGASYNC},{"novsync",PERF_NOVSYNC},{"filecache",PERF_FILECACHE},{"audiocache",PERF_AUDIOCACHE},
                {"prepthread",PERF_PREPTHREAD},{"latepace",PERF_LATEPACE},{"matrix",PERF_MATRIX},{"widecache",PERF_WIDECACHE},{"geomo2",PERF_GEOMO2},{"ftol",PERF_FTOL},{"passaudit",PERF_PASSAUDIT},{"presentclear",PERF_PRESENTCLEAR},{"gxmscenes",PERF_GXMSCENES},{"paceprobe",PERF_PACEPROBE},{"fistbits",PERF_FISTBITS},{"fxhalf",PERF_FXHALF},{"floorinline",PERF_FLOORINLINE},{"glstatecache",PERF_GLSTATECACHE},{"lensasync",PERF_LENSASYNC},{"keepcolor",PERF_KEEPCOLOR},{"fxquarter",PERF_FXQUARTER}};
            g_diag_perf=0;
            for (char *t=strtok(list,", \r\n");t;t=strtok(NULL,", \r\n")) {
                const unsigned nopts=sizeof opts/sizeof opts[0];
                unsigned k=0; while (k<nopts && strcmp(t,opts[k].name)) k++;
                if (k<nopts) g_diag_perf|=opts[k].bit; else if (strcmp(t,"none")) perf_bad=1;
            }
        }
        if (g_diag_perf&PERF_LOGASYNC) {
            static char async_buffer[65536]; pthread_t flusher;
            if (setvbuf(stderr,async_buffer,_IOFBF,sizeof async_buffer) || pthread_create(&flusher,NULL,diag_log_flusher,NULL)) return 5;
        } else setvbuf(stderr,NULL,_IONBF,0);
        if (perf_bad) { fprintf(stderr,"[diag] unknown perf option\n"); return 5; }
        fprintf(stderr,"[diag] perf options=%02x (%s): fastcom=%d lookup=%d logasync=%d novsync=%d filecache=%d audiocache=%d prepthread=%d latepace=%d\n",
                g_diag_perf,perf?"marker":"default",!!(g_diag_perf&PERF_FASTCOM),!!(g_diag_perf&PERF_LOOKUP),!!(g_diag_perf&PERF_LOGASYNC),
                !!(g_diag_perf&PERF_NOVSYNC),!!(g_diag_perf&PERF_FILECACHE),!!(g_diag_perf&PERF_AUDIOCACHE),!!(g_diag_perf&PERF_PREPTHREAD),!!(g_diag_perf&PERF_LATEPACE));
    }
#ifndef SH3_MATRIX_LAB
    if (g_diag_perf&PERF_MATRIX) { fprintf(stderr,"[diag] matrix requested but SH3_MATRIX_LAB is not compiled\n"); return 5; }
#endif
#ifndef SH3_NUMERIC_LAB
    if (g_diag_perf&(PERF_FTOL|PERF_GEOMO2)) { fprintf(stderr,"[diag] numeric experiment requested but SH3_NUMERIC_LAB is not compiled\n"); return 5; }
#endif
    fprintf(stderr,"[diag] extra perf options: widecache=%u matrix=%u ftol=%u geomo2=%u passaudit=%u presentclear=%u gxmscenes=%u paceprobe=%u\n",
            !!(g_diag_perf&PERF_WIDECACHE),!!(g_diag_perf&PERF_MATRIX),!!(g_diag_perf&PERF_FTOL),!!(g_diag_perf&PERF_GEOMO2),
            !!(g_diag_perf&PERF_PASSAUDIT),!!(g_diag_perf&PERF_PRESENTCLEAR),!!(g_diag_perf&PERF_GXMSCENES),!!(g_diag_perf&PERF_PACEPROBE));
#ifndef SH3_FLOOR_INLINE_LAB
    if (g_diag_perf&PERF_FLOORINLINE) { fprintf(stderr,"[diag] floorinline requested but not compiled\n"); return 5; }
#endif
    fprintf(stderr,"[diag] exact CRT floor helper inlining=%u (isolated lab)\n",!!(g_diag_perf&PERF_FLOORINLINE));
    fprintf(stderr,"[diag] exact GL state cache=%u (opt-in)\n",!!(g_diag_perf&PERF_GLSTATECACHE));
    fprintf(stderr,"[diag] exact integer-rounding experiment fistbits=%u (native nearest checked per call)\n",!!(g_diag_perf&PERF_FISTBITS));
    fprintf(stderr,"[diag] half-resolution additive effect experiment fxhalf=%u (visual concession, selected by active perf options)\n",!!(g_diag_perf&PERF_FXHALF));
    if (g_diag_perf&PERF_PASSAUDIT) {
        setenv("SH4_TRACE_FRAME","2400",1);setenv("SH4_TRACE_COUNT","2",1);
        fprintf(stderr,"[diag] bounded draw/pass trace frames2400..2401; no per-draw readback, diagnostic only\n");
    }
    SceKernelSystemSwVersion reported_fw={.size=sizeof reported_fw};
    int fw_status=sceKernelGetSystemSwVersion(&reported_fw);
    fprintf(stderr,"[diag] reported firmware status=%x version=%08x text=%.27s (SDK API may return spoofed version)\n",
            fw_status,reported_fw.version,reported_fw.versionString);
    FILE *com_profile=fopen("ux0:data/sh3/diag/profilelock","rb");
    if (com_profile) {
        fclose(com_profile);
        if (unlink("ux0:data/sh3/diag/profilelock")) return 5;
        diag_com_profile=1;
        fprintf(stderr,"[diag] sampled outer COM lock timing enabled (1/128)\n");
    }
    FILE *direct_io=fopen("ux0:data/sh3/diag/directio","rb");
    if (direct_io) {
        fclose(direct_io);
        if (unlink("ux0:data/sh3/diag/directio")) return 5;
        diag_direct_io=1; fprintf(stderr,"[diag] experimental direct reads of game data enabled\n");
    }
    FILE *pause_trace=fopen("ux0:data/sh3/diag/pausetrace","rb");
    if (pause_trace) {
        unsigned from=0,to=0; int n=fscanf(pause_trace,"%u,%u",&from,&to);
        fclose(pause_trace);
        if (unlink("ux0:data/sh3/diag/pausetrace")) return 5;
        if (n!=2 || from>=to || to>600000u) { fprintf(stderr,"[diag] invalid pause trace window\n"); return 5; }
        diag_pause_trace_from=from; diag_pause_trace_to=to;
        fprintf(stderr,"[diag] pause trace window %u..%u ms\n",from,to);
    }
    FILE *pause_readback=fopen("ux0:data/sh3/diag/pausereadback","rb");
    if (pause_readback) {
        fclose(pause_readback);
        if (unlink("ux0:data/sh3/diag/pausereadback")) return 5;
        setenv("SH4_CPREAD","1",1);
        fprintf(stderr,"[diag] experimental GPU-to-system copies enabled\n");
    }
    FILE *io_profile=fopen("ux0:data/sh3/diag/profileio","rb");
    if (io_profile) {
        fclose(io_profile);
        if (unlink("ux0:data/sh3/diag/profileio")) return 5;
        diag_io_profile=1; fprintf(stderr,"[diag] game data read timing enabled\n");
    }
    FILE *bench_save=fopen("ux0:data/sh3/diag/benchsave","rb");
    if(bench_save) {
        int mode=fgetc(bench_save),tail=fgetc(bench_save);
        int valid=mode=='1' && (tail==EOF || (tail=='\n' && fgetc(bench_save)==EOF));
        fclose(bench_save);struct stat fixture;
        if(!valid || stat("ux0:data/sh3/diag/save-fixture/data.001",&fixture) ||
           !S_ISREG(fixture.st_mode) || fixture.st_size!=9952) {
            fprintf(stderr,"[bench-save] invalid isolated Save1 fixture\n");return 5;
        }
        if(unlink("ux0:data/sh3/diag/benchsave"))return 5;
        diag_save_fixture=1;
        fprintf(stderr,"[bench-save] isolated save-fixture active; real savedata excluded\n");
    }
    /* Earlier diagnostics can leave an empty, uninitialized settings artifact. */
    struct stat old_settings;
    if (!stat(DIAG_SAVE_WRITE_FILE("disp.ini"),&old_settings) && S_ISREG(old_settings.st_mode) && old_settings.st_size==0) {
        if (unlink(DIAG_SAVE_WRITE_FILE("disp.ini"))) { fprintf(stderr,"[diag] cannot discard empty diagnostic settings\n"); return 5; }
        fprintf(stderr,"[diag] discarded empty diagnostic settings artifact\n");
    }
    setenv("SH3_DIAG_THREADS","1",1);
    setenv("SH3_DIAG_ASSUME_CUBEMAP","1",1);
    setenv("SH3_DIAG_NOPAD","1",1);
    if (g_diag_perf&PERF_AUDIOCACHE) setenv("SH3_DIAG_AUDIO_LOCK_CACHE","1",1);
    FILE *audio_detail=fopen("ux0:data/sh3/diag/audiodetail","rb");
    if (audio_detail) {
        fclose(audio_detail);
        if (unlink("ux0:data/sh3/diag/audiodetail")) return 5;
        setenv("SH3_DIAG_AUDIO_DETAIL","1",1);
        fprintf(stderr,"[diag] audio buffer detail enabled for this process\n");
    }
    FILE *audio_cache=fopen("ux0:data/sh3/diag/audiocache","rb");
    if (audio_cache) {
        fclose(audio_cache);
        if (unlink("ux0:data/sh3/diag/audiocache")) return 5;
        setenv("SH3_DIAG_AUDIO_LOCK_CACHE","1",1);
        fprintf(stderr,"[diag] experimental DirectSound staging cache enabled; 16 KB cap per buffer\n");
    }
    /* Temporary bounds instrumentation; an isolated one-shot marker permits
       paired performance diagnostics while keeping the default guarded. */
    FILE *no_guard=fopen("ux0:data/sh3/diag/noguard","rb");
    if (no_guard) {
        fclose(no_guard);
        if (unlink("ux0:data/sh3/diag/noguard")) { fprintf(stderr,"[diag] cannot consume noguard trigger\n"); return 5; }
        fprintf(stderr,"[diag] heap guards disabled for this test\n");
    } else setenv("SH3_DIAG_HEAPGUARD","1",1);
    FILE *profile=fopen("ux0:data/sh3/diag/profile","rb");
    if (profile) {
        fclose(profile);
        if (unlink("ux0:data/sh3/diag/profile")) { fprintf(stderr,"[diag] cannot consume profile trigger\n"); return 5; }
        setenv("SH4_STATS","1",1);
        fprintf(stderr,"[diag] SH4-derived performance counters enabled for this process\n");
    }
    /* The draw-phase counters are separately opt-in: reading a timer on each
       draw affects performance, so never enable them for ordinary runs. */
    { FILE *pk=fopen("ux0:data/sh3/diag/prepkb","rb"); if (pk) { char v[16]={0}; if (fgets(v,sizeof v,pk)) { v[strcspn(v,"\r\n")]=0; setenv("SH4_PREP_KB",v,1); fprintf(stderr,"[diag] preparation ring %s KB requested\n",v); } fclose(pk); unlink("ux0:data/sh3/diag/prepkb"); } }
    { FILE *ld=fopen("ux0:data/sh3/diag/lazydepth","rb"); if (ld) { fclose(ld); unlink("ux0:data/sh3/diag/lazydepth"); setenv("SH3_LAZYDEPTH","1",1); fprintf(stderr,"[diag] lazy depth attachment enabled\n"); } }
    { FILE *sp=fopen("ux0:data/sh3/diag/shadowprobe","rb"); if (sp) { char v[16]={0}; if (fgets(v,sizeof v,sp)) { v[strcspn(v,"\r\n")]=0; setenv("SH3_SHADOWPROBE",v,1); fprintf(stderr,"[diag] shadow volume probe %s requested (requires SH3_SHADOW_PROBE build)\n",v); } fclose(sp); unlink("ux0:data/sh3/diag/shadowprobe"); } }
    { FILE *te=fopen("ux0:data/sh3/diag/texeager","rb"); if (te) { char v[8]={0}; if (!fgets(v,sizeof v,te)) v[0]=0; fclose(te); unlink("ux0:data/sh3/diag/texeager"); setenv("SH3_TEX_EAGER",!strncmp(v,"ab",2)?"ab":"1",1); fprintf(stderr,"[diag] ordered texture uploads requested (%s)\n",!strncmp(v,"ab",2)?"alternating A/B":"on"); } }
#ifdef SH3_PARTICLE_NATIVE_LAB
    { FILE *pn=fopen("ux0:data/sh3/diag/particlenative","rb"); if (pn) { char v[8]={0}; if (!fgets(v,sizeof v,pn)) v[0]=0; fclose(pn); unlink("ux0:data/sh3/diag/particlenative"); setenv("SH3_PARTICLE_NATIVE",!strncmp(v,"ab",2)?"ab":"1",1); fprintf(stderr,"[diag] native particle grid requested (%s)\n",!strncmp(v,"ab",2)?"alternating A/B":"on"); } }
    { FILE *pn=fopen("ux0:data/sh3/diag/particledraw","rb"); if (pn) { char v[8]={0}; if (!fgets(v,sizeof v,pn)) v[0]=0; fclose(pn); unlink("ux0:data/sh3/diag/particledraw"); setenv("SH3_PARTICLE_DRAW",!strncmp(v,"ab",2)?"ab":"1",1); fprintf(stderr,"[diag] native particle draw loop requested (%s)\n",!strncmp(v,"ab",2)?"alternating A/B":"on"); } }
    { FILE *pn=fopen("ux0:data/sh3/diag/qsortnative","rb"); if (pn) { char v[8]={0}; if (!fgets(v,sizeof v,pn)) v[0]=0; fclose(pn); unlink("ux0:data/sh3/diag/qsortnative"); setenv("SH3_QSORT_NATIVE",!strncmp(v,"ab",2)?"ab":"1",1); fprintf(stderr,"[diag] native qsort requested (%s)\n",!strncmp(v,"ab",2)?"alternating A/B":"on"); } }
    { FILE *pn=fopen("ux0:data/sh3/diag/collidenative","rb"); if (pn) { char v[8]={0}; if (!fgets(v,sizeof v,pn)) v[0]=0; fclose(pn); unlink("ux0:data/sh3/diag/collidenative"); setenv("SH3_COLLIDE_NATIVE",!strncmp(v,"ab",2)?"ab":"1",1); fprintf(stderr,"[diag] native collision test requested (%s)\n",!strncmp(v,"ab",2)?"alternating A/B":"on"); } }
    { FILE *pn=fopen("ux0:data/sh3/diag/texfillnative","rb"); if (pn) { char v[8]={0}; if (!fgets(v,sizeof v,pn)) v[0]=0; fclose(pn); unlink("ux0:data/sh3/diag/texfillnative"); setenv("SH3_TEXFILL_NATIVE",!strncmp(v,"ab",2)?"ab":"1",1); fprintf(stderr,"[diag] native texture fill requested (%s)\n",!strncmp(v,"ab",2)?"alternating A/B":"on"); } }
    { FILE *pn=fopen("ux0:data/sh3/diag/prandnative","rb"); if (pn) { char v[8]={0}; if (!fgets(v,sizeof v,pn)) v[0]=0; fclose(pn); unlink("ux0:data/sh3/diag/prandnative"); setenv("SH3_PRAND_NATIVE",!strncmp(v,"ab",2)?"ab":"1",1); fprintf(stderr,"[diag] native particle randomizer requested (%s)\n",!strncmp(v,"ab",2)?"alternating A/B":"on"); } }
    { FILE *pn=fopen("ux0:data/sh3/diag/blurnative","rb"); if (pn) { char v[8]={0}; if (!fgets(v,sizeof v,pn)) v[0]=0; fclose(pn); unlink("ux0:data/sh3/diag/blurnative"); setenv("SH3_BLUR_NATIVE",!strncmp(v,"ab",2)?"ab":"1",1); fprintf(stderr,"[diag] native image smoothing requested (%s)\n",!strncmp(v,"ab",2)?"alternating A/B":"on"); } }
    { FILE *pn=fopen("ux0:data/sh3/diag/pplanes","rb"); if (pn) { char v[8]={0}; if (!fgets(v,sizeof v,pn)) v[0]=0; fclose(pn); unlink("ux0:data/sh3/diag/pplanes"); setenv("SH3_PPLANES_NATIVE",!strncmp(v,"ab",2)?"ab":"1",1); fprintf(stderr,"[diag] native particle plane collision requested (%s)\n",!strncmp(v,"ab",2)?"alternating A/B":"on"); } }
    { FILE *pn=fopen("ux0:data/sh3/diag/pbox","rb"); if (pn) { char v[8]={0}; if (!fgets(v,sizeof v,pn)) v[0]=0; fclose(pn); unlink("ux0:data/sh3/diag/pbox"); setenv("SH3_PBOX_NATIVE",!strncmp(v,"ab",2)?"ab":"1",1); fprintf(stderr,"[diag] native particle box wrap requested (%s)\n",!strncmp(v,"ab",2)?"alternating A/B":"on"); } }
    { FILE *pn=fopen("ux0:data/sh3/diag/pcyl","rb"); if (pn) { char v[8]={0}; if (!fgets(v,sizeof v,pn)) v[0]=0; fclose(pn); unlink("ux0:data/sh3/diag/pcyl"); setenv("SH3_PCYL_NATIVE",!strncmp(v,"ab",2)?"ab":"1",1); fprintf(stderr,"[diag] native particle cylinder collision requested (%s)\n",!strncmp(v,"ab",2)?"alternating A/B":"on"); } }
    { FILE *pn=fopen("ux0:data/sh3/diag/pupdate","rb"); if (pn) { char v[8]={0}; if (!fgets(v,sizeof v,pn)) v[0]=0; fclose(pn); unlink("ux0:data/sh3/diag/pupdate"); setenv("SH3_PUPDATE_NATIVE",!strncmp(v,"ab",2)?"ab":"1",1); fprintf(stderr,"[diag] native particle update body requested (%s)\n",!strncmp(v,"ab",2)?"alternating A/B":"on"); } }
    { FILE *pn=fopen("ux0:data/sh3/diag/wallnative","rb"); if (pn) { char v[8]={0}; if (!fgets(v,sizeof v,pn)) v[0]=0; fclose(pn); unlink("ux0:data/sh3/diag/wallnative"); setenv("SH3_WALL_NATIVE",!strncmp(v,"ab",2)?"ab":"1",1); fprintf(stderr,"[diag] native wall test early exit requested (%s)\n",!strncmp(v,"ab",2)?"alternating A/B":"on"); } }
    { FILE *pn=fopen("ux0:data/sh3/diag/copy64","rb"); if (pn) { char v[8]={0}; if (!fgets(v,sizeof v,pn)) v[0]=0; fclose(pn); unlink("ux0:data/sh3/diag/copy64"); setenv("SH3_COPY64_NATIVE",!strncmp(v,"ab",2)?"ab":"1",1); fprintf(stderr,"[diag] native 64-byte copy requested (%s)\n",!strncmp(v,"ab",2)?"alternating A/B":"on"); } }
    { FILE *pn=fopen("ux0:data/sh3/diag/allnative","rb"); if (pn) { char v[8]={0}; if (!fgets(v,sizeof v,pn)) v[0]=0; fclose(pn); unlink("ux0:data/sh3/diag/allnative"); setenv("SH3_ALL_NATIVE",!strncmp(v,"ab",2)?"ab":"1",1); fprintf(stderr,"[diag] all native kernels requested (%s)\n",!strncmp(v,"ab",2)?"alternating A/B":"on"); } }
    { FILE *pn=fopen("ux0:data/sh3/diag/labtiming","rb"); if (pn) { char v[8]={0}; if (!fgets(v,sizeof v,pn)) v[0]=0; fclose(pn); unlink("ux0:data/sh3/diag/labtiming"); setenv("SH3_LAB_TIMING",!strncmp(v,"ab",2)?"ab":"1",1); fprintf(stderr,"[diag] per-call lab timing requested (%s)\n",!strncmp(v,"ab",2)?"alternating A/B":"on"); } }
#endif
#ifdef SH3_SHADOW_NATIVE_LAB
    { FILE *sn=fopen("ux0:data/sh3/diag/shadownative","rb"); if (sn) { char v[8]={0}; if (!fgets(v,sizeof v,sn)) v[0]=0; fclose(sn); unlink("ux0:data/sh3/diag/shadownative"); setenv("SH3_SHADOW_NATIVE",!strncmp(v,"ab",2)?"ab":"1",1); fprintf(stderr,"[diag] native shadow volumes requested (%s)\n",!strncmp(v,"ab",2)?"alternating A/B":"on"); } }
    { FILE *ss=fopen("ux0:data/sh3/diag/shadowstrip","rb"); if (ss) { char v[8]={0}; if (!fgets(v,sizeof v,ss)) v[0]=0; fclose(ss); unlink("ux0:data/sh3/diag/shadowstrip"); setenv("SH3_SHADOW_STRIP",!strncmp(v,"ab",2)?"ab":"1",1); fprintf(stderr,"[diag] native strip shadow volumes requested (%s)\n",!strncmp(v,"ab",2)?"alternating A/B":"on"); } }
#endif
    { FILE *nc=fopen("ux0:data/sh3/diag/nativeclip","rb"); if (nc) { char v[8]={0}; if (!fgets(v,sizeof v,nc)) v[0]=0; fclose(nc); unlink("ux0:data/sh3/diag/nativeclip"); setenv("SH3_NATIVE_CLIP",!strncmp(v,"ab",2)?"ab":"1",1); fprintf(stderr,"[diag] finite native clipping requested (%s, requires lab build)\n",!strncmp(v,"ab",2)?"alternating A/B":"on"); } }
#ifdef SH3_PC24_VEC3_LAB
    { FILE *f=fopen("ux0:data/sh3/diag/pc24vec","rb");if(f){
        char v[8]={0};size_t n=fread(v,1,sizeof v-1,f);fclose(f);v[strcspn(v,"\r\n")]=0;
        if(!n || (strcmp(v,"0") && strcmp(v,"1"))){fprintf(stderr,"[pc24-vec3] invalid marker\n");return 5;}
        if(unlink("ux0:data/sh3/diag/pc24vec"))return 5;
        if(v[0]=='0')setenv("SH3_PC24_VEC3_OFF","1",1);else unsetenv("SH3_PC24_VEC3_OFF");
    } }
#endif
    { FILE *rtt=fopen("ux0:data/sh3/diag/rttrace","rb"); if (rtt) { fclose(rtt); unlink("ux0:data/sh3/diag/rttrace"); setenv("SH3_RTTRACE","1",1); fprintf(stderr,"[diag] render-target trace enabled\n"); } }
    { FILE *tf=fopen("ux0:data/sh3/diag/texfree","rb"); if (tf) { fclose(tf); unlink("ux0:data/sh3/diag/texfree"); setenv("SH3_TEXFREE","1",1); fprintf(stderr,"[diag] texture copies released after upload (texfree)\n"); } }
    if (unlink("ux0:data/sh3/diag/privatedepth")==0) {
        setenv("SH3_PRIVATE_DEPTH_DISCARD","1",1);
        fprintf(stderr,"[diag] private framebuffer depth discard requested\n");
    }
    if (unlink("ux0:data/sh3/diag/vcursortest")==0)
        setenv("SH3_DIAG_VCURSOR_TEST","1",1);
    if (unlink("ux0:data/sh3/diag/vcursorsweep")==0) setenv("SH3_DIAG_VCURSOR_SWEEP","1",1);
    { FILE *vp=fopen("ux0:data/sh3/diag/vcursorpos","rb");   /* test: "x y" in the game's 512 x 512 pointer space */
      if (vp) { char b[64]={0}; size_t n=fread(b,1,63,vp); (void)n; fclose(vp); unlink("ux0:data/sh3/diag/vcursorpos"); setenv("SH3_DIAG_VCURSOR_POS",b,1); } }
    if (unlink("ux0:data/sh3/diag/probefx")==0)
        setenv("SH3_PROBE_FX","1",1);
    if (unlink("ux0:data/sh3/diag/probefxoff")==0)
        setenv("SH3_PROBE_FX","off",1);
    if (unlink("ux0:data/sh3/diag/probefxwide")==0)
        setenv("SH3_PROBE_FX","wide",1);
    if (unlink("ux0:data/sh3/diag/probefxpair")==0)
        setenv("SH3_PROBE_FX","pair",1);
    if (unlink("ux0:data/sh3/diag/probefxone")==0)
        setenv("SH3_PROBE_FX","one",1);
    if (unlink("ux0:data/sh3/diag/hospitalsharp")==0)
        setenv("SH3_PROBE_FX","hospital",1);
    if (unlink("ux0:data/sh3/diag/auditshadowoff")==0)
        setenv("SH3_AUDIT_SHADOW_OFF","1",1);
    if (unlink("ux0:data/sh3/diag/auditshadowon")==0)
        setenv("SH3_AUDIT_SHADOW_OFF","0",1);
    if (unlink("ux0:data/sh3/diag/auditfogoff")==0)
        setenv("SH3_AUDIT_NOFOG","1",1);
    if (unlink("ux0:data/sh3/diag/auditparticlesoff")==0)
        setenv("SH3_AUDIT_PARTICLE_DRAW_OFF","1",1);
    if (unlink("ux0:data/sh3/diag/probegeo")==0)
        setenv("SH3_PROBE_GEO","1",1);
    FILE *profile_draw=fopen("ux0:data/sh3/diag/profiledraw","rb");
    if (profile_draw) {
        fclose(profile_draw);
        if (unlink("ux0:data/sh3/diag/profiledraw")) { fprintf(stderr,"[diag] cannot consume profiledraw trigger\n"); return 5; }
        setenv("SH4_STATS","1",1);
        setenv("SH4_PH","1",1);
        fprintf(stderr,"[diag] SH4 draw-phase timers enabled for this process\n");
    }
    { FILE *m=fopen("ux0:data/sh3/diag/glstatecacheab","rb"); if (m) {
        fclose(m); if (unlink("ux0:data/sh3/diag/glstatecacheab")) return 5;
        extern int g_glstatecache_ab; g_glstatecache_ab=1;
        fprintf(stderr,"[diag] GL state cache A/B enabled (native kernel switches independent)\n");
    } }
#ifdef SH3_PLAYTEST_PERFDEFAULT_LAB
    /* Isolated candidate: reproduce the measured benchmark policy by default.
       An explicit one-shot override keeps a same-binary control available. */
    if (unlink("ux0:data/sh3/diag/perfdefaultsoff") == 0)
        setenv("SH3_PERFDEFAULT_OFF", "1", 1);
    if (!getenv("SH3_PERFDEFAULT_OFF")) {
        g_pace60_park=1; g_pace60_clamp=1; g_pace60_all_rooms=1;
        setenv("SH3_ALL_NATIVE", "1", 0);
        setenv("SH3_TEX_EAGER", "1", 0);
        fprintf(stderr,"[perf-default-lab] native, eager textures and bounded room pacing enabled\n");
    }
#endif
    if (unlink("ux0:data/sh3/diag/probenoread") == 0) { setenv("SH3_PROBE_NOREAD","1",1); fprintf(stderr,"[diag] probe emulation without GPU readback\n"); }
    if (unlink("ux0:data/sh3/diag/pace60force") == 0) { g_pace60_even=0; fprintf(stderr,"[pace] forced one-tick target restored (A/B)\n"); }
    {   /* bench override of the frame-rate setting: marker fpsmode / env SH3_FPS_MODE = 0 (60), 1 (30), 2 (unlimited) */
        extern int g_fps_mode_override; int v=-1;
        FILE *m=fopen("ux0:data/sh3/diag/fpsmode","rb");
        if (m) { v=fgetc(m); fclose(m); if (unlink("ux0:data/sh3/diag/fpsmode")) return 5; }
        else if (getenv("SH3_FPS_MODE")) v=getenv("SH3_FPS_MODE")[0];
        if (v>='0' && v<='2') { g_fps_mode_override=v-'0'; fprintf(stderr,"[fps] frame-rate setting forced to %d (0=60 1=30 2=unlimited)\n",g_fps_mode_override); }
    }
    { FILE *m=fopen("ux0:data/sh3/diag/pace60park","rb"); if (m) {
#ifndef SH3_FRAME_POLICY_LAB
        fclose(m);fprintf(stderr,"[diag] pace60park requires SH3_FRAME_POLICY_LAB (simulation coupling)\n");return 5;
#else
        char value[8]={0};int complete=diag_read_perf_options(m,value,sizeof value);fclose(m);
        value[strcspn(value,"\r\n")]=0;
        if (!complete || (strcmp(value,"1") && strcmp(value,"ab"))) {
            fprintf(stderr,"[diag] invalid pace60park marker (1 or ab)\n");return 5;
        }
        if (unlink("ux0:data/sh3/diag/pace60park")) return 5;
        g_ab_pace60_park=!strcmp(value,"ab");g_pace60_park=!g_ab_pace60_park;
        fprintf(stderr,"[diag] park-only 60 Hz pacing lab enabled (%s); clock counters retained\n",value);
#endif
    } }
    { FILE *m=fopen("ux0:data/sh3/diag/pace60clamp","rb"); if (m) {
        char value[8]={0};int complete=diag_read_perf_options(m,value,sizeof value);fclose(m);
        value[strcspn(value,"\r\n")]=0;
        if (!complete || strcmp(value,"1") || (!g_pace60_park && !g_ab_pace60_park)) {
            fprintf(stderr,"[diag] pace60clamp requires pace60park and value 1\n");return 5;
        }
        if (unlink("ux0:data/sh3/diag/pace60clamp")) return 5;
        g_pace60_clamp=1;
        fprintf(stderr,"[diag] pacing lab retains original adaptive maximum of 3 simulation ticks\n");
    } }
    { FILE *m=fopen("ux0:data/sh3/diag/pace60rooms","rb"); if (m) {
        char value[8]={0};int complete=diag_read_perf_options(m,value,sizeof value);fclose(m);
        value[strcspn(value,"\r\n")]=0;
        if (!complete || strcmp(value,"1") || !g_pace60_clamp) {
            fprintf(stderr,"[diag] pace60rooms requires pace60park, pace60clamp=1 and value 1\n");return 5;
        }
        if (unlink("ux0:data/sh3/diag/pace60rooms")) return 5;
        g_pace60_all_rooms=1;
        fprintf(stderr,"[diag] pacing lab extended to gameplay state 5 in stages 1..39\n");
    } }
    /* Optional paired test of the same clock requests used by sh4vita. */
    FILE *clock_test=fopen("ux0:data/sh3/diag/clock444","rb");
#ifdef SH3_PLAYTEST_PROFILE
    int playtest_clock=1;
#else
    int playtest_clock=0;
#endif
    if (clock_test || playtest_clock) {
        if (clock_test) {
            fclose(clock_test);
            if (unlink("ux0:data/sh3/diag/clock444")) { fprintf(stderr,"[diag] cannot consume clock444 trigger\n"); return 5; }
        }
        int arm=scePowerSetArmClockFrequency(444), bus=scePowerSetBusClockFrequency(222);
        int gpu=scePowerSetGpuClockFrequency(222), xbar=scePowerSetGpuXbarClockFrequency(166);
        fprintf(stderr,"[diag] clock requests arm=%d bus=%d gpu=%d xbar=%d; observed MHz %d/%d/%d/%d\n",
                arm,bus,gpu,xbar,scePowerGetArmClockFrequency(),scePowerGetBusClockFrequency(),
                scePowerGetGpuClockFrequency(),scePowerGetGpuXbarClockFrequency());
    }
    FILE *profile_cpu=fopen("ux0:data/sh3/diag/profilecpu","rb");
    if (profile_cpu) {
        fclose(profile_cpu);
        if (unlink("ux0:data/sh3/diag/profilecpu")) { fprintf(stderr,"[diag] cannot consume profilecpu trigger\n"); return 5; }
        setenv("SH4_STATS","1",1);
        setenv("SH4_CPU_TIME","1",1);
        fprintf(stderr,"[diag] SH4 per-thread CPU profiling enabled for this process\n");
    }
    rt_cpu_profile_init();
    FILE *profile_worker=fopen("ux0:data/sh3/diag/profileworker","rb");
    if (profile_worker) {
        fclose(profile_worker);
        if (unlink("ux0:data/sh3/diag/profileworker")) { fprintf(stderr,"[diag] cannot consume profileworker trigger\n"); return 5; }
        setenv("SH3_DIAG_WORKER_TIMING","1",1);
        setenv("SH3_DIAG_WORKER_PARTS","1",1);
        fprintf(stderr,"[diag] worker callback timing enabled for this process\n");
    }
    FILE *readback_trace=fopen("ux0:data/sh3/diag/readbacktrace","rb");
    if (readback_trace) {
        fclose(readback_trace);
        if (unlink("ux0:data/sh3/diag/readbacktrace")) { fprintf(stderr,"[diag] cannot consume readbacktrace trigger\n"); return 5; }
        setenv("SH3_DIAG_READBACK_TRACE","1",1);
        fprintf(stderr,"[diag] 64x64 readback tracing enabled for this process\n");
    }
    if (unlink("ux0:data/sh3/diag/stencilwrap") == 0) { setenv("SH3_STENCIL_WRAP","1",1); fprintf(stderr,"[diag] stencil INCR/DECR wrap candidate enabled\n"); }
    if (unlink("ux0:data/sh3/diag/lensprobe") == 0) { setenv("SH3_DIAG_LENSPROBE","1",1); fprintf(stderr,"[diag] original lens-flare occlusion probe restored (A/B)\n"); }
    FILE *lensquery_off=fopen("ux0:data/sh3/diag/lensqueryoff","rb");
    if (lensquery_off) {
        fclose(lensquery_off);
        if (unlink("ux0:data/sh3/diag/lensqueryoff")) return 5;
        setenv("SH3_DIAG_LENSQUERY_OFF","1",1);
        fprintf(stderr,"[diag] lensflare_ztest early-out cost probe enabled (visual behavior unqualified)\n");
    }
    FILE *readback_finish=fopen("ux0:data/sh3/diag/readbackfinish","rb");
    if (readback_finish) {
        fclose(readback_finish);
        if (unlink("ux0:data/sh3/diag/readbackfinish")) { fprintf(stderr,"[diag] cannot consume readbackfinish trigger\n"); return 5; }
        fprintf(stderr,"[diag] 64x64 producer finish already enabled by default\n");
    }
    FILE *readback_no_finish=fopen("ux0:data/sh3/diag/readbacknofinish","rb");
    if (readback_no_finish) {
        fclose(readback_no_finish);
        if (unlink("ux0:data/sh3/diag/readbacknofinish")) { fprintf(stderr,"[diag] cannot consume readbacknofinish trigger\n"); return 5; }
        setenv("SH3_DIAG_READBACK_NO_FINISH","1",1);
        fprintf(stderr,"[diag] 64x64 producer finish disabled for comparison\n");
    }
    FILE *profile_heap=fopen("ux0:data/sh3/diag/profileheap","rb");
    FILE *budget_worker=fopen("ux0:data/sh3/diag/budgetworker","rb");
    if (budget_worker) {
        fclose(budget_worker);
        if (unlink("ux0:data/sh3/diag/budgetworker")) return 5;
        extern int g_diag_worker_budget;
        g_diag_worker_budget=1;
        fprintf(stderr,"[diag] worker omission diagnostic: 613370 off 120..150s, both audio callbacks off 160..190s, then restore; not playable\n");
    }
    FILE *budget_passes=fopen("ux0:data/sh3/diag/budgetpasses","rb");
    if (budget_passes) {
        fclose(budget_passes);
        if (unlink("ux0:data/sh3/diag/budgetpasses")) return 5;
        setenv("SH3_DIAG_BUDGET_PASSES","1",1);
        fprintf(stderr,"[diag] visual omission diagnostic: small FVF144/PS0 off120..150s, VS2f/PS4 off160..190s; then restore, park33:210 only\n");
    }
    FILE *budget_draw=fopen("ux0:data/sh3/diag/budgetdraw","rb");
    if (budget_draw) {
        fclose(budget_draw);
        if (unlink("ux0:data/sh3/diag/budgetdraw")) return 5;
        setenv("SH3_DIAG_BUDGET_DRAW","1",1);
        fprintf(stderr,"[diag] destructive-visual budget probe: gfx off 120..150s, prep+gfx off 160..190s, then restore; diagnostic only\n");
    }
    FILE *res=fopen("ux0:data/sh3/diag/res","rb");
    if (res) {
        unsigned w=0,h=0; char trailing=0;
        int fields=fscanf(res,"%ux%u %c",&w,&h,&trailing);
        fclose(res);
        if (unlink("ux0:data/sh3/diag/res")) return 5;
        /* Same 30:17 shape as the panel only: the widescreen projection and HUD hooks assume that ratio. */
        if (fields!=2 || w<480 || w>960 || w%30 || h!=w/30*17) { fprintf(stderr,"[diag] invalid res marker (use 30:17, e.g. 720x408)\n"); return 5; }
        g_diag_res_w=w; g_diag_res_h=h;
        fprintf(stderr,"[diag] internal render resolution %ux%u, scaled to the 960x544 display\n",w,h);
    }
    FILE *rt512=fopen("ux0:data/sh3/diag/rt512","rb");
    if (rt512) {
        fclose(rt512);
        if (unlink("ux0:data/sh3/diag/rt512")) return 5;
        setenv("SH3_DIAG_RT512","1",1);
        fprintf(stderr,"[diag] experimental render-target cap 512; display remains 960x544\n");
    }
    if (profile_heap) {
        fclose(profile_heap);
        if (unlink("ux0:data/sh3/diag/profileheap")) { fprintf(stderr,"[diag] cannot consume profileheap trigger\n"); return 5; }
        setenv("SH3_DIAG_HEAPSTATS","1",1);
        fprintf(stderr,"[diag] heap guard scan profiling enabled for this process\n");
    }
    FILE *fade_trace=fopen("ux0:data/sh3/diag/fadetrace","rb");
    if (fade_trace) {
        fclose(fade_trace);
        if (unlink("ux0:data/sh3/diag/fadetrace")) return 5;
        setenv("SH3_DIAG_FADETRACE","1",1);
        fprintf(stderr,"[diag] sampled startup fade draw trace enabled\n");
    }
    FILE *zone_trace=fopen("ux0:data/sh3/diag/zonetrace","rb");
    if (zone_trace) {
        fclose(zone_trace);
        if (unlink("ux0:data/sh3/diag/zonetrace")) return 5;
        setenv("SH3_DIAG_ZONE_TRACE","1",1);
        fprintf(stderr,"[diag] read-only stage/room trace enabled\n");
    }
    FILE *tiny_log=fopen("ux0:data/sh3/diag/tinylog","rb");
    if (tiny_log) {
        fclose(tiny_log);
        if (unlink("ux0:data/sh3/diag/tinylog")) { fprintf(stderr,"[diag] cannot consume tinylog trigger\n"); return 5; }
        setenv("SH3_DIAG_TINYLOG","1",1);
    }
    /* The Vita display is widescreen; retain a one-shot 4:3 comparison mode. */
    FILE *squeeze_trace=fopen("ux0:data/sh3/diag/squeezetrace","rb");
    if(squeeze_trace) {
        fclose(squeeze_trace);
        if(unlink("ux0:data/sh3/diag/squeezetrace"))return 5;
        setenv("SH3_DIAG_SQUEEZE_TRACE","1",1);
    }
    { FILE *fb=fopen("ux0:data/sh3/diag/footbias","rb"); if(fb){ char v[24]={0}; if(!fgets(v,sizeof v,fb))v[0]=0; fclose(fb); unlink("ux0:data/sh3/diag/footbias"); if(v[0])setenv("SH3_FOOT_BIAS",v,1); } }
    FILE *blood_audit=fopen("ux0:data/sh3/diag/bloodaudit","rb");
    if(blood_audit) {
        fclose(blood_audit);
        if(unlink("ux0:data/sh3/diag/bloodaudit"))return 5;
        setenv("SH3_BLOOD_AUDIT","1",1);
    }
    FILE *blood_packet=fopen("ux0:data/sh3/diag/bloodpacket","rb");
    if(blood_packet) {
        int mode=fgetc(blood_packet),tail=fgetc(blood_packet);
        int valid=(mode=='0'||mode=='1') && (tail==EOF || (tail=='\n' && fgetc(blood_packet)==EOF));
        fclose(blood_packet);
        if(!valid){fprintf(stderr,"[blood-packet] expected 0 or 1\n");return 5;}
        if(unlink("ux0:data/sh3/diag/bloodpacket"))return 5;
        setenv("SH3_BLOOD_PACKET_PASS",mode=='1'?"1":"0",1);
        fprintf(stderr,"[blood-packet] world projection override=%c\n",mode);
    }
    FILE *wide_test=fopen("ux0:data/sh3/diag/wide","rb");
    if (wide_test) {
        fclose(wide_test);
        if (unlink("ux0:data/sh3/diag/wide")) { fprintf(stderr,"[diag] cannot consume wide trigger\n"); return 5; }
    }
    FILE *classic_test=fopen("ux0:data/sh3/diag/classic43","rb");
    if (classic_test) {
        fclose(classic_test);
        if (unlink("ux0:data/sh3/diag/classic43")) { fprintf(stderr,"[diag] cannot consume classic43 trigger\n"); return 5; }
        fprintf(stderr,"[diag] experimental classic 4:3 comparison enabled\n");
    } else {
        setenv("SH3_DIAG_WIDE_NATIVE","1",1);
        setenv("SH3_DIAG_WIDE_FOV","1",1);
        setenv("SH3_DIAG_WIDE_HUD","1",1);
        fprintf(stderr,"[diag] experimental wide rendering enabled by default\n");
    }
#ifdef SH3_MOVIE_LAB
    FILE *movie_core=fopen("ux0:data/sh3/diag/moviecore","rb");
    if(movie_core) {
        char value[8]={0};size_t n=fread(value,1,sizeof(value)-1,movie_core);fclose(movie_core);
        if(n==2 && value[1]=='\n'){value[1]=0;n=1;}
        if(n!=1 || value[0]<'0' || value[0]>'2') {
            fprintf(stderr,"[movie-lab] moviecore requires exactly 0, 1 or 2\n");return 5;
        }
        if(unlink("ux0:data/sh3/diag/moviecore"))return 5;
        setenv("SH4_MOVIE_CORE",value,1);
        fprintf(stderr,"[movie-lab] diagnostic decoder core %s\n",value);
    }
#ifdef SH3_MOVIE_HW_LAB
    FILE *movie_hw=fopen("ux0:data/sh3/diag/moviehw","rb");
    if(movie_hw) {
        fclose(movie_hw);
        if(unlink("ux0:data/sh3/diag/moviehw"))return 5;
        setenv("SH3_DIAG_MOVIE_HW","1",1);
        fprintf(stderr,"[movie-lab] experimental AvPlayer decode enabled\n");
    }
    FILE *movie_as=fopen("ux0:data/sh3/diag/movieas","rb");
    if(movie_as) {
        static char name[16]; size_t n=fread(name,1,sizeof name-1,movie_as); fclose(movie_as);
        name[n]=0; while(n && (name[n-1]=='\n'||name[n-1]=='\r'||name[n-1]==' ')) name[--n]=0;
        if(unlink("ux0:data/sh3/diag/movieas"))return 5;
        setenv("SH3_DIAG_MOVIE_AS",name,1);
        fprintf(stderr,"[movie-lab] title film op.000 replaced by %s\n",name);
    }
    FILE *movie_hw_tex=fopen("ux0:data/sh3/diag/moviehwtex","rb");
    if(movie_hw_tex) {
        fclose(movie_hw_tex);
        if(unlink("ux0:data/sh3/diag/moviehwtex"))return 5;
        setenv("SH3_DIAG_MOVIE_HW_TEX","1",1);
        fprintf(stderr,"[movie-lab] AvPlayer frames shown directly as YUV texture\n");
    }
#endif
    FILE *movie_wide=fopen("ux0:data/sh3/diag/moviewide","rb");
    if(movie_wide) {
        fclose(movie_wide);
        if(unlink("ux0:data/sh3/diag/moviewide"))return 5;
        setenv("SH3_DIAG_MOVIE_WIDE","1",1);
        fprintf(stderr,"[movie-lab] native film stretch enabled\n");
    }
    FILE *movie_direct=fopen("ux0:data/sh3/diag/moviedirect","rb");
    if(movie_direct) {
        fclose(movie_direct);
        if(unlink("ux0:data/sh3/diag/moviedirect"))return 5;
        setenv("SH3_DIAG_MOVIE_DIRECT_IO","1",1);
        fprintf(stderr,"[movie-lab] direct movie cache reads enabled\n");
    }
    FILE *movie_probe=fopen("ux0:data/sh3/diag/movieprobe","rb");
    if (movie_probe) {
        fclose(movie_probe);
        if (unlink("ux0:data/sh3/diag/movieprobe")) return 5;
        static const char *const flags[]={"SH3_DIAG_MOVIES","SH3_DIAG_MOVIE_PC","SH3_DIAG_MSG_QUEUE","SH3_DIAG_MULTIWAIT_POLL",
            "SH3_DIAG_MOVIE_RETAIN_FILTERS","SH3_DIAG_MOVIE_GUEST_RENDERER",
            "SH3_DIAG_MOVIE_PREP_SOURCE","SH3_DIAG_MOVIE_PROBE_PINS",
            "SH3_DIAG_MOVIE_TEST_CONNECT","SH3_DIAG_MOVIE_ONE_FRAME",
            "SH3_DIAG_MOVIE_INIT_GUEST"};
        for (unsigned i=0;i<sizeof flags/sizeof flags[0];i++) setenv(flags[i],"1",1);
        fprintf(stderr,"[movie-lab] isolated one-frame probe enabled; playback remains disabled\n");
    }
    FILE *movie_present=fopen("ux0:data/sh3/diag/moviepresent","rb");
    if (movie_present) {
        fclose(movie_present);
        if (unlink("ux0:data/sh3/diag/moviepresent")) return 5;
        if (!getenv("SH3_DIAG_MOVIES")) { fprintf(stderr,"[movie-lab] moviepresent requires movieprobe\n"); return 5; }
        unsetenv("SH3_DIAG_MOVIE_ONE_FRAME"); /* avoid an earlier Receive/Stop before the Run-frame probe */
        setenv("SH3_DIAG_MOVIE_RUN_FRAME","1",1);
        setenv("SH3_DIAG_MOVIE_PRESENT_PROBE","1",1);
        setenv("SH3_DIAG_MOVIE_CAPTURE_DRAW","1",1);
        fprintf(stderr,"[movie-lab] experimental injected draw and immediate capture enabled\n");
    }
    FILE *movie_bottom=fopen("ux0:data/sh3/diag/moviebottom","rb");
    if (movie_bottom) {
        fclose(movie_bottom);
        if (unlink("ux0:data/sh3/diag/moviebottom")) return 5;
        if (!getenv("SH3_DIAG_MOVIES")) { fprintf(stderr,"[movie-lab] moviebottom requires movieprobe\n"); return 5; }
        unsetenv("SH3_DIAG_MOVIE_TOPDOWN");
        fprintf(stderr,"[movie-lab] experimental bottom-up movie samples enabled\n");
    }
    FILE *movie_sequence=fopen("ux0:data/sh3/diag/moviesequence","rb");
    if (movie_sequence) {
        fclose(movie_sequence);
        if (unlink("ux0:data/sh3/diag/moviesequence")) return 5;
        if (!getenv("SH3_DIAG_MOVIE_PRESENT_PROBE")) { fprintf(stderr,"[movie-lab] moviesequence requires moviepresent\n"); return 5; }
        setenv("SH3_DIAG_MOVIE_PRESENT_SEQUENCE","1",1);
        fprintf(stderr,"[movie-lab] experimental three-frame sequence enabled\n");
    }
    FILE *movie_fit=fopen("ux0:data/sh3/diag/moviefit","rb");
    if (movie_fit) {
        fclose(movie_fit);
        if (unlink("ux0:data/sh3/diag/moviefit")) return 5;
        if (!getenv("SH3_DIAG_MOVIE_PRESENT_PROBE")) { fprintf(stderr,"[movie-lab] moviefit requires moviepresent\n"); return 5; }
        setenv("SH3_DIAG_MOVIE_FIT_PROBE","1",1);
        fprintf(stderr,"[movie-lab] experimental 4:3 fit rectangle enabled\n");
    }
    FILE *movie_short=fopen("ux0:data/sh3/diag/movieshort","rb");
    if (movie_short) {
        fclose(movie_short);
        if (unlink("ux0:data/sh3/diag/movieshort")) return 5;
        if (!getenv("SH3_DIAG_MOVIES")) { fprintf(stderr,"[movie-lab] movieshort requires movieprobe\n"); return 5; }
        setenv("SH3_DIAG_MOVIE_PC_SHORT","1",1);
        fprintf(stderr,"[movie-lab] short PC movie source requested\n");
    }
    FILE *movie_short_320=fopen("ux0:data/sh3/diag/movie320","rb");
    if (movie_short_320) {
        fclose(movie_short_320);
        if (unlink("ux0:data/sh3/diag/movie320")) return 5;
        if (!getenv("SH3_DIAG_MOVIE_PC_SHORT")) { fprintf(stderr,"[movie-lab] movie320 requires movieshort\n"); return 5; }
        setenv("SH3_DIAG_MOVIE_PC_SHORT_320","1",1);
        fprintf(stderr,"[movie-lab] 320x240 short PC movie source requested\n");
    }
    FILE *movie_short_384=fopen("ux0:data/sh3/diag/movie384","rb");
    if (movie_short_384) {
        fclose(movie_short_384);
        if (unlink("ux0:data/sh3/diag/movie384")) return 5;
        if (!getenv("SH3_DIAG_MOVIE_PC_SHORT") || getenv("SH3_DIAG_MOVIE_PC_SHORT_320")) { fprintf(stderr,"[movie-lab] movie384 requires movieshort without movie320\n"); return 5; }
        setenv("SH3_DIAG_MOVIE_PC_SHORT_384","1",1);
        fprintf(stderr,"[movie-lab] 384x288 short PC movie source requested\n");
    }
    FILE *movie_catchup=fopen("ux0:data/sh3/diag/moviecatchup","rb");
    if (movie_catchup) {
        fclose(movie_catchup);
        if (unlink("ux0:data/sh3/diag/moviecatchup")) return 5;
        if (!getenv("SH3_DIAG_MOVIES")) { fprintf(stderr,"[movie-lab] moviecatchup requires movieprobe\n"); return 5; }
        setenv("SH4_MOVIE_ALL_FRAMES","0",1);
        fprintf(stderr,"[movie-lab] late B-frame skipping requested\n");
    }
    FILE *movie_texture_audit=fopen("ux0:data/sh3/diag/movietex","rb");
    if (movie_texture_audit) {
        fclose(movie_texture_audit);
        if (unlink("ux0:data/sh3/diag/movietex")) return 5;
        if (!getenv("SH3_DIAG_MOVIES")) { fprintf(stderr,"[movie-lab] movietex requires movieprobe\n"); return 5; }
        setenv("SH3_DIAG_MOVIE_TEXTURE_AUDIT","1",1);
        fprintf(stderr,"[movie-lab] bounded movie texture audit enabled\n");
    }
    FILE *movie_wait_audit=fopen("ux0:data/sh3/diag/moviewait","rb");
    if (movie_wait_audit) {
        fclose(movie_wait_audit);
        if (unlink("ux0:data/sh3/diag/moviewait")) return 5;
        if (!getenv("SH3_DIAG_MOVIES")) { fprintf(stderr,"[movie-lab] moviewait requires movieprobe\n"); return 5; }
        setenv("SH3_DIAG_MOVIE_WAIT_AUDIT","1",1);
        fprintf(stderr,"[movie-lab] bounded movie wait audit enabled\n");
    }
    FILE *movie_play=fopen("ux0:data/sh3/diag/movieplay","rb");
    if (movie_play) {
        fclose(movie_play);
        if (unlink("ux0:data/sh3/diag/movieplay")) return 5;
        if (!getenv("SH3_DIAG_MOVIES") || getenv("SH3_DIAG_MOVIE_PRESENT_PROBE")) {
            fprintf(stderr,"[movie-lab] movieplay requires movieprobe without moviepresent\n"); return 5;
        }
        unsetenv("SH3_DIAG_MOVIE_ONE_FRAME");
        setenv("SH3_DIAG_MOVIE_ALLOW_RUN","1",1);
        fprintf(stderr,"[movie-lab] experimental threaded playback probe enabled\n");
    }
#if defined(SH3_MOVIE_PLAYTEST_DEFAULT) && defined(SH3_MOVIE_HW_LAB)
    /* Playtest build for Valentin: the exact marker set qualified on the console (movieprobe + movieplay +
       moviewide + moviehw + moviehwtex, bench v9), without short source, audits or bench automation.
       Full films from ux0:data/sh3/movie/<name>.mpg, AvPlayer when <name>.mp4 exists. */
    if (!getenv("SH3_DIAG_MOVIES")) {
        static const char *const flags[]={"SH3_DIAG_MOVIES","SH3_DIAG_MOVIE_PC","SH3_DIAG_MSG_QUEUE","SH3_DIAG_MULTIWAIT_POLL",
            "SH3_DIAG_MOVIE_RETAIN_FILTERS","SH3_DIAG_MOVIE_GUEST_RENDERER",
            "SH3_DIAG_MOVIE_PREP_SOURCE","SH3_DIAG_MOVIE_PROBE_PINS",
            "SH3_DIAG_MOVIE_TEST_CONNECT","SH3_DIAG_MOVIE_INIT_GUEST",
            "SH3_DIAG_MOVIE_ALLOW_RUN","SH3_DIAG_MOVIE_WIDE","SH3_DIAG_MOVIE_HW","SH3_DIAG_MOVIE_HW_TEX"};
        for (unsigned i=0;i<sizeof flags/sizeof flags[0];i++) setenv(flags[i],"1",1);
        fprintf(stderr,"[movie-playtest] full PC movies enabled by default (AvPlayer direct when .mp4 present)\n");
    }
#endif
#endif
    /* One-shot opt-in for unattended controller tests, never enabled by default. */
    FILE *auto_confirm=fopen("ux0:data/sh3/diag/autoconfirm","rb");
    if (auto_confirm) {
        fclose(auto_confirm);
        if (unlink("ux0:data/sh3/diag/autoconfirm")) return 5;
        setenv("SH3_DIAG_AUTOSTART","1",1);
        setenv("SH3_DIAG_AUTOCONFIRM","1",1);
        fprintf(stderr,"[diag] bounded confirmation until nonzero stage enabled\n");
    }
    FILE *auto_start=fopen("ux0:data/sh3/diag/autostart","rb");
    if (auto_start) {
        fclose(auto_start);
        if (unlink("ux0:data/sh3/diag/autostart")) { fprintf(stderr,"[diag] cannot consume autostart trigger\n"); return 5; }
        setenv("SH3_DIAG_AUTOSTART","1",1);
        fprintf(stderr,"[diag] unattended confirmation test enabled\n");
    }
    const char *move_trigger="ux0:data/sh3/diag/automove";
    FILE *auto_move=fopen(move_trigger,"rb");
    int move_control=0;
    if (!auto_move) {
        move_trigger="ux0:data/sh3/diag/movecontrol";
        auto_move=fopen(move_trigger,"rb");
        move_control=auto_move!=NULL;
    }
    if (auto_move) {
        unsigned long delay=0;
        int valid=fscanf(auto_move,"%lu",&delay)==1 && delay>=2000 && delay<=600000;
        fclose(auto_move);
        if (unlink(move_trigger)) { fprintf(stderr,"[diag] cannot consume movement trigger\n"); return 5; }
        if (valid) {
            char value[16]; snprintf(value,sizeof value,"%lu",delay);
            setenv(move_control?"SH3_DIAG_MOVE_CONTROL":"SH3_DIAG_AUTOMOVE","1",1);
            if (move_control) setenv("SH3_DIAG_PADLOG","1",1);
            setenv("SH3_DIAG_MOVE_AT_MS",value,1);
            fprintf(stderr,"[diag] unattended %s test at %lu ms\n",move_control?"movement capture":"movement",delay);
        }
    }
    /* GL stays on one thread without the separate GL command queue (SH4_NORTHREAD). By default that thread is the
       game thread (sync). prepthread moves the D3D8 replay and every GL call to the sh4vita preparation thread
       (core 2); the game thread is then pinned to core 0 and guest workers to core 1. */
    FILE *move_key_file=fopen("ux0:data/sh3/diag/movekey","rb");
    if (move_key_file) {
        char key[8]={0};size_t n=fread(key,1,sizeof key-1,move_key_file);fclose(move_key_file);
        if (unlink("ux0:data/sh3/diag/movekey")) return 5;
        key[strcspn(key,"\r\n")]=0;
        if (!getenv("SH3_DIAG_AUTOMOVE") || !n || strlen(key)!=1 || !strchr("WQEI",key[0])) {
            fprintf(stderr,"[diag] movekey requires automove and one of W,Q,E,I\n");return 5;
        }
        setenv("SH3_DIAG_MOVE_KEY",key,1);
        fprintf(stderr,"[diag] automated movement key selected: %s\n",key);
    }
    setenv("SH4_NORTHREAD","1",1);
    if (g_diag_perf&PERF_PREPTHREAD) {
        setenv("SH4_PREP","thread",1);
        int pinned=sceKernelChangeThreadCpuAffinityMask(sceKernelGetThreadId(),SCE_KERNEL_CPU_MASK_USER_0);
        fprintf(stderr,"[diag] D3D8 preparation thread enabled; game thread affinity request=%08x\n",(unsigned)pinned);
    } else setenv("SH4_PREP","sync",1);
    FILE *prep_lazy=fopen("ux0:data/sh3/diag/preplazy","rb");
    if (prep_lazy) {
        fclose(prep_lazy);
        if (unlink("ux0:data/sh3/diag/preplazy")) { fprintf(stderr,"[diag] cannot consume preplazy trigger\n"); return 5; }
        setenv("SH4_PREP","lazy",1);
        setenv("SH3_DIAG_PREP_LAZY_SAFE","1",1);
        fprintf(stderr,"[diag] experimental single-thread lazy preparation enabled\n");
    }
    const char *exe="ux0:data/sh3/sh3.exe";
    SceUID guest_id=sceKernelAllocMemBlock("sh3pc_guest",SCE_KERNEL_MEMBLOCK_TYPE_USER_RW,DIAG_MEM_SIZE,NULL);
    if (guest_id<0 || sceKernelGetMemBlockBase(guest_id,(void **)&g_mem)<0 || !g_mem) {
        fprintf(stderr,"[diag] Vita guest memory allocation failed: %08x\n",(unsigned)guest_id); return 5;
    }
    memset(g_mem,0,DIAG_MEM_SIZE);
#else
    if (argc != 2) { fprintf(stderr, "usage: sh3vita-host <sh3.exe>\n"); return 2; }
    const char *exe=argv[1];
    /* Keep unattended Mac tests silent without changing system volume.
       Explicit opt-in permits audible diagnostics when requested. */
    const char *audible=getenv("SH3_DIAG_AUDIBLE");
    if (!audible || strcmp(audible,"1")) {
        if (setenv("SDL_AUDIODRIVER","dummy",1)) { fprintf(stderr,"[diag] cannot select silent audio driver\n"); return 9; }
        fprintf(stderr,"[diag] Mac diagnostic audio output muted\n");
    }
#endif
    if (SDL_Init(SDL_INIT_TIMER)) { fprintf(stderr,"[diag] SDL timer init failed: %s\n",SDL_GetError()); return 9; }
    FILE *f=fopen(exe,"rb"); if (!f || fseek(f,0,SEEK_END)) { fprintf(stderr,"[diag] cannot open executable %s\n",exe); return 3; }
    long n=ftell(f); if (n<=0 || fseek(f,0,SEEK_SET)) return 4;
    uint8_t *bytes=malloc((size_t)n);
#ifndef __vita__
    g_mem=calloc(1,DIAG_MEM_SIZE);
#endif
    if (!bytes || !g_mem || fread(bytes,1,(size_t)n,f)!=(size_t)n) return 5;
    fclose(f); Sh3PeLayout layout; int r=sh3_pe_load(bytes,(size_t)n,g_mem,200u*1024*1024,&layout); free(bytes);
    if (r || layout.entry != x86_entry) { fprintf(stderr,"[diag] PE/entry mismatch %d\n",r); return 6; }
#ifdef SH3_RAND_LAB
    extern int diag_rand_enabled,diag_rand_layout_valid(void);
    FILE *native_rand=fopen("ux0:data/sh3/diag/nativerand","rb");
    if (native_rand) {
        char v[8]={0}; if (!fgets(v,sizeof v,native_rand)) v[0]=0;
        fclose(native_rand);
        if (!strncmp(v,"ab",2)) { extern int g_ab_rand; g_ab_rand=1; }
        if (unlink("ux0:data/sh3/diag/nativerand") || !diag_rand_layout_valid()) {
            fprintf(stderr,"[diag] native rand PE signature mismatch or marker error\n"); return 5;
        }
        diag_rand_enabled=1;
        fprintf(stderr,"[diag] experimental native rand warm path enabled; cold path retained\n");
    }
#endif
#if defined(__vita__) || defined(_WIN32)
    /* Diagnostic new-game entry only. Retain the original initialization and
       loading path (005d7540), changing its two spawn descriptors, not live
       stage state. Grid centers need not be safe floor/spawn positions. */
#ifdef _WIN32
    const char *spawn=getenv("SH3_DIAG_STARTROOM"); /* Windows bench: "stage,room" */
    if (spawn) {
        unsigned stage=0,room=0; char trailing=0;
        int fields=sscanf(spawn,"%u,%u %c",&stage,&room,&trailing);
#else
    FILE *spawn=fopen("ux0:data/sh3/diag/startroom","rb");
    if (spawn) {
        unsigned stage=0,room=0; char trailing=0;
        int fields=fscanf(spawn,"%u,%u %c",&stage,&room,&trailing);
        fclose(spawn);
        if (unlink("ux0:data/sh3/diag/startroom")) return 5;
#endif
        if (fields!=2 || stage<1 || stage>39 || room<1 || room>266 ||
            g_mem[0x71e074]!=34 || g_mem[0x71e08c]!=1) {
            fprintf(stderr,"[diag] invalid diagnostic new-game room or PE spawn layout\n"); return 5;
        }
        uint16_t lo=0,hi=0;
        memcpy(&lo,g_mem+0x6b7080+2*stage,2);
        memcpy(&hi,g_mem+0x6b7080+2*(stage+1),2);
        if (room<lo || room>=hi) {
            fprintf(stderr,"[diag] diagnostic room outside requested stage interval\n"); return 5;
        }
        unsigned grid=g_mem[0x6b6f70+room];
        int gx=grid>>4,gz=grid&15;
        if (gx>8) gx-=15;
        if (gz>8) gz-=15;
        float x=((float)gx-0.5f)*40000.0f,z=((float)gz-0.5f)*40000.0f;
#ifdef _WIN32
        { const char *xz=getenv("SH3_DIAG_STARTXZ"); if (xz) sscanf(xz,"%f,%f",&x,&z); }
#else
        { FILE *xzf=fopen("ux0:data/sh3/diag/startxz","rb"); if (xzf) { if (fscanf(xzf,"%f,%f",&x,&z)!=2) { x=((float)gx-0.5f)*40000.0f; z=((float)gz-0.5f)*40000.0f; } fclose(xzf); unlink("ux0:data/sh3/diag/startxz"); } }
#endif
        for (unsigned i=0;i<2;i++) {
            uint8_t *descriptor=g_mem+0x71e060+24*i;
            memcpy(descriptor,&x,4); memcpy(descriptor+8,&z,4);
            descriptor[20]=(uint8_t)stage;
        }
        fprintf(stderr,"[zone] experimental new-game spawn stage=%u room_candidate=%u grid=%02x x=%.0f z=%.0f y=0; native loading, floor/flags unverified\n",stage,room,grid,x,z);
    }
#endif
#ifdef _WIN32
    if (getenv("SH3_DIAG_PERF")) g_diag_perf=(unsigned)strtoul(getenv("SH3_DIAG_PERF"),NULL,0);
#endif
    { const char *v=getenv("SH3_NATIVE_CLIP");g_diag_native_clip=v && !strcmp(v,"1"); g_ab_clip=v && !strcmp(v,"ab"); }
    { extern int g_tex_eager; const char *v=getenv("SH3_TEX_EAGER"); g_tex_eager=v && !strcmp(v,"1"); g_ab_texeager=v && !strcmp(v,"ab"); }
#ifdef SH3_PARTICLE_NATIVE_LAB
    { extern int g_particle_native; const char *v=getenv("SH3_PARTICLE_NATIVE"); g_particle_native=v && !strcmp(v,"1"); g_ab_particle=v && !strcmp(v,"ab"); }
    { const char *v=getenv("SH3_ALL_NATIVE"); g_ab_all=v && !strcmp(v,"ab"); }
    { const char *v=getenv("SH3_LAB_TIMING"); g_lab_timing=v && !strcmp(v,"1"); }
    { extern int g_copy64_native; const char *v=getenv("SH3_COPY64_NATIVE"); g_copy64_native=v && !strcmp(v,"1"); g_ab_copy64=v && !strcmp(v,"ab"); }
    { extern int g_wall_native; const char *v=getenv("SH3_WALL_NATIVE"); g_wall_native=v && !strcmp(v,"1"); g_ab_wall=v && !strcmp(v,"ab"); }
    { extern int g_pupdate_native; const char *v=getenv("SH3_PUPDATE_NATIVE"); g_pupdate_native=v && !strcmp(v,"1"); g_ab_pupdate=v && !strcmp(v,"ab"); }
    { extern int g_pcyl_native; const char *v=getenv("SH3_PCYL_NATIVE"); g_pcyl_native=v && !strcmp(v,"1"); g_ab_pcyl=v && !strcmp(v,"ab"); }
    { extern int g_pbox_native; const char *v=getenv("SH3_PBOX_NATIVE"); g_pbox_native=v && !strcmp(v,"1"); g_ab_pbox=v && !strcmp(v,"ab"); }
    { extern int g_pplanes_native; const char *v=getenv("SH3_PPLANES_NATIVE"); g_pplanes_native=v && !strcmp(v,"1"); g_ab_pplanes=v && !strcmp(v,"ab"); }
    { extern int g_blur_native; const char *v=getenv("SH3_BLUR_NATIVE"); g_blur_native=v && !strcmp(v,"1"); g_ab_blur=v && !strcmp(v,"ab"); }
    { extern int g_prand_native; const char *v=getenv("SH3_PRAND_NATIVE"); g_prand_native=v && !strcmp(v,"1"); g_ab_prand=v && !strcmp(v,"ab"); }
    { extern int g_texfill_native; const char *v=getenv("SH3_TEXFILL_NATIVE"); g_texfill_native=v && !strcmp(v,"1"); g_ab_texfill=v && !strcmp(v,"ab"); }
    { extern int g_collide_native; const char *v=getenv("SH3_COLLIDE_NATIVE"); g_collide_native=v && !strcmp(v,"1"); g_ab_collide=v && !strcmp(v,"ab"); }
    { extern int g_qsort_native; const char *v=getenv("SH3_QSORT_NATIVE"); g_qsort_native=v && !strcmp(v,"1"); g_ab_qsort=v && !strcmp(v,"ab"); }
    { extern int g_particle_draw_native; const char *v=getenv("SH3_PARTICLE_DRAW"); g_particle_draw_native=v && !strcmp(v,"1"); g_ab_pdraw=v && !strcmp(v,"ab"); }
#endif
#ifdef SH3_SHADOW_NATIVE_LAB
    { extern int g_shadow_strip; const char *v=getenv("SH3_SHADOW_STRIP"); g_shadow_strip=v && !strcmp(v,"1"); g_ab_sstrip=v && !strcmp(v,"ab"); }
    { extern int g_shadow_native; const char *v=getenv("SH3_SHADOW_NATIVE"); g_shadow_native=v && !strcmp(v,"1"); g_ab_shadow=v && !strcmp(v,"ab"); }
#endif
#ifdef SH3_SHADOW_PROBE
    g_ab_probe=shadow_probe_mode()>=2;
#endif
#ifdef SH3_PARTICLE_NATIVE_LAB
    { const char *v=getenv("SH3_ALL_NATIVE"); if (v && !strcmp(v,"1")) {
        diag_all_native_set(1);
        fprintf(stderr,"[allnative] constant mode enabled (all compiled native kernels)\n");
    } }
#endif
#ifdef SH3_PC24_VEC3_LAB
    g_diag_pc24_vec3=getenv("SH3_PC24_VEC3_OFF")==NULL;
    fprintf(stderr,"[pc24-vec3] native path enabled=%d (bounded PC24 only)\n",g_diag_pc24_vec3);
#endif
    diag_guest_heap=diag_heap_create(g_mem,DIAG_MEM_SIZE,layout.heap_base,DIAG_MEM_SIZE-layout.heap_base);
    if(diag_guest_heap) diag_taskmem=diag_taskmem_create(diag_guest_heap);
    if(!diag_taskmem) { fprintf(stderr,"[diag] cannot initialize COM task allocator\n"); return 5; }
    if (!diag_guest_heap) { fprintf(stderr,"[diag] guest heap initialization failed\n"); return 8; }
    for (unsigned i=0; i<x86_iat_n; i++) {
        uint32_t token=DIAG_IMPORT_BASE+i;
        if (x86_iat[i].slot > DIAG_MEM_SIZE-4) return 7;
        memcpy(g_mem+x86_iat[i].slot,&token,4);
    }
    CPU cpu={0}; cpu.esp=0x300000; cpu.fs_base=0x10000; cpu.fcw=0x37f; diag_main_cpu=&cpu;
    { uint32_t tid=0x100; memcpy(g_mem+cpu.fs_base+0x24,&tid,4); }
    cpu.esp-=4; *(uint32_t *)(g_mem+cpu.esp)=0; /* synthetic return address */
    fprintf(stderr,"[diag] start entry=%08x heap=%08x guest_mib=%u heap_bytes=%u (imports fail closed)\n",layout.entry,layout.heap_base,(unsigned)SH3_GUEST_MEMORY_MB,(unsigned)(DIAG_MEM_SIZE-layout.heap_base));
#ifdef SH3_PROFILE
    rt_prof_start();
#endif
    if (getenv("SH4_CPU_TIME")) diag_calibrate();
#ifdef _WIN32
    { HANDLE h=NULL; if (DuplicateHandle(GetCurrentProcess(),GetCurrentThread(),GetCurrentProcess(),&h,0,FALSE,DUPLICATE_SAME_ACCESS)) diag_main_thread_handle=h; }
#endif
    SDL_CreateThread(diag_freeze_watch,"freeze_watch",NULL);
    rt_call(&cpu,layout.entry);
    fprintf(stderr,"[diag] guest entry returned eax=%08x\n",cpu.eax);
    return 0;
}

#ifdef SH3_WIN_TRACE_4B18F0
/* Windows bench: trace the collision sweep F_004b18f0 called from the retry loops of F_004ca820 (zone 4 hang). */
extern void __real_F_004b18f0(CPU *);
void __wrap_F_004b18f0(CPU *c) {
    static unsigned long n; uint32_t esp=c->esp, a[6], i; float f[8]={0};
    memcpy(a,g_mem+esp+4,sizeof a);
    __real_F_004b18f0(c);
    n++;
    if (n<=24 || n%200000==0) {
        fprintf(stderr,"[sweep] #%lu args=%08x %08x %08x %08x %08x %08x ret=%u",n,a[0],a[1],a[2],a[3],a[4],a[5],c->eax);
        for (i=0;i<2;i++) if (a[i]>0 && a[i]<DIAG_MEM_SIZE-16) { memcpy(f+4*i,g_mem+a[i],12); fprintf(stderr," p%u=(%g,%g,%g)",i,f[4*i],f[4*i+1],f[4*i+2]); }
        fprintf(stderr,"\n");
    }
}
#endif
