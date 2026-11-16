/* Cache of the game's D3DXAssembleShader results (Vita).
 *
 * Loading freeze (2026-11-15, bench loadfreeze/vita-v4): at each stage load the game assembles ~137 vertex/pixel
 * shader sources from text with the statically linked D3DX8 assembler (F_0062ea78, stdcall, 6 arguments), ~0.22 s
 * each in translated code on the Vita: ~30 s without a single Present. The output depends only on (source, flags),
 * so it is cached like the SH4 GXP cache: bundled file in the VPK (app0:/d3dxcache/sh3dxa1.bin), then the writable
 * file in the game folder; a miss assembles normally and appends the result. Entries keep the full source and are
 * matched byte for byte, never by hash alone. Only calls without ppConstants are served from the cache (the ones
 * observed); anything else, or an assembler failure, goes to the original unchanged.
 *
 * File format: "SH3DXA1\0" then records {u32 flags, u32 srclen, u32 codelen, src[srclen], code[codelen]}.
 */
#include "sh4base/d3d8/d3d8.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <SDL.h>

#define DXA_BUNDLE "app0:/d3dxcache/sh3dxa1.bin"
#define DXA_FILE "d3dxcache-sh3dxa1.bin"
#define DXA_KIND 0x7d   /* private ComObj kind for our ID3DXBuffer */

typedef struct { uint32_t flags, srclen, codelen; uint8_t *src, *code; } DxaEntry;
static DxaEntry *dxa; static unsigned dxa_n, dxa_cap, dxa_bundle_n;
static int dxa_loaded;
static unsigned dxa_hits, dxa_misses;
static uint64_t dxa_saved_us;

static void dxa_add(uint32_t flags, const uint8_t *src, uint32_t srclen, const uint8_t *code, uint32_t codelen) {
    if (dxa_n == dxa_cap) {
        unsigned cap = dxa_cap ? dxa_cap * 2 : 256;
        DxaEntry *p = realloc(dxa, cap * sizeof *p);
        if (!p) return;
        dxa = p; dxa_cap = cap;
    }
    DxaEntry *e = &dxa[dxa_n];
    e->src = malloc(srclen ? srclen : 1); e->code = malloc(codelen);
    if (!e->src || !e->code) { free(e->src); free(e->code); return; }
    memcpy(e->src, src, srclen); memcpy(e->code, code, codelen);
    e->flags = flags; e->srclen = srclen; e->codelen = codelen;
    dxa_n++;
}
static const DxaEntry *dxa_find(uint32_t flags, const uint8_t *src, uint32_t srclen) {
    for (unsigned i = 0; i < dxa_n; i++)
        if (dxa[i].flags == flags && dxa[i].srclen == srclen && !memcmp(dxa[i].src, src, srclen)) return &dxa[i];
    return NULL;
}
/* Reads a cache file; stops at the first malformed or truncated record (an interrupted append). */
static unsigned dxa_read(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    char magic[8]; unsigned n = 0;
    if (fread(magic, 1, 8, f) == 8 && !memcmp(magic, "SH3DXA1\0", 8)) {
        uint32_t h[3];
        while (fread(h, 4, 3, f) == 3) {
            if (h[1] > 65536 || !h[2] || h[2] > 262144) break;
            uint8_t *b = malloc(h[1] + h[2]);
            if (!b || fread(b, 1, h[1] + h[2], f) != h[1] + h[2]) { free(b); break; }
            if (!dxa_find(h[0], b, h[1])) { dxa_add(h[0], b, h[1], b + h[1], h[2]); n++; }
            free(b);
        }
    }
    fclose(f);
    return n;
}
static void dxa_load(void) {
    dxa_loaded = 1;
    dxa_bundle_n = dxa_read(DXA_BUNDLE);
    char path[600]; snprintf(path, sizeof path, "%s/%s", g_game_dir, DXA_FILE);
    unsigned w = dxa_read(path);
    rt_log("[d3dx-cache] loaded bundle=%u writable=%u entries\n", dxa_bundle_n, w);
}
static void dxa_append(const DxaEntry *e) {
    char path[600]; snprintf(path, sizeof path, "%s/%s", g_game_dir, DXA_FILE);
    FILE *f = fopen(path, "ab");
    if (!f) { rt_log("[d3dx-cache] cannot open %s for append\n", path); return; }
    if (ftell(f) == 0) fwrite("SH3DXA1\0", 1, 8, f);
    uint32_t h[3] = {e->flags, e->srclen, e->codelen};
    int ok = fwrite(h, 4, 3, f) == 3 && fwrite(e->src, 1, e->srclen, f) == e->srclen && fwrite(e->code, 1, e->codelen, f) == e->codelen;
    if (fclose(f) || !ok) rt_log("[d3dx-cache] write failure\n");
}

