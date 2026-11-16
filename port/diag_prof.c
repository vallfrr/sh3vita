/* Sampled profile of the translated x86 guest code, ported from sh4vita/port/src/rt/calltrace.c (GPL-3.0, same project family).
 * Build with cmake -DSH3_PROFILE=ON: generated code gets -finstrument-functions; a thread samples the current guest function of
 * the main thread every ms during frames begin..end (marker file ux0:data/sh3/diag/profilesamp containing "begin,end", or env SH3_PROFILE).
 * Offsets are relative to F_004043f0; resolve with tools/resolve_prof.sh. COM/IAT host calls are attributed to the host function. */
#ifdef SH3_PROFILE
#include <SDL.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
void rt_log(const char *fmt, ...);
#define NF __attribute__((no_instrument_function))
typedef struct CPU CPU;
extern void F_004043f0(CPU *c);
extern unsigned g_frame;
#define STK 512
#define HT 8192
static void *volatile stk[STK];
static volatile int depth;
static volatile unsigned long long n_calls;
static void *volatile cur_host;
static uintptr_t main_lo, main_hi;
static unsigned p_begin, p_end;
static struct { void *fn; unsigned n; } ht[HT];
static struct { void *fn; unsigned n; } inc[HT];
#define ET 16384
static struct { void *a, *b; unsigned n; } edge[ET];   /* inclusive parent->child pairs (adjacent on the stack) */   /* inclusive: samples with fn anywhere on the stack (counted once) */
#define HOT 16384
static struct { void *fn; unsigned n; } hot[HOT];   /* entries per guest function inside the window */
#define IS_MAIN() ((uintptr_t)__builtin_frame_address(0) - main_lo < main_hi - main_lo)
NF void *rt_prof_host(void *f) { void *pv = cur_host; if (IS_MAIN()) cur_host = f; return pv; }
NF void __cyg_profile_func_enter(void *fn, void *site) {
    (void)site; if (!IS_MAIN()) return;
    int d = depth; if (d < STK) stk[d] = fn; depth = d + 1; n_calls++;
    if (g_frame >= p_begin && g_frame < p_end) {
        unsigned h = ((uintptr_t)fn >> 2) * 2654435761u % HOT;
        while (hot[h].fn && hot[h].fn != fn) h = (h + 1) % HOT;
        hot[h].fn = fn; hot[h].n++;
    }
}
NF void __cyg_profile_func_exit(void *fn, void *site) {
    (void)site; if (!IS_MAIN()) return;
    int d = depth; if (d > STK) { depth = d - 1; return; }
    while (d > 0 && stk[d - 1] != fn) d--;
    depth = d > 0 ? d - 1 : 0;
}
static NF int prof_thread(void *arg) {
    (void)arg; unsigned total = 0, tick = 0; unsigned long long c0 = 0; unsigned f0 = 0;
    rt_log("[prof] thread started, window %u..%u\n", p_begin, p_end);
    for (;;) {
        SDL_Delay(1);
        if (++tick % 5000 == 0) rt_log("[prof] tick %u frame %u depth %d samples %u\n", tick, g_frame, depth, total);
        /* Stall dump: no frame presented for 4 s while the main guest thread keeps running -> print its instrumented call stack
           (top 24 entries, offsets relative to F_004043f0) and the current host function, then every 20 s while it lasts. */
        static unsigned last_frame, same, dumps;
        if (g_frame != last_frame) { last_frame = g_frame; same = 0; }
        else if (g_frame > 100 && ++same % 4000 == 0 && dumps < 12) {
            int d = depth; dumps++;
            rt_log("[stall] frame %u unchanged for %u ms, depth %d, host=%p\n", g_frame, same, d, (void *)cur_host);
            for (int i = d - 1; i >= 0 && i >= d - 24 && i < STK; i--)
                rt_log("[stall]   T %+d\n", (int)((char *)stk[i] - (char *)F_004043f0));
        }
        if (g_frame < p_begin) continue;
        if (!c0) { c0 = n_calls; f0 = g_frame; }
        if (g_frame >= p_end) break;
        int d = depth; void *fn = cur_host ? cur_host : d > 0 && d <= STK ? stk[d - 1] : NULL;
        unsigned h = ((uintptr_t)fn >> 2) * 2654435761u % HT;
        while (ht[h].fn && ht[h].fn != fn) h = (h + 1) % HT;
        ht[h].fn = fn; ht[h].n++; total++;
        if (d > STK) d = STK;
        for (int i = 0; i < d; i++) {
            void *g = stk[i]; int dup = 0;
            for (int j = 0; j < i && !dup; j++) dup = stk[j] == g;
            if (dup || !g) continue;
            unsigned k = ((uintptr_t)g >> 2) * 2654435761u % HT;
            while (inc[k].fn && inc[k].fn != g) k = (k + 1) % HT;
            inc[k].fn = g; inc[k].n++;
        }
        for (int i = 1; i < d; i++) {
            void *a = stk[i - 1], *b = stk[i]; int dup = 0;
            for (int j = 1; j < i && !dup; j++) dup = stk[j - 1] == a && stk[j] == b;
            if (dup || !a || !b) continue;
            unsigned k = (((uintptr_t)a >> 2) * 2654435761u ^ ((uintptr_t)b >> 2) * 40503u) % ET;
            while (edge[k].a && (edge[k].a != a || edge[k].b != b)) k = (k + 1) % ET;
            edge[k].a = a; edge[k].b = b; edge[k].n++;
        }
    }
    rt_log("[prof] %u samples over %u frames, ref F_004043f0=%p\n", total, p_end - p_begin, (void *)F_004043f0);
    rt_log("[prof] guest calls: %llu over %u frames = %llu per frame\n", n_calls - c0, g_frame - f0, (n_calls - c0) / (g_frame - f0 ? g_frame - f0 : 1));
    for (int k = 0; k < 60; k++) {
        int best = -1;
        for (int i = 0; i < HT; i++) if (ht[i].fn && (best < 0 || ht[i].n > ht[best].n)) best = i;
        if (best < 0 || !ht[best].n) break;
        rt_log("[prof] %5.1f%% %5u  T %+d\n", 100.0 * ht[best].n / total, ht[best].n, (int)((char *)ht[best].fn - (char *)F_004043f0));
        ht[best].n = 0;
    }
    for (int k = 0; k < 150; k++) {  /* inclusive time (function or any callee on the stack) */
        int best = -1;
        for (int i = 0; i < HT; i++) if (inc[i].fn && (best < 0 || inc[i].n > inc[best].n)) best = i;
        if (best < 0 || !inc[best].n) break;
        rt_log("[incl] %5.1f%% %5u  T %+d\n", 100.0 * inc[best].n / total, inc[best].n, (int)((char *)inc[best].fn - (char *)F_004043f0));
        inc[best].n = 0;
    }
    for (int k = 0; k < 400; k++) {  /* call-tree edges: samples with caller->callee on the stack */
        int best = -1;
        for (int i = 0; i < ET; i++) if (edge[i].a && (best < 0 || edge[i].n > edge[best].n)) best = i;
        if (best < 0 || edge[best].n < total / 500) break;
        rt_log("[edge] %5.1f%% T %+d T %+d\n", 100.0 * edge[best].n / total, (int)((char *)edge[best].a - (char *)F_004043f0), (int)((char *)edge[best].b - (char *)F_004043f0));
        edge[best].n = 0;
    }
    for (int k = 0; k < 40; k++) {   /* most entered functions: calls per frame */
        int best = -1;
        for (int i = 0; i < HOT; i++) if (hot[i].fn && (best < 0 || hot[i].n > hot[best].n)) best = i;
        if (best < 0 || !hot[best].n) break;
        rt_log("[hot] %8.1f calls/frame T %+d\n", (double)hot[best].n / (p_end - p_begin), (int)((char *)hot[best].fn - (char *)F_004043f0));
        hot[best].n = 0;
    }
    return 0;
}
/* Bench command "profile N": start the sampler now if no window was configured at launch. */
NF void rt_prof_window(unsigned begin, unsigned end) {
    static int started; if (started++) return;
    p_begin = begin; p_end = end; rt_log("[prof] window %u..%u (bench)\n", p_begin, p_end);
    if (!SDL_CreateThread(prof_thread, "prof", NULL)) rt_log("[prof] SDL_CreateThread: %s\n", SDL_GetError());
}
NF void rt_prof_start(void) {
    main_hi = (uintptr_t)__builtin_frame_address(0) + 65536; main_lo = main_hi - 4u * 1024 * 1024;
    char buf[64] = {0}; const char *e = getenv("SH3_PROFILE");
    if (!e) { FILE *f = fopen("ux0:data/sh3/diag/profilesamp", "rb"); if (!f) return; size_t n = fread(buf, 1, 63, f); fclose(f); buf[n] = 0; e = buf; }
    if (!strncmp(e, "bench", 5)) return;   /* window set later by the bench command "profile N" */
    char *q; p_begin = (unsigned)strtoul(e, &q, 10);
    p_end = *q == ',' ? (unsigned)strtoul(q + 1, NULL, 10) : p_begin + 300;
    rt_log("[prof] window %u..%u\n", p_begin, p_end);
    if (!SDL_CreateThread(prof_thread, "prof", NULL)) rt_log("[prof] SDL_CreateThread: %s\n", SDL_GetError());
}
#endif