/* Minimal ID3DXBuffer: QueryInterface, AddRef, Release, GetBufferPointer, GetBufferSize. */
typedef struct { ComObj com; uint32_t data, size; } DxaBuffer;
static DxaBuffer *dxa_buf(CPU *c) { HOSTFN; ComObj *o = com_get(ARG(1)); return o && o->kind == DXA_KIND ? (DxaBuffer *)o : NULL; }
static void dxa_qi(CPU *c) { HOSTFN; if (ARG(3)) W32(ARG(3), 0); STDRET(3, 0x80004002u); }
static void dxa_addref(CPU *c) { HOSTFN; DxaBuffer *b = dxa_buf(c); if (!b) rt_fatal("[d3dx-cache] AddRef on unknown buffer"); STDRET(1, ++b->com.refs); }
static void dxa_release(CPU *c) {
    HOSTFN; DxaBuffer *b = dxa_buf(c);
    if (!b) rt_fatal("[d3dx-cache] Release on unknown buffer");
    if (--b->com.refs > 0) STDRET(1, b->com.refs);
    uint32_t guest = b->com.guest;
    gheap_free(b->data); com_free_guest(guest); free(b);
    STDRET(1, 0);
}
static void dxa_ptr(CPU *c) { HOSTFN; DxaBuffer *b = dxa_buf(c); if (!b) rt_fatal("[d3dx-cache] GetBufferPointer on unknown buffer"); STDRET(1, b->data); }
static void dxa_size(CPU *c) { HOSTFN; DxaBuffer *b = dxa_buf(c); if (!b) rt_fatal("[d3dx-cache] GetBufferSize on unknown buffer"); STDRET(1, b->size); }
static const gfn dxa_vt[] = {dxa_qi, dxa_addref, dxa_release, dxa_ptr, dxa_size};

static uint32_t dxa_new_buffer(const uint8_t *code, uint32_t size) {
    HOSTFN;
    DxaBuffer *b = calloc(1, sizeof *b);
    uint32_t data = gheap_alloc(size);
    if (!b || !data) { free(b); if (data) gheap_free(data); return 0; }
    memcpy(mb + data, code, size);
    b->data = data; b->size = size;
    return com_wrap(&b->com, DXA_KIND, dxa_vt, 5, "ID3DXBuffer");
}

extern void __real_F_0062ea78(CPU *);
void __wrap_F_0062ea78(CPU *c) {
    HOSTFN;
    uint32_t src = ARG(1), len = ARG(2), flags = ARG(3), ppconst = ARG(4), ppcode = ARG(5), pperr = ARG(6);
    if (!dxa_loaded) dxa_load();
    int cacheable = src && len && len <= 65536 && (uint64_t)src + len <= g_mem_size && !ppconst && ppcode;
    uint64_t t0 = SDL_GetPerformanceCounter();
    if (cacheable) {
        const DxaEntry *e = dxa_find(flags, mb + src, len);
        uint32_t buf = e ? dxa_new_buffer(e->code, e->codelen) : 0;
        if (buf) {
            W32(ppcode, buf);
            if (pperr) W32(pperr, 0);
            dxa_hits++;
            if (dxa_hits == 1 || !(dxa_hits % 50))
                rt_log("[d3dx-cache] hits=%u misses=%u (entries %u, assembly saved ~%llu ms)\n", dxa_hits, dxa_misses, dxa_n, (unsigned long long)(dxa_saved_us / 1000));
            dxa_saved_us += 220000;   /* median assembly time measured on the Vita */
            STDRET(6, 0);
        }
    }
    __real_F_0062ea78(c);
    uint64_t us = (SDL_GetPerformanceCounter() - t0) * 1000000 / SDL_GetPerformanceFrequency();
    if (!cacheable || c->eax || !R32(ppcode)) {
        static unsigned skipped;
        if (skipped++ < 20) rt_log("[d3dx-cache] not cached: cacheable=%d hr=%08x %llu us\n", cacheable, c->eax, (unsigned long long)us);
        return;
    }
    /* Read the guest-created ID3DXBuffer through its own vtable (GetBufferPointer = slot 3, GetBufferSize = 4). */
    uint32_t obj = R32(ppcode), vt = R32(obj), hr = c->eax, ecx = c->ecx, edx = c->edx;
    uint32_t ptr = rt_guest_call(c, R32(vt + 12), 1, obj), size = rt_guest_call(c, R32(vt + 16), 1, obj);
    c->eax = hr; c->ecx = ecx; c->edx = edx;   /* the caller sees exactly the original's return state */
    if (!ptr || !size || size > 262144 || (uint64_t)ptr + size > g_mem_size) return;
    dxa_misses++;
    unsigned before = dxa_n;
    dxa_add(flags, mb + src, len, mb + ptr, size);
    if (dxa_n > before) dxa_append(&dxa[dxa_n - 1]);
    if (dxa_misses <= 3 || !(dxa_misses % 50)) rt_log("[d3dx-cache] miss #%u assembled in %llu us, stored (%u entries)\n", dxa_misses, (unsigned long long)us, dxa_n);
}
