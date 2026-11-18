// OpenGL rendering of the D3D8 state: programs (translated VS / fixed pipeline + texture stages / PS),
// states, attributes from guest memory, render targets (all textures, rendered
// upside down in Y to keep the D3D "row 0 = top" convention), Clear, Present, CopyRects.
#include "d3d8.h"
#include "prep.h"
#include "audit.h"
#include "plat.h"
#include "shader_bank.h"
#include "shader_ready.h"
#include "shader_cache_version.h"
#include "diag_widescreen.h"
#ifdef __vita__
#include <vitaGL.h>
#include <sys/stat.h>
#endif
#include <stdlib.h>
#include <math.h>
#include <stdio.h>
#include <stdarg.h>

#ifndef GL_DEPTH24_STENCIL8
#define GL_DEPTH24_STENCIL8 0x88F0
#endif
#ifndef GL_FRAMEBUFFER
#define GL_FRAMEBUFFER 0x8D40
#define GL_RENDERBUFFER 0x8D41
#define GL_COLOR_ATTACHMENT0 0x8CE0
#define GL_DEPTH_ATTACHMENT 0x8D00
#define GL_STENCIL_ATTACHMENT 0x8D20
#endif

// ---------- programmes ----------
typedef struct Prog {
    uint64_t vkey, fkey;
    uint64_t vsource, fsource;
    unsigned stage_types;   // distinguish shader hashes from fixed-function state
    GLuint prog;
    GLint c, flip, vp, pc, tf, fogc, fogp, aref, s[MAX_STAGES];
    uint32_t vsc_gen, psc_gen, sig[10];   // last values sent (GL uniforms = program state)
    int sent;   // 0: nothing sent yet
    struct Prog *next;
} Prog;
#define PROG_BUCKETS 1024
static Prog *progs[PROG_BUCKETS];
static GLuint g_blit_prog, g_gamma_prog, g_copy_prog, g_readfbo;
static void bind_read(Surface *s);
static GLuint cur_prog;   // dernier glUseProgram
#ifdef SH3_SAMPLER_CACHE_LAB
/* Consumer-only, keyed by the actual GL object (not by the D3D stage).
 * An object can be bound on several stages: the last state applied must win.
 * All out-of-band render helpers invalidate this cache along with GL state. */
typedef struct { GLuint texture; GLint value[4]; unsigned valid; } SamplerMemo;
static SamplerMemo sampler_memo[64];
static unsigned sampler_saved, sampler_sent;
static void sampler_invalidate(void) {
    for (unsigned i=0;i<64;i++) sampler_memo[i].valid=0;
}
#endif
static int fx_route(Device *,uint32_t,uint32_t,uint32_t,const void *,const uint8_t *,uint32_t,const uint8_t *const *);
extern unsigned g_diag_perf;
#ifdef __vita__
int g_glstatecache_ab, g_glstatecache_phase; /* opt-in same-process lab; phase uses atomic access */
static int fx_active;
static Device fx_device;
static GLuint fx_fbo,fx_texture;
static unsigned fx_width,fx_height;
static unsigned fx_closed_frame=~0u,fx_draws,fx_batches;
#endif

/* Exact state memoization, consumer-owned. Keep every render-state word so a
 * future apply_states extension cannot silently omit a newly read rs entry.
 * Callbacks and framebuffer operations invalidate the snapshot below. */
#ifdef __vita__
static uint32_t state_cache_rs[256],state_cache_vp[6];
_Static_assert(sizeof state_cache_rs==sizeof ((Device *)0)->rs,"render state snapshot size");
_Static_assert(sizeof state_cache_vp==sizeof ((Device *)0)->vp,"viewport snapshot size");
static unsigned state_cache_shift,state_cache_depth,state_hits,state_misses;
static int state_cache_valid;
static int state_cache_matches(Device *d,unsigned shift) {
    if (!isfinite(d->vp.minz) || !isfinite(d->vp.maxz) ||
        d->vp.w>2048u || d->vp.h>2048u) {
        state_cache_valid=0; ++state_misses; return 0;
    }
    unsigned depth=d->ds!=NULL;
    if (state_cache_valid && state_cache_shift==shift && state_cache_depth==depth &&
        !memcmp(state_cache_rs,d->rs,sizeof state_cache_rs) &&
        !memcmp(state_cache_vp,&d->vp,sizeof state_cache_vp)) {
        ++state_hits; return 1;
    }
    memcpy(state_cache_rs,d->rs,sizeof state_cache_rs);
    memcpy(state_cache_vp,&d->vp,sizeof state_cache_vp);
    state_cache_shift=shift;state_cache_depth=depth;state_cache_valid=1;
    ++state_misses;return 0;
}
#endif
void gfx_state_invalidate(void) {
#ifdef SH3_SAMPLER_CACHE_LAB
    sampler_invalidate();
#endif
#ifdef __vita__
    state_cache_valid=0;
#endif
}

// OpenGL's missing-location sentinel is -1. vitaGL's fast uniform handles
// encode pointers, so valid locations can have either sign depending on the pool.
static int uniform_valid(GLint location) { return location != -1; }

static char *strf(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    char *s = malloc((size_t)n + 1);
    va_start(ap, fmt);
    vsnprintf(s, (size_t)n + 1, fmt, ap);
    va_end(ap);
    return s;
}
static void cat(char **s, const char *add) {
    size_t a = *s ? strlen(*s) : 0, b = strlen(add);
    *s = realloc(*s, a + b + 1);
    memcpy(*s + a, add, b + 1);
}
static void catf(char **s, const char *fmt, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    cat(s, buf);
}

#ifdef __vita__
#define GLSL_PRE_VS ""
#define GLSL_PRE_FS "precision mediump float;\n"
#else
#define GLSL_PRE_VS "#version 120\n"
#define GLSL_PRE_FS "#version 120\n"
#endif

static GLuint compile(GLenum type, const char *src) {
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, NULL);
    glCompileShader(s);
    GLint ok = 0;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[2048];
        glGetShaderInfoLog(s, sizeof log, NULL, log);
        rt_log("[gl] compile failure:\n%s\n--- source ---\n%s\n", log, src);
    }
    return s;
}
static void bind_attrs(GLuint p) {
    char name[8];
    for (int i = 0; i < 16; i++) { snprintf(name, sizeof name, "aV%d", i); glBindAttribLocation(p, (GLuint)i, name); }
}

#ifdef __vita__
// On-disk program cache: shark takes ~0.5 s per VS+FS pair (hitches in game). The pair is
// compiled together (varying linkage), so the pair is cached: shc/<hash>.bin = [n][VS][n][FS].
const char *sh4_shader_bundle_stage_root(void) {
    return SH4_SHADER_BUNDLE_ROOT "/" SH4_SHADER_CACHE_ABI "/stages";
}
typedef struct ShcHeader {
    char magic[8];
    uint32_t version, bytes;
    uint64_t checksum;
} ShcHeader;
static uint64_t shc_checksum(const void *data, size_t n) {
    const uint8_t *p = data;
    uint64_t h = 0xcbf29ce484222325ull;
    for (size_t i = 0; i < n; i++) h = (h ^ p[i]) * 0x100000001b3ull;
    return h;
}
static int shc_write_file(FILE *f, const void *data, size_t n) {
    if (!f) return 0;
    ShcHeader header = {{'S','H','4','G','X','P','1',0}, 1, (uint32_t)n, shc_checksum(data, n)};
    int ok = fwrite(&header, 1, sizeof header, f) == sizeof header && fwrite(data, 1, n, f) == n;
    return fclose(f) == 0 && ok;
}
static uint64_t shc_key(const char *vs, const char *fs) {
    uint64_t h = 0xcbf29ce484222325ull;
    for (const char *s = vs; *s; s++) h = (h ^ (uint8_t)*s) * 0x100000001b3ull;
    h = (h ^ 0xff) * 0x100000001b3ull;
    for (const char *s = fs; *s; s++) h = (h ^ (uint8_t)*s) * 0x100000001b3ull;
    return h;
}
static int shc_opt(void) {
    const char *opt = getenv("SH4_SHADER_OPT");
    return opt && opt[0] >= '1' && opt[0] <= '3' && opt[1] == 0 ? opt[0] - '0' : 3;
}
static FILE *shc_open_bundle(const char *vs, const char *fs) {
    const char *enabled = getenv("SH4_BUNDLED_CACHE");
    if ((enabled && !strcmp(enabled, "0")) || shc_opt() != 3) return NULL;
    char path[600];
    snprintf(path, sizeof path, SH4_SHADER_BUNDLE_ROOT "/" SH4_SHADER_CACHE_ABI "/programs/%016llx.bin",
             (unsigned long long)shc_key(vs, fs));
    return fopen(path, "rb");
}
static FILE *shc_open(const char *vs, const char *fs, const char *mode) {
    static char directory[48];
    if (!directory[0]) {
        const char *test = getenv("SH4_SHADER_TEST");
        int level = shc_opt();
        unsigned trial = 0;
        if (test && *test) {
            const char *p = test;
            for (; *p >= '0' && *p <= '9' && trial < 100000; p++) trial = trial * 10 + (unsigned)(*p - '0');
            if (*p || trial >= 1000000) trial = 0;
        }
        if (trial) snprintf(directory, sizeof directory, "shc-" SH4_SHADER_CACHE_ABI "-test%u-o%d", trial, level);
        else if (level != 3) snprintf(directory, sizeof directory, "shc-" SH4_SHADER_CACHE_ABI "-o%d", level);
        else strcpy(directory, "shc-" SH4_SHADER_CACHE_ABI);
        if (trial || level != 3) rt_log("[shader-cache] directory=%s\n", directory);
    }
    char path[600];
    snprintf(path, sizeof path, "%s/%s", g_game_dir, directory);
    if (*mode == 'w') mkdir(path, 0777);
    snprintf(path, sizeof path, "%s/%s/%016llx.bin", g_game_dir, directory, (unsigned long long)shc_key(vs, fs));
    return fopen(path, mode);
}
static int shc_load_file(GLuint *program, FILE *f) {
    GLuint p = *program;
    if (!f) return 0;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    ShcHeader header;
    int ok = n > (long)sizeof header + 8 && n <= 2 * 1024 * 1024 &&
        fread(&header, 1, sizeof header, f) == sizeof header &&
        !memcmp(header.magic, "SH4GXP1\0", 8) && header.version == 1 &&
        header.bytes == (size_t)n - sizeof header;
    size_t bytes = ok ? header.bytes : 0;
    uint8_t *b = bytes ? malloc(bytes) : NULL;
    ok = b && fread(b, 1, bytes, f) == bytes && shc_checksum(b, bytes) == header.checksum;
    fclose(f);
    n = (long)bytes;
    uint32_t vl = 0, fl = 0;
    if (ok) memcpy(&vl, b, 4);
    // Validate using subtraction before forming any file offset. A corrupt signed
    // length must never turn into a wrapped offset into the shader byte stream.
    ok = ok && vl > 0 && (size_t)vl < (size_t)n - 8;
    if (ok) memcpy(&fl, b + 4 + vl, 4);
    ok = ok && fl > 0 && (size_t)fl == (size_t)n - 8 - vl;   // truncated file: recompile
    if (ok) {
        GLuint v = glCreateShader(GL_CG_VERTEX_SHADER_EXT), fr = glCreateShader(GL_CG_FRAGMENT_SHADER_EXT);
        glShaderBinary(1, &v, 0, b + 4, (GLsizei)vl);
        glShaderBinary(1, &fr, 0, b + 8 + vl, (GLsizei)fl);
        glAttachShader(p, v);
        glAttachShader(p, fr);
        bind_attrs(p);
        glLinkProgram(p);
        GLint linked = 0;
        glGetProgramiv(p, GL_LINK_STATUS, &linked);
        ok = linked != 0;
        glDeleteShader(v);
        glDeleteShader(fr);
        if (!ok) {
            // vitaGL has no glDetachShader. Recreate the failed program so the
            // normal source fallback starts with clean shader and attribute state.
            glDeleteProgram(p);
            *program = glCreateProgram();
            rt_log("[shader-cache] cached program failed to link; recompiling source\n");
        }
    }
    free(b);
    return ok;
}
static int shc_load(GLuint *program, const char *vs, const char *fs) {
    if (shc_load_file(program, shc_open_bundle(vs, fs))) {
        rt_log("[shader-cache] hit bundle abi=" SH4_SHADER_CACHE_ABI "\n");
        return 1;
    }
    if (shc_load_file(program, shc_open(vs, fs, "rb"))) {
        rt_log("[shader-cache] hit writable abi=" SH4_SHADER_CACHE_ABI "\n");
        return 1;
    }
    return 0;
}
static void shc_save(GLuint p, GLuint v, GLuint fr, const char *vs, const char *fs) {
    GLint len = 0;
    glGetProgramiv(p, GL_PROGRAM_BINARY_LENGTH, &len);   // bound: VS + FS + headers
    uint8_t *b = len > 0 ? malloc((size_t)len + 8) : NULL;
    if (!b) return;
    GLsizei vl = 0, fl = 0;
    vglGetShaderBinary(v, len, &vl, b + 4);
    if (vl <= 0 || vl >= len) { free(b); return; }
    vglGetShaderBinary(fr, len - vl, &fl, b + 8 + vl);
    if (fl <= 0 || fl > len - vl) { free(b); return; }
    memcpy(b, &vl, 4);
    memcpy(b + 4 + vl, &fl, 4);
    FILE *f = shc_open(vs, fs, "wb");
    if (f && !shc_write_file(f, b, 8 + (size_t)vl + (size_t)fl))
        rt_log("[shader-cache] writable cache write failure\n");
    free(b);
}
#endif

uint64_t g_t_link, g_t_tex;   // diagnostics: cumulative link / texture upload time (long frames)
static GLuint link_(const char *vs, const char *fs, int prepare);
static GLuint link(const char *vs, const char *fs) {
    uint64_t t0 = SDL_GetPerformanceCounter();
    GLuint p = link_(vs, fs, 0);
    g_t_link += SDL_GetPerformanceCounter() - t0;
    return p;
}
static GLuint link_(const char *vs, const char *fs, int prepare) {
    static unsigned s_link_n;
    uint64_t t0 = SDL_GetPerformanceCounter();
    GLuint p = glCreateProgram();
#ifdef __vita__
    if (!prepare && shc_load(&p, vs, fs)) {
        rt_log("[link] #%u from cache, %u ms\n", ++s_link_n, (unsigned)((SDL_GetPerformanceCounter() - t0) * 1000 / SDL_GetPerformanceFrequency()));
        return p;
    }
#endif
    GLuint v = compile(GL_VERTEX_SHADER, vs), f = compile(GL_FRAGMENT_SHADER, fs);
    glAttachShader(p, v);
    glAttachShader(p, f);
    bind_attrs(p);
    glLinkProgram(p);
    GLint ok = 0;
    glGetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[2048];
        glGetProgramInfoLog(p, sizeof log, NULL, log);
        rt_log("[gl] link failure: %s\n", log);
    }
#ifdef __vita__
    else if (!prepare) shc_save(p, v, f, vs, fs);
#endif
    rt_log("[link] #%u compiled, %u ms\n", ++s_link_n, (unsigned)((SDL_GetPerformanceCounter() - t0) * 1000 / SDL_GetPerformanceFrequency()));
    glDeleteShader(v);
    glDeleteShader(f);
    return p;
}

#ifdef __vita__
// Optional prototype: prepare static vertex stages before entering game code.
// A separate vitaGL cache of exact translated Cg stages is required; a program
// linked against a dummy fragment stage must never replace a game's program.
extern uint32_t sh4_cg_cache_revision(void) __attribute__((weak));
extern uint32_t sh4_cg_cache_failures(void) __attribute__((weak));
extern uint64_t sh4_cg_cache_config(void) __attribute__((weak));
extern int sh4_cg_cache_validate(const char *) __attribute__((weak));
extern void sh4_cg_cache_observe(void (*)(const char *)) __attribute__((weak));
static ShaderReady *prewarm_files;
static void prewarm_observe(const char *path) { shader_ready_add(prewarm_files, path); }
int gfx_prewarm_shader_bank(const char *path, void (*progress)(unsigned, unsigned)) {
    HOSTFN;
    if (!sh4_cg_cache_revision || !sh4_cg_cache_failures) {
        rt_log("[prewarm] unavailable: compiled-stage cache not linked\n");
        return -1;
    }
    ShaderBank bank;
    int status = shader_bank_load(path, &bank);
    if (status) { rt_log("[prewarm] bank unavailable or invalid (%d); using normal runtime\n", status); return status; }
    uint64_t start = SDL_GetPerformanceCounter();
    char ready_path[600];
    snprintf(ready_path, sizeof ready_path, "%s/shader-ready.bin", g_game_dir);
    int persistent = sh4_cg_cache_config && sh4_cg_cache_validate && sh4_cg_cache_observe;
    uint64_t config = persistent ? sh4_cg_cache_config() : 0;
    if (persistent && shader_ready_check(ready_path, bank.checksum, config, sh4_cg_cache_validate)) {
        rt_log("[prewarm] persistent cache verified, no preparation needed; %u ms\n",
            (unsigned)((SDL_GetPerformanceCounter() - start) * 1000 / SDL_GetPerformanceFrequency()));
        shader_bank_free(&bank);
        return 0;
    }
    // Check the bank against the executable actually loaded in guest memory.
    for (unsigned i = 0; i < bank.count; i++) {
        if (R32(bank.entries[i].func) != 0xfffe0101u) {
            rt_log("[prewarm] bank/executable mismatch\n");
            shader_bank_free(&bank);
            return -2;
        }
    }
    static const char fs[] = GLSL_PRE_FS
        "varying vec4 vD0;\nvarying vec4 vD1;\nvarying vec4 vT0;\nvarying vec4 vT1;\n"
        "varying vec4 vT2;\nvarying vec4 vT3;\nvarying float vFog;\n"
        "void main() { gl_FragColor = vec4(1.0); }\n";
    uint32_t failures = sh4_cg_cache_failures();
    prewarm_files = persistent ? calloc(1, sizeof *prewarm_files) : NULL;
    if (prewarm_files) sh4_cg_cache_observe(prewarm_observe);
    rt_log("[prewarm] begin count=%u bank=%016llx backend=%08x\n", bank.count,
           (unsigned long long)bank.checksum, sh4_cg_cache_revision());
    int saved_queue = q_on;
    q_on = 0; // startup, before the game or render thread owns any GL objects
    for (unsigned i = 0; i < bank.count; i++) {
        if (progress) progress(i, bank.count);
        // Always visit the stage cache when rebuilding the readiness manifest.
        // A cached complete dummy program could otherwise hide a deleted stage.
        GLuint p = link_(bank.entries[i].source, fs, 1);
        GLint ok = 0;
        glGetProgramiv(p, GL_LINK_STATUS, &ok);
        glDeleteProgram(p);
        if (!ok) { status = -3; break; }
        rt_log("[prewarm] ready %u/%u func=%08x\n", i + 1, bank.count, bank.entries[i].func);
    }
    if (sh4_cg_cache_failures() != failures) status = -4;
    if (progress) progress(bank.count, bank.count);
    glReleaseShaderCompiler(); // preparation is complete; retain only compiled binaries
    if (prewarm_files) {
        sh4_cg_cache_observe(NULL);
        int saved = !status && prewarm_files->count >= bank.count &&
            shader_ready_save(ready_path, bank.checksum, config, prewarm_files);
        rt_log("[prewarm] persistent manifest saved=%d files=%u\n", saved, prewarm_files->count);
        if (!saved && !status) status = -5;
        free(prewarm_files);
        prewarm_files = NULL;
    }
    q_on = saved_queue;
    rt_log("[prewarm] end status=%d elapsed=%u ms\n", status,
        (unsigned)((SDL_GetPerformanceCounter() - start) * 1000 / SDL_GetPerformanceFrequency()));
    shader_bank_free(&bank);
    return status;
}
#endif

// ---------- pipeline fixe : sommets (FVF) ----------
static char *ffp_vs(uint32_t fvf) {
    uint32_t pos = fvf & 0xe, ntex = (fvf >> 8) & 0xf;
    char *s = strf(GLSL_PRE_VS "attribute vec4 aV0;\n%s%s", fvf & 0x40 ? "attribute vec4 aV5;\n" : "",
                   fvf & 0x80 ? "attribute vec4 aV6;\n" : "");
    for (uint32_t i = 0; i < ntex && i < 4; i++) catf(&s, "attribute vec4 aV%u;\n", 7 + i);
    cat(&s, "uniform vec4 uVp;\nuniform vec4 uFlip;\nuniform vec4 c[4];\n"
            "varying vec4 vD0;\nvarying vec4 vD1;\nvarying vec4 vT0;\nvarying vec4 vT1;\nvarying vec4 vT2;\nvarying vec4 vT3;\nvarying float vFog;\nvoid main() {\n");
    if (pos == 0x4) // XYZRHW: screen coordinates (D3D pixel centre = integer)
        cat(&s, "  float w = aV0.w == 0.0 ? 1.0 : 1.0 / aV0.w;\n"
                "  gl_Position = vec4(((aV0.x + 0.5 - uVp.x) / uVp.z) * 2.0 - 1.0,\n"
                "                     (1.0 - ((aV0.y + 0.5 - uVp.y) / uVp.w) * 2.0) * uFlip.x,\n"
                "                     clamp(aV0.z, 0.0, 1.0) * 2.0 - 1.0, 1.0) * w;\n");   // D3D clamps RHW depth to [0,1]; the map's player arrow has z=1.00001 and GL clipped it
    else // XYZ: world*view*projection (rows of c[0..3], D3D row-vector convention)
        cat(&s, "  vec4 p = vec4(aV0.xyz, 1.0);\n"
                "  vec4 q = vec4(dot(p, c[0]), dot(p, c[1]), dot(p, c[2]), dot(p, c[3]));\n"
                "  gl_Position = vec4(q.x, q.y * uFlip.x, q.z * 2.0 - q.w, q.w);\n");
    catf(&s, "  vD0 = %s;\n  vD1 = %s;\n", fvf & 0x40 ? "aV5.zyxw" : "vec4(1.0)", fvf & 0x80 ? "aV6.zyxw" : "vec4(0.0)");
    cat(&s, "  vFog = vD1.w;\n");   // FFP vertex fog = specular alpha
    for (uint32_t i = 0; i < 4; i++) catf(&s, "  vT%u = %s;\n", i, i < ntex ? (i == 0 ? "aV7" : i == 1 ? "aV8" : i == 2 ? "aV9" : "aV10") : "vec4(0.0, 0.0, 0.0, 1.0)");
    cat(&s, "}\n");
    return s;
}

// ---------- fixed pipeline: texture stages ----------
static void arg_expr(char *out, size_t n, uint32_t a, int s, const char *cur) {
    const char *base;
    char tx[16];
    switch (a & 0xf) {
    case 0: base = "vD0"; break;
    case 1: base = s == 0 ? "vD0" : cur; break;
    case 2: snprintf(tx, sizeof tx, "tx%d", s); base = tx; break;
    case 3: base = "uTF"; break;
    case 4: base = "vD1"; break;
    case 5: base = "tmp"; break;
    default: base = "vec4(1.0)"; break;
    }
    char b[64];
    snprintf(b, sizeof b, (a & 0x20) ? "%s.wwww" : "%s", base);
    snprintf(out, n, (a & 0x10) ? "(1.0 - %s)" : "%s", b);
}
static void op_expr(char **s, uint32_t op, const char *a1, const char *a2, const char *a0, int st) {
    switch (op) {
    case 2: catf(s, "%s", a1); break;
    case 3: catf(s, "%s", a2); break;
    case 4: catf(s, "%s * %s", a1, a2); break;
    case 5: catf(s, "%s * %s * 2.0", a1, a2); break;
    case 6: catf(s, "%s * %s * 4.0", a1, a2); break;
    case 7: catf(s, "%s + %s", a1, a2); break;
    case 8: catf(s, "%s + %s - 0.5", a1, a2); break;
    case 9: catf(s, "(%s + %s - 0.5) * 2.0", a1, a2); break;
    case 10: catf(s, "%s - %s", a1, a2); break;
    case 11: catf(s, "%s + %s - %s * %s", a1, a2, a1, a2); break;
    case 12: catf(s, "mix(%s, %s, vD0.w)", a2, a1); break;
    case 13: catf(s, "mix(%s, %s, tx%d.w)", a2, a1, st); break;
    case 14: catf(s, "mix(%s, %s, uTF.w)", a2, a1); break;
    case 15: catf(s, "%s + %s * (1.0 - tx%d.w)", a1, a2, st); break;
    case 16: catf(s, "mix(%s, %s, cur.w)", a2, a1); break;
    case 18: catf(s, "vec4(%s.xyz + %s.w * %s.xyz, %s.w)", a1, a1, a2, a1); break;
    case 19: catf(s, "vec4(%s.xyz * %s.xyz + %s.w, %s.w)", a1, a2, a1, a1); break;
    case 20: catf(s, "vec4((1.0 - %s.w) * %s.xyz + %s.xyz, %s.w)", a1, a2, a1, a1); break;
    case 21: catf(s, "vec4((1.0 - %s.xyz) * %s.xyz + %s.w, %s.w)", a1, a2, a1, a1); break;
    case 24: catf(s, "vec4(dot((%s.xyz - 0.5) * 2.0, (%s.xyz - 0.5) * 2.0))", a1, a2); break;
    case 25: catf(s, "%s + %s * %s", a0, a1, a2); break;
    case 26: catf(s, "mix(%s, %s, %s)", a2, a1, a0); break;
    default: catf(s, "%s", a1); break; // PREMODULATE, BUMPENV* : approximation
    }
}

// 64-bit key of the stage state (fixed pipeline) + fragment options.
static uint64_t mix64(uint64_t h, uint64_t v) { h ^= v + 0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2); return h; }
static int tci_of(Device *d, int s) { return (int)(d->tss[s][11] & 3); }
static uint64_t ffp_fkey(Device *d) {
    uint64_t h = 0x1234;
    for (int s = 0; s < MAX_STAGES; s++) {
        uint32_t *t = d->tss[s];
        h = mix64(h, t[1] | (uint64_t)t[2] << 8 | (uint64_t)t[3] << 16 | (uint64_t)t[26] << 24 | (uint64_t)t[28] << 32);
        h = mix64(h, t[4] | (uint64_t)t[5] << 8 | (uint64_t)t[6] << 16 | (uint64_t)t[27] << 24 |
                     (uint64_t)tci_of(d, s) << 32 | (uint64_t)(d->tex[s] != NULL || d->cube[s] != NULL) << 36 |
                     (uint64_t)(d->cube[s] != NULL) << 37);
        if (t[1] == 1) break;
    }
    return h;
}
static char *ffp_fs_body(Device *d) {
    char *s = strf("  vec4 cur = vD0;\n  vec4 tmp = vec4(0.0);\n");
    for (int st = 0; st < MAX_STAGES; st++) {
        uint32_t *t = d->tss[st];
        uint32_t cop = t[1], aop = t[4];
        if (cop == 1) break;
        int usetex = d->tex[st] != NULL || d->cube[st] != NULL;
        catf(&s, "  vec4 tx%d = %s;\n", st, usetex ? "" : "vec4(1.0)");
        if (usetex) {
            size_t n = strlen(s);
            s[n - 2] = 0; // removes the trailing semicolon and newline to complete the expression
            catf(&s, "tex%d(vT%d);\n", st, tci_of(d, st));
        }
        char c1[48], c2[48], c0[48], a1[48], a2[48], a0[48];
        arg_expr(c1, sizeof c1, t[2], st, "cur");
        arg_expr(c2, sizeof c2, t[3], st, "cur");
        arg_expr(c0, sizeof c0, t[26], st, "cur");
        arg_expr(a1, sizeof a1, t[5], st, "cur");
        arg_expr(a2, sizeof a2, t[6], st, "cur");
        arg_expr(a0, sizeof a0, t[27], st, "cur");
        const char *dst = t[28] == 5 ? "tmp" : "cur";
        catf(&s, "  { vec4 rc = clamp(");
        op_expr(&s, cop, c1, c2, c0, st);
        cat(&s, ", 0.0, 1.0);\n    vec4 ra = clamp(");
        if (aop == 1) cat(&s, st == 0 ? "vD0" : "cur");
        else op_expr(&s, aop, a1, a2, a0, st);
        catf(&s, ", 0.0, 1.0);\n    %s = vec4(rc.xyz, ra.w); }\n", dst);
    }
    cat(&s, "  col = cur;\n");
    return s;
}

// ---------- fragment : assemblage ----------
static unsigned ctl_nofog;   // ctl.txt nofog=1: fog disabled in every fragment shader (diagnostic)
static uint64_t frag_opts(Device *d) {
    uint32_t *rs = d->rs;
    uint64_t o = 0;
    if (rs[15]) o |= (uint64_t)(rs[25] & 0xf) << 0;            // ALPHATEST + function
    if (rs[28] && !ctl_nofog) {
        static int audit_nofog = -1;
        if (audit_nofog < 0) {
            audit_nofog = getenv("SH3_AUDIT_NOFOG") != NULL;
            if (audit_nofog) rt_log("[audit-fog] first fogged draw changed\n");
        }
        if (!audit_nofog) o |= (uint64_t)(1 + (rs[35] & 3)) << 4;
    }
    if (rs[29]) o |= 1u << 8;                                  // SPECULARENABLE
    if (d->sh3_fadebox) o |= 1u << 9 | (uint64_t)d->sh3_fadebox << 16;   // transition half-texel copy: exact 2x2 box
    return o;
}
static char *build_fs(Device *d, const char *body, uint64_t opts) {
    char *s = strf(GLSL_PRE_FS "varying vec4 vD0;\nvarying vec4 vD1;\nvarying vec4 vT0;\nvarying vec4 vT1;\nvarying vec4 vT2;\nvarying vec4 vT3;\nvarying float vFog;\n"
                   "uniform vec4 pc[8];\nuniform vec4 uTF;\nuniform vec4 uFogC;\nuniform vec4 uFogP;\nuniform float uARef;\n");
    for (int i = 0; i < MAX_STAGES; i++) {
        if (i == 0 && (opts >> 9 & 1)) {
            /* Transition RT copy (quad 0..W, UV 0..1, see device.c): D3D samples texel corners, which made the
               image blur and slide half a pixel per frame (and grow blocks on Vita). Valentin's decision
               2026-11-15: the transition must only darken, the picture must not move. So each pixel reads its
               own texel centre (+half texel, or -half on a mirrored axis) with a POINT sampler: an exact copy;
               the darkening of the following screen pass is untouched. */
            uint32_t fb = (uint32_t)(opts >> 16);
            unsigned bw = fb >> 16 & 0xffff, bh = fb >> 2 & 0x3fff;
            catf(&s, "uniform sampler2D s0;\nvec4 tex0(vec4 c) { return texture2D(s0, c.xy + vec2(%.9f, %.9f)); }\n",
                 (fb >> 1 & 1 ? 0.5 : -0.5) / bw, (fb & 1 ? 0.5 : -0.5) / bh);
            continue;
        }
        if (d->cube[i]) catf(&s, "uniform samplerCube s%d;\nvec4 tex%d(vec4 c) { return textureCube(s%d, c.xyz); }\n",i,i,i);
        else catf(&s, "uniform sampler2D s%d;\nvec4 tex%d(vec4 c) { return texture2D(s%d, c.xy); }\n",i,i,i);
    }
    cat(&s, "void main() {\n  vec4 col;\n");
    cat(&s, body);
    if (opts & (1u << 8)) cat(&s, "  col.xyz = clamp(col.xyz + vD1.xyz, 0.0, 1.0);\n");
    static const char *cmp[] = {"", "false", "a < r", "a == r", "a <= r", "a > r", "a != r", "a >= r", "true"};
    uint32_t af = opts & 0xf;
    if (af && af < 8)
        catf(&s, "  { float a = floor(col.w * 255.0 + 0.5); float r = uARef; if (!(%s)) discard; }\n", cmp[af]);
    uint32_t fog = (opts >> 4) & 0xf;
    if (fog) {
        if (fog == 1) cat(&s, "  float f = vFog;\n");
        else {
            cat(&s, "  float z = 1.0 / gl_FragCoord.w;\n");
            if (fog == 2) cat(&s, "  float f = exp(-uFogP.z * z);\n");
            else if (fog == 3) cat(&s, "  float f = exp(-(uFogP.z * z) * (uFogP.z * z));\n");
            else cat(&s, "  float f = (uFogP.y - z) / (uFogP.y - uFogP.x);\n");
        }
        cat(&s, "  col.xyz = mix(uFogC.xyz, col.xyz, clamp(f, 0.0, 1.0));\n");
    }
    const char *dbg = getenv("SH4_FSDEBUG");   // uv | diffuse | tex: isolates one rendering step
    if (dbg && !strcmp(dbg, "uv")) cat(&s, "  col = vec4(vT0.xy, 0.0, 1.0);\n");
    if (dbg && !strcmp(dbg, "diffuse")) cat(&s, "  col = vec4(vD0.xyz, 1.0);\n");
    if (dbg && !strcmp(dbg, "tex")) cat(&s, "  col = vec4(texture2D(s0, vT0.xy).xyz, 1.0);\n");
    cat(&s, "  gl_FragColor = col;\n}\n");
    return s;
}

static Prog *get_prog(Device *d, VShader *vs, uint32_t fvf, PShader *ps) {
    uint64_t opts = frag_opts(d);
    // Shader objects and handles are recycled after cutscenes/death. Their host
    // addresses are not program identities: reuse by translated source instead.
    uint64_t vkey = vs ? vs->source_hash : (0x8000000000000000ull | fvf);
    uint64_t fkey = mix64(ps ? ps->source_hash : ffp_fkey(d), opts);
    for (int s=0;s<MAX_STAGES;s++) fkey=mix64(fkey,d->cube[s]!=NULL);
    unsigned stage_types = (vs != NULL) | (unsigned)(ps != NULL) << 1;
    uint32_t bucket = (uint32_t)((vkey * 31 + fkey) % PROG_BUCKETS);
    for (Prog *p = progs[bucket]; p; p = p->next)
        if (p->vkey == vkey && p->fkey == fkey && p->stage_types == stage_types) {
            if (g_audit_checks && (p->vsource != (vs ? vs->source_hash : 0) || p->fsource != (ps ? ps->source_hash : 0)))
                rt_fatal("[audit-error] frame=%u program cache aliases a different shader after resource reuse", g_pframe);
            return p;
        }
    char *vsrc = vs ? strf(GLSL_PRE_VS "%s", vs->glsl) : ffp_vs(fvf);
    char *body = ps ? strf("%s", ps->glsl_body) : ffp_fs_body(d);
    char *fsrc = build_fs(d, body, opts);
    extern unsigned g_stat_progs;
    g_stat_progs++;
    if (g_stat_progs <= 3 || getenv("SH3_DIAG_FADETRACE")) rt_log("[glsl] prog #%u vs=%08x ps=%x\n--- VS ---\n%s--- FS ---\n%s--- fin ---\n",
                                  g_stat_progs, vs ? vs->handle : (uint32_t)(vkey & 0xffffffffu),
                                  ps ? ps->handle : 0, vsrc, fsrc);
    Prog *p = calloc(1, sizeof *p);
    p->vkey = vkey;
    p->fkey = fkey;
    p->stage_types = stage_types;
    p->vsource = vs ? vs->source_hash : 0;
    p->fsource = ps ? ps->source_hash : 0;
    p->prog = link(vsrc, fsrc);
    free(vsrc); free(body); free(fsrc);
    glUseProgram(p->prog);
    cur_prog = p->prog;
    p->c = glGetUniformLocation(p->prog, "c");
    p->flip = glGetUniformLocation(p->prog, "uFlip");
    p->vp = glGetUniformLocation(p->prog, "uVp");
    p->pc = glGetUniformLocation(p->prog, "pc");
    p->tf = glGetUniformLocation(p->prog, "uTF");
    p->fogc = glGetUniformLocation(p->prog, "uFogC");
    p->fogp = glGetUniformLocation(p->prog, "uFogP");
    p->aref = glGetUniformLocation(p->prog, "uARef");
    char name[8];
    for (int i = 0; i < MAX_STAGES; i++) {
        snprintf(name, sizeof name, "s%d", i);
        p->s[i] = glGetUniformLocation(p->prog, name);
#ifdef __vita__
        if (p->s[i] < -1) rt_log("[uniform-location] program=%u sampler=%d location=%08x valid signed handle\n", p->prog, i, (unsigned)p->s[i]);
#endif
        if (uniform_valid(p->s[i])) glUniform1i(p->s[i], i);
    }
#ifdef __vita__
    if (p->c < -1 || p->flip < -1 || p->pc < -1 || p->tf < -1 || p->fogc < -1 || p->fogp < -1 || p->aref < -1)
        rt_log("[uniform-location] program=%u c=%08x flip=%08x pc=%08x tf=%08x fogc=%08x fogp=%08x aref=%08x\n",
            p->prog, (unsigned)p->c, (unsigned)p->flip, (unsigned)p->pc, (unsigned)p->tf, (unsigned)p->fogc, (unsigned)p->fogp, (unsigned)p->aref);
#endif
    p->next = progs[bucket];
    progs[bucket] = p;
    return p;
}

// ---------- cibles de rendu ----------
int g_att_stale;   // incremented when a GL texture/buffer is deleted: GL names may be reused
#ifdef __vita__
#define RT_MAX 1024
#else
#define RT_MAX 65536
#endif
unsigned rt_shift(const Surface *s) {
    uint32_t w = s->tex ? s->tex->w : s->w, h = s->tex ? s->tex->h : s->h;
    unsigned limit = RT_MAX;
#ifdef __vita__
    /* Diagnostic quality/cost probe. Reuse the existing target/viewport/copy
       scaling; guest dimensions and the 960x544 display remain unchanged. */
    static unsigned vita_limit;
    if (!vita_limit) {
        const char *value=getenv("SH3_DIAG_RT512");
        vita_limit=value && !strcmp(value,"1") ? 512u : RT_MAX;
    }
    limit=vita_limit;
#endif
#ifndef __vita__
    // Diagnostic parity with Vita's capped offscreen targets; normal PC rendering stays full-size.
    static unsigned pc_limit;
    if (!pc_limit) {
        const char *value = getenv("SH4_RTMAX");
        char *end = NULL;
        unsigned long requested = value ? strtoul(value, &end, 10) : 0;
        pc_limit = value && end != value && !*end && requested >= 1 && requested <= RT_MAX ? (unsigned)requested : RT_MAX;
    }
    limit = pc_limit;
#endif
    unsigned sh = 0;
    while ((w >> sh) > limit || (h >> sh) > limit) sh++;
    return sh;
}
static GLuint color_tex(Surface *s, int *level) {
    if (s->tex) { *level = s->level; return tex_gl(s->tex); }
    *level = 0;
    if (!s->fbo_tex) {
        rt_log("[rt] color %ux%u fmt=%u\n", s->w, s->h, s->fmt);
        glGenTextures(1, &s->fbo_tex);
        glBindTexture(GL_TEXTURE_2D, s->fbo_tex);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, (GLsizei)(s->w >> rt_shift(s)), (GLsizei)(s->h >> rt_shift(s)), 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    }
    return s->fbo_tex;
}
static GLuint depth_rb(Surface *s) {
    if (!s->rb) {
        rt_log("[rt] depth %ux%u\n", s->w, s->h);
        glGenRenderbuffers(1, &s->rb);
        glBindRenderbuffer(GL_RENDERBUFFER, s->rb);
        glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, (GLsizei)(s->w >> rt_shift(s)), (GLsizei)(s->h >> rt_shift(s)));
    }
    return s->rb;
}
int surf_is_gl(const Surface *s) { return s->is_backbuffer || (s->usage & 1) || (s->tex && (s->tex->usage & 1)); }

/* Lazy depth attachment (env SH3_LAZYDEPTH, marker lazydepth): a target is bound without its depth/stencil
   renderbuffer; the renderbuffer is attached before the first draw/clear that tests, writes or clears depth or
   stencil. Passes that never touch depth (full-screen effect quads) then begin a GXM scene without depth, so the
   forced depth load/store of STORE_DEPTH_STENCIL is skipped for them. Depth contents are untouched meanwhile. */
static int lazy_depth=-1; static GLuint lazy_want, lazy_fbo; static GLuint *lazy_attached;
static void lazy_depth_need(void) {
    if (lazy_want && lazy_attached && *lazy_attached!=lazy_want) {
        GLint cur=0; glGetIntegerv(GL_FRAMEBUFFER_BINDING,&cur); if ((GLuint)cur!=lazy_fbo) return;
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, lazy_want);
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_STENCIL_ATTACHMENT, GL_RENDERBUFFER, lazy_want);
        *lazy_attached=lazy_want;
    }
}
/* Diagnostic render-target sequence (marker rttrace, set as env SH3_RTTRACE): every 300th frame, one line per bind
   with target size, depth attachment and the draw count issued into it. */
static int rtt_on=-1, skip_pass=-2; static unsigned rtt_draws, rtt_frame, rtt_active, pass_idx; static char rtt_line[1024]; static int rtt_len;
static void rtt_flush_bind(void) { if (rtt_active && rtt_len<(int)sizeof rtt_line-40) { uint64_t a=SDL_GetPerformanceCounter(); glFinish(); double gms=(double)(SDL_GetPerformanceCounter()-a)*1000.0/(double)SDL_GetPerformanceFrequency(); rtt_len+=snprintf(rtt_line+rtt_len,sizeof rtt_line-rtt_len,":%u/%.1fms",rtt_draws,gms); } rtt_draws=0; }
void gfx_bind_target(Device *d) {
    gfx_state_invalidate();
    gfx_fx_flush();
    int level;
    GLuint ct = color_tex(d->rt, &level);
    // vitaGL fixes the sceGxm target size of an FBO at its first scene: one FBO per target size.
    typedef struct { GLuint w, h, fbo, ct, rb; int level; } FboSlot;
    static FboSlot pool[32];
    static int npool;
    unsigned rsh = rt_shift(d->rt);
    GLuint rw = (d->rt->w >> level) >> rsh, rh = (d->rt->h >> level) >> rsh;
    static int seen;
    if (g_att_stale != seen) { seen = g_att_stale; for (int i = 0; i < npool; i++) pool[i].ct = 0; }
    // Stable GXM colour targets; the vitaGL renderbuffer owns the shared logical depth/stencil storage.
    FboSlot *fs = NULL;
    for (int i = 0; i < npool && !fs; i++) if (pool[i].w == rw && pool[i].h == rh && pool[i].ct == ct && pool[i].level == level) fs = &pool[i];
    for (int i = 0; i < npool && !fs; i++) if (pool[i].w == rw && pool[i].h == rh && !pool[i].ct) fs = &pool[i];
    if (!fs) {
        fs = &pool[npool < 32 ? npool++ : 31];
        if (!fs->fbo) glGenFramebuffers(1, &fs->fbo);
        fs->w = rw; fs->h = rh; fs->ct = fs->rb = 0; fs->level = -1;
        rt_log("[rt] FBO %ux%u (slot %d)\n", rw, rh, (int)(fs - pool));
    }
    glBindFramebuffer(GL_FRAMEBUFFER, fs->fbo);
    GLuint rb = d->ds ? depth_rb(d->ds) : 0;
    if (lazy_depth<0) {
        const char *value=getenv("SH3_LAZYDEPTH");
#ifndef SH3_LAZY_DEPTH_DEFAULT
#define SH3_LAZY_DEPTH_DEFAULT 1 /* promoted 2026-11-12: 266 zones Win+Vita and UI scenario reviewed; SH3_LAZYDEPTH=0 disables */
#endif
        lazy_depth=value ? strcmp(value,"0")!=0 : SH3_LAZY_DEPTH_DEFAULT;
    }
    GLuint rb_want=rb; if (lazy_depth) rb=0;
    if (rtt_on<0) rtt_on=getenv("SH3_RTTRACE")!=NULL;
    pass_idx++; if (skip_pass==-2) { const char *sp=getenv("SH3_SKIPPASS"); skip_pass=sp?atoi(sp):-1; }
    if (rtt_on && rtt_active && rtt_len<(int)sizeof rtt_line-40) { rtt_flush_bind(); rtt_len+=snprintf(rtt_line+rtt_len,sizeof rtt_line-rtt_len," %ux%u%s",rw,rh,rb?"d":""); }
    // Attachments change only with the target, avoiding redundant state changes on every draw.
    if (fs->ct != ct || fs->level != level || fs->rb != rb) {
        if (!(g_diag_perf & 8388608u) || fs->ct != ct || fs->level != level)
            glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, ct, level);
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, rb);
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_STENCIL_ATTACHMENT, GL_RENDERBUFFER, rb);
        fs->ct = ct; fs->level = level; fs->rb = rb;
    }
    lazy_want=lazy_depth?rb_want:0; lazy_attached=&fs->rb; lazy_fbo=fs->fbo;
    static int nchk;
    GLenum st = nchk++ < 24 ? glCheckFramebufferStatus(GL_FRAMEBUFFER) : 0x8CD5;   // the check flushes the GL queue: only at the start
    if (st != 0x8CD5) {
        static int warned;
        if (warned++ < 8) rt_log("[gl] FBO incomplet 0x%x (rt %ux%u, ds %ux%u)\n", st, d->rt->w, d->rt->h,
                                 d->ds ? d->ds->w : 0, d->ds ? d->ds->h : 0);
    }
}

// ---------- states ----------
static GLenum blend_of(uint32_t b, int src) {
    switch (b) {
    case 1: return GL_ZERO; case 2: return GL_ONE; case 3: return GL_SRC_COLOR; case 4: return GL_ONE_MINUS_SRC_COLOR;
    case 5: return GL_SRC_ALPHA; case 6: return GL_ONE_MINUS_SRC_ALPHA; case 7: return GL_DST_ALPHA;
    case 8: return GL_ONE_MINUS_DST_ALPHA; case 9: return GL_DST_COLOR; case 10: return GL_ONE_MINUS_DST_COLOR;
    case 11: return GL_SRC_ALPHA_SATURATE;
    case 12: return src ? GL_SRC_ALPHA : GL_ONE_MINUS_SRC_ALPHA;
    case 13: return src ? GL_ONE_MINUS_SRC_ALPHA : GL_SRC_ALPHA;
    default: return GL_ONE;
    }
}
static GLenum cmp_of(uint32_t f) {
    static const GLenum t[] = {GL_ALWAYS, GL_NEVER, GL_LESS, GL_EQUAL, GL_LEQUAL, GL_GREATER, GL_NOTEQUAL, GL_GEQUAL, GL_ALWAYS};
    return f < 9 ? t[f] : GL_ALWAYS;
}
static GLenum sop_of(uint32_t o) {
    // D3DSTENCILOP: 1 KEEP 2 ZERO 3 REPLACE 4 INCRSAT 5 DECRSAT 6 INVERT 7 INCR (wraps) 8 DECR (wraps).
    // 7/8 used to map to the saturating GL_INCR/GL_DECR, which breaks stencil-counted shadow/light volumes
    // (the game counts with INCR/DECR on every volume face; cross-checked against another SH3 port, 2026-11-16).
    static const GLenum sat[] = {GL_KEEP, GL_KEEP, GL_ZERO, GL_REPLACE, GL_INCR, GL_DECR, GL_INVERT, GL_INCR, GL_DECR};
    static const GLenum wrap[] = {GL_KEEP, GL_KEEP, GL_ZERO, GL_REPLACE, GL_INCR, GL_DECR, GL_INVERT, GL_INCR_WRAP, GL_DECR_WRAP};
    // Candidate (not yet tested on the console): wrapping INCR/DECR, as the sibling port does. Opt in with SH3_STENCIL_WRAP=1.
    static int use_wrap = -1; if (use_wrap < 0) use_wrap = getenv("SH3_STENCIL_WRAP") != NULL;
    const GLenum *t = use_wrap ? wrap : sat;
    return o < 9 ? t[o] : GL_KEEP;
}
static void apply_states(Device *d) {
    uint32_t *rs = d->rs;
    unsigned vsh = rt_shift(d->rt);
#ifdef __vita__
    if (fx_active) vsh += (g_diag_perf & 16777216u) ? 2u : 1u;
    int cache = (g_diag_perf&1048576u) != 0;
    if (g_glstatecache_ab) {
        static int previous = -1;
        cache = __atomic_load_n(&g_glstatecache_phase,__ATOMIC_ACQUIRE);
        /* GL may have changed while bypassing the cache. Invalidate on the
           consumer itself before reusing a cached snapshot after a switch. */
        if (cache != previous) { gfx_state_invalidate(); previous = cache; }
    }
    if (cache && state_cache_matches(d,vsh)) return;
#endif
    glViewport((GLint)(d->vp.x >> vsh), (GLint)(d->vp.y >> vsh), (GLsizei)(d->vp.w >> vsh), (GLsizei)(d->vp.h >> vsh));
    glDepthRange(d->vp.minz, d->vp.maxz);
    if (rs[7] && d->ds) { glEnable(GL_DEPTH_TEST); glDepthFunc(cmp_of(rs[23])); } else glDisable(GL_DEPTH_TEST);
    glDepthMask(rs[14] ? GL_TRUE : GL_FALSE);
    if (rs[27]) {
        glEnable(GL_BLEND);
        glBlendFunc(blend_of(rs[19], 1), blend_of(rs[20], 0));
        static const GLenum eq[] = {GL_FUNC_ADD, GL_FUNC_ADD, GL_FUNC_SUBTRACT, GL_FUNC_REVERSE_SUBTRACT, GL_MIN, GL_MAX};
        glBlendEquation(rs[171] < 6 ? eq[rs[171]] : GL_FUNC_ADD);
    } else glDisable(GL_BLEND);
    // Y-flipped rendering: vertex order is reversed, the D3D front face (clockwise) becomes CCW.
    if (rs[22] == 1) glDisable(GL_CULL_FACE);
    else { glEnable(GL_CULL_FACE); glFrontFace(GL_CCW); glCullFace(rs[22] == 2 ? GL_FRONT : GL_BACK); }
    if (rs[52] && d->ds) {
        glEnable(GL_STENCIL_TEST);
        glStencilFunc(cmp_of(rs[56]), (GLint)rs[57], rs[58]);
        glStencilOp(sop_of(rs[53]), sop_of(rs[54]), sop_of(rs[55]));
        glStencilMask(rs[59]);
    } else glDisable(GL_STENCIL_TEST);
    uint32_t cw = rs[168];
    glColorMask(cw & 1, (cw >> 1) & 1, (cw >> 2) & 1, (cw >> 3) & 1);
    if (rs[47]) { glEnable(GL_POLYGON_OFFSET_FILL); glPolygonOffset(0.0f, -(float)rs[47]); }
    else glDisable(GL_POLYGON_OFFSET_FILL);
    glDisable(GL_SCISSOR_TEST);
}
static void apply_sampler(Device *d, int s) {
    uint32_t *t = d->tss[s];
    static const GLenum wrap[] = {GL_REPEAT, GL_REPEAT, GL_MIRRORED_REPEAT, GL_CLAMP_TO_EDGE, GL_CLAMP_TO_EDGE, GL_MIRRORED_REPEAT};
    GLenum mag = t[16] >= 2 ? GL_LINEAR : GL_NEAREST;
    GLenum min = t[17] >= 2 ? GL_LINEAR : GL_NEAREST;
    GLenum target=d->cube[s]?GL_TEXTURE_CUBE_MAP:GL_TEXTURE_2D;
    if (d->tex[s] && d->tex[s]->levels > 1 && t[18])
        min = t[17] >= 2 ? (t[18] >= 2 ? GL_LINEAR_MIPMAP_LINEAR : GL_LINEAR_MIPMAP_NEAREST)
                         : (t[18] >= 2 ? GL_NEAREST_MIPMAP_LINEAR : GL_NEAREST_MIPMAP_NEAREST);
#ifdef SH3_SAMPLER_CACHE_LAB
    const GLenum name[4]={GL_TEXTURE_MAG_FILTER,GL_TEXTURE_MIN_FILTER,GL_TEXTURE_WRAP_S,GL_TEXTURE_WRAP_T};
    const GLint value[4]={(GLint)mag,(GLint)min,(GLint)wrap[t[13]<6?t[13]:1],(GLint)wrap[t[14]<6?t[14]:1]};
    extern int g_att_stale;
    static int seen_deletions;
    if (seen_deletions!=g_att_stale) { sampler_invalidate();seen_deletions=g_att_stale; }
    SamplerMemo *memo=NULL;
    if (!d->cube[s] && d->tex[s] && d->tex[s]->gl) {
        GLuint object=d->tex[s]->gl;
        memo=&sampler_memo[object%64u];
        if (memo->texture!=object) { memo->texture=object;memo->valid=0; }
    }
    for (unsigned i=0;i<4;i++) {
        if (memo && (memo->valid&(1u<<i)) && memo->value[i]==value[i]) { ++sampler_saved;continue; }
        glTexParameteri(target,name[i],value[i]);++sampler_sent;
        if (memo) { memo->value[i]=value[i];memo->valid|=1u<<i; }
    }
#else
    glTexParameteri(target, GL_TEXTURE_MAG_FILTER, (GLint)mag);
    glTexParameteri(target, GL_TEXTURE_MIN_FILTER, (GLint)min);
    glTexParameteri(target, GL_TEXTURE_WRAP_S, (GLint)wrap[t[13] < 6 ? t[13] : 1]);
    glTexParameteri(target, GL_TEXTURE_WRAP_T, (GLint)wrap[t[14] < 6 ? t[14] : 1]);
#endif
}
static void set_color4(GLint loc, uint32_t argb) {
    if (uniform_valid(loc))
        glUniform4f(loc, (float)((argb >> 16) & 255) / 255.f, (float)((argb >> 8) & 255) / 255.f,
                    (float)(argb & 255) / 255.f, (float)(argb >> 24) / 255.f);
}

// ---------- attributs ----------
typedef struct { int reg, stream; uint32_t off, type; } Elem;
static int decl_elems(const uint32_t *t, int n, Elem *out) {
    int k = 0, stream = 0;
    uint32_t off = 0;
    for (int i = 0; i < n && t[i] != 0xffffffffu; i++) {
        uint32_t tok = t[i], ty = tok >> 29;
        if (ty == 1) { stream = (int)(tok & 0xf); off = 0; }
        else if (ty == 2 && (tok & (1u << 28))) off += 4 * ((tok >> 16) & 0xf);
        else if (ty == 2) {
            uint32_t dt = (tok >> 16) & 0xf;
            static const uint32_t sz[] = {4, 8, 12, 16, 4, 4, 4, 8};
            out[k].reg = (int)(tok & 0x1f); out[k].stream = stream; out[k].off = off; out[k].type = dt;
            off += dt < 8 ? sz[dt] : 4;
            k++;
        } else if (ty == 4) i += (int)((tok >> 25) & 0xf) * 4;
    }
    return k;
}
static int fvf_elems(uint32_t fvf, Elem *out) {
    int k = 0;
    uint32_t off = 0, pos = fvf & 0xe;
    out[k++] = (Elem){0, 0, 0, pos == 0x4 ? 3u : 2u};
    off = pos == 0x4 ? 16 : 12;
    if (pos >= 0x6 && pos <= 0xe) off += 4 * ((pos - 0x4) / 2);
    if (fvf & 0x10) { out[k++] = (Elem){3, 0, off, 2}; off += 12; }
    if (fvf & 0x20) off += 4;
    if (fvf & 0x40) { out[k++] = (Elem){5, 0, off, 4}; off += 4; }
    if (fvf & 0x80) { out[k++] = (Elem){6, 0, off, 4}; off += 4; }
    uint32_t ntex = (fvf >> 8) & 0xf;
    for (uint32_t i = 0; i < ntex && i < 8; i++) {
        uint32_t f = (fvf >> (16 + 2 * i)) & 3; // 0=2D 1=3D 2=4D 3=1D
        static const uint32_t ty[] = {1, 2, 3, 0};
        if (i < 4) out[k++] = (Elem){7 + (int)i, 0, off, ty[f]};
        off += 4 * (ty[f] + 1);
    }
    return k;
}
static uint32_t fvf_stride(uint32_t fvf) {
    Elem e[16];
    int n = fvf_elems(fvf, e);
    uint32_t s = 0;
    for (int i = 0; i < n; i++) {
        static const uint32_t sz[] = {4, 8, 12, 16, 4, 4, 4, 8};
        uint32_t end = e[i].off + sz[e[i].type];
        if (end > s) s = end;
    }
    if ((fvf & 0x20) && !(fvf & 0xc0) && !(fvf >> 8 & 0xf)) s += 4;
    return s;
}

// Streams read by the current VS (bit mask; 1 for an FVF) and, for an FVF, its stride: the game thread derives the data to copy from them.
uint32_t gfx_stream_mask(const Device *d, uint32_t *fvfs) {
    VShader *vs = vs_get(d->vs);
    if (vs && !vs->glsl) vs = NULL;
    if (!vs) { *fvfs = fvf_stride(d->vs); return 1; }
    *fvfs = 0;
    if (!vs->smask1) {
        Elem e[32];
        int ne = decl_elems(vs->decl, vs->ndecl, e);
        uint32_t m = 0;
        for (int i = 0; i < ne; i++) m |= 1u << e[i].stream;
        vs->smask1 = m | 0x10000;
    }
    return vs->smask1 & 0xffff;
}

// ---------- dessin ----------
static GLenum prim_of(uint32_t t) {
    static const GLenum m[] = {GL_POINTS, GL_POINTS, GL_LINES, GL_LINE_STRIP, GL_TRIANGLES, GL_TRIANGLE_STRIP, GL_TRIANGLE_FAN};
    return t < 7 ? m[t] : GL_TRIANGLES;
}
static uint32_t enabled_attrs;
// SH4_STATS: GL time per draw phase (program/uniforms, textures, attributes, states, indices, draw call).
uint64_t g_t_ph[6], g_stat_vbytes, g_stat_ubytes, g_t_copy;
unsigned g_stat_cp[3];   // CopyRects : blit GPU, relecture GL->CPU, copie CPU
static int ph_on = -1;
static uint64_t ph_t;
#define PH(i) do { if (ph_on) { uint64_t _n = SDL_GetPerformanceCounter(); g_t_ph[i] += _n - ph_t; ph_t = _n; } } while (0)
static void use_prog(GLuint p) { if (p != cur_prog) { glUseProgram(p); cur_prog = p; } }

// VBO mode (Vita default; PC: SH4_VBO=1): rarely-relocked vertex buffers live in a GL VBO (no more vertex copy on every draw).
static int use_vbo = -1, use_ibo = -1;   // IBO: SH4_NOIBO=1 to compare
int gfx_use_vbo(void) {
#ifdef __vita__   // measured: hospital 88 -> 58 ms/frame, GL forest 9.9 -> 6.7 ms; SH4_NOVBO=1 to compare
    if (use_vbo < 0) use_vbo = getenv("SH4_NOVBO") == NULL;
#else
    if (use_vbo < 0) use_vbo = getenv("SH4_VBO") != NULL;
#endif
    if (use_ibo < 0) use_ibo = use_vbo && !getenv("SH4_NOIBO");
    return use_vbo;
}
static GLuint bound_vbo;
static void bind_vbo(GLuint b) { if (b != bound_vbo) { glBindBuffer(GL_ARRAY_BUFFER, b); bound_vbo = b; } }
static void buf_sync(Buffer *b) {
    if (!b->gl) {
        glGenBuffers(1, &b->gl);
        bind_vbo(b->gl);
        glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)b->size, g_mem + b->mem, GL_STATIC_DRAW);
    } else {
        bind_vbo(b->gl);
        if (b->dhi > b->dlo) glBufferSubData(GL_ARRAY_BUFFER, b->dlo, b->dhi - b->dlo, g_mem + b->mem + b->dlo);
    }
    b->dlo = b->dhi = 0;
}
// Rewritten indices (offset + switch to 16 bits when possible): work buffer copied by vitaGL at draw time.
static void *idx_scratch(size_t n) {
    static void *buf;
    static size_t cap;
    if (n > cap) { free(buf); cap = n * 2; buf = malloc(cap); }
    return buf;
}
// Indices: bounds [lo, hi]. Client arrays -> rebase on lo (vitaGL only copies hi-lo+1 vertices);
// VBO -> offset of first (attribute pointers stay small offsets, cf. sceGxm 16-bit offset).
// Returns the indices to draw (work buffer if rewritten: offset and/or switch to 16 bits).
static const void *idx_rewrite(const void *indices, uint32_t n, int *index32, int vbo, uint32_t *first, uint32_t *lo_o, uint32_t *hi_o) {
    uint32_t lo = 0xffffffffu, hi = 0;
    int i32 = *index32;
    if (i32) for (uint32_t i = 0; i < n; i++) { uint32_t v = ((const uint32_t *)indices)[i]; if (v < lo) lo = v; if (v > hi) hi = v; }
    else for (uint32_t i = 0; i < n; i++) { uint32_t v = ((const uint16_t *)indices)[i]; if (v < lo) lo = v; if (v > hi) hi = v; }
    int64_t bias = vbo ? (int64_t)*first : -(int64_t)lo;
    if (!vbo) *first += lo;
    lo += (uint32_t)bias; hi += (uint32_t)bias;
    if (bias || (i32 && hi < 0xffff)) {
        int w32 = hi >= 0xffff;
        void *o = idx_scratch((size_t)n * (w32 ? 4 : 2));
        for (uint32_t i = 0; i < n; i++) {
            uint32_t v = (i32 ? ((const uint32_t *)indices)[i] : ((const uint16_t *)indices)[i]) + (uint32_t)bias;
            if (w32) ((uint32_t *)o)[i] = v; else ((uint16_t *)o)[i] = (uint16_t)v;
        }
        indices = o;
        *index32 = w32;
    }
    *lo_o = lo; *hi_o = hi;
    return indices;
}

// IBO cache (VBO mode): the indices of a rarely-relocked IB are rewritten once then stay in an IBO; vitaGL draws
// from the IBO without copying anything (no min/max scan, no recording in the GL queue, no temporary copy).
// Key = IB content (Buffer.ver, renewed on every Lock/Unlock) + index range + BaseVertexIndex.
typedef struct { uint32_t ver, off, n, first, i32; } IboKey;
typedef struct { IboKey k; GLuint ibo; uint32_t lo, hi, bytes, last; } IboEnt;   // ibo == 0 : case libre
#define IBO_N 1024
#define IBO_BUDGET (4u << 20)
static IboEnt ibo_tab[IBO_N];
static unsigned ibo_cnt;
static uint32_t ibo_bytes;
static GLuint ibo_pool[16];   // names reserved in batches (glGenBuffers forces a sync with the render thread)
static int ibo_np;
static GLuint bound_ibo;
static void bind_ibo(GLuint b) { if (b != bound_ibo) { glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, b); bound_ibo = b; } }
static unsigned ibo_slot(const IboKey *k) {   // key slot, or first free slot of its chain
    unsigned h = (k->ver * 2654435761u + k->off * 40503u + k->n * 2246822519u + k->first * 3266489917u + k->i32) >> 22;
    while (ibo_tab[h].ibo && memcmp(&ibo_tab[h].k, k, sizeof *k)) h = (h + 1) % IBO_N;
    return h;
}
// Frees entries unused for min_age frames (0 = all) and re-hashes the survivors.
static void ibo_trim(unsigned min_age) {
    static IboEnt keep[IBO_N];
    GLuint del[IBO_N];
    unsigned nk = 0, nd = 0;
    bind_ibo(0);
    for (unsigned i = 0; i < IBO_N; i++) {
        if (!ibo_tab[i].ibo) continue;
        if (min_age && g_pframe - ibo_tab[i].last < min_age) keep[nk++] = ibo_tab[i];
        else { del[nd++] = ibo_tab[i].ibo; ibo_bytes -= ibo_tab[i].bytes; }
    }
    if (nd) glDeleteBuffers((GLsizei)nd, del);
    memset(ibo_tab, 0, sizeof ibo_tab);
    for (unsigned i = 0; i < nk; i++) ibo_tab[ibo_slot(&keep[i].k)] = keep[i];
    ibo_cnt = nk;
}
// IBO entry (bound to the binding point) for these indices, created if needed; NULL if the draw is not cacheable.
static IboEnt *ibo_get(Device *d, const void *indices, uint32_t n, uint32_t first, int index32) {
    Buffer *ib = d->ib;
    uint32_t isz = index32 ? 4 : 2;
    if (!ib || ib->dyn || n * isz > (256u << 10)) return NULL;
    const uint8_t *i0 = g_mem + ib->mem, *ip = indices;
    if (ip < i0 || ip + (size_t)n * isz > i0 + ib->size) return NULL;   // not in the current IB (user array)
    IboKey k = {ib->ver, (uint32_t)(ip - i0), n, first, (uint32_t)index32};
    unsigned h = ibo_slot(&k);
    if (ibo_tab[h].ibo) { ibo_tab[h].last = g_pframe; bind_ibo(ibo_tab[h].ibo); return &ibo_tab[h]; }
    uint32_t f = first, lo, hi;
    int i32 = index32;
    const void *o = idx_rewrite(indices, n, &i32, 1, &f, &lo, &hi);
    if (i32) return NULL;   // ponytail: real 32-bit indices (> 65534): client-array path
    if (ibo_bytes + n * 2 > IBO_BUDGET || ibo_cnt >= IBO_N * 3 / 4) {
        ibo_trim(120);
        if (ibo_bytes + n * 2 > IBO_BUDGET || ibo_cnt >= IBO_N * 3 / 4) ibo_trim(0);
        h = ibo_slot(&k);
    }
    if (!ibo_np) { glGenBuffers(16, ibo_pool); ibo_np = 16; }
    GLuint id = ibo_pool[--ibo_np];
    bind_ibo(id);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, (GLsizeiptr)n * 2, o, GL_STATIC_DRAW);
    ibo_tab[h] = (IboEnt){k, id, lo, hi, n * 2, g_pframe};
    ibo_cnt++; ibo_bytes += n * 2;
    return &ibo_tab[h];
}
static unsigned ctl_trace_frame, ctl_trace_n = 1;   // ctl.txt trace=F (Vita): same as SH4_TRACE_FRAME=F without restarting
/* The bench runs on the game thread. Arm the trace on the render thread. */
static unsigned bench_trace_requested;
void diag_trace_next_frames(void) {
    __atomic_store_n(&bench_trace_requested, 1, __ATOMIC_RELEASE);
}
static int trace_now(void) {   // SH4_TRACE_FRAME=N: detailed trace of frame N
    static const char *tf; static unsigned tgt;
    if (ctl_trace_frame && g_pframe + 1 >= ctl_trace_frame && g_pframe + 1 < ctl_trace_frame + ctl_trace_n) return 1;
    if (!tf) {
        tf = getenv("SH4_TRACE_FRAME") ? getenv("SH4_TRACE_FRAME") : ""; tgt = (unsigned)atoi(tf);
        const char *count = getenv("SH4_TRACE_COUNT");
        if (count && atoi(count) > 0 && atoi(count) <= 60) ctl_trace_n = (unsigned)atoi(count);
    }
    return *tf && g_pframe + 1 >= tgt && g_pframe + 1 - tgt < ctl_trace_n;
}

static void draw_impl(Device *d, uint32_t prim, uint32_t first, uint32_t nverts, uint32_t nprims, const void *indices,
                      int index32, uint32_t min_index, const uint8_t *up, uint32_t up_stride, const uint8_t *const *inl);
// Hot settings (diagnostics, Vita): ux0:data/sh3/ctl.txt re-read every 30 frames, one line per variant "FRAME tokens..."
// (the last one with FRAME <= current frame applies; no number = 0). Tokens: skip=hex,hex (VS handles whose draws are
// ignored), skipn=N (draws with fewer than N indices ignored), skipall=1, gpufin=0|1. Logs the render thread's active time per window.
#ifdef __vita__
int g_ctl_pad[4], g_ctl_pad_on;
unsigned g_ctl_keys[8], g_ctl_nkeys;   // ctl.txt keys=hex,hex: DirectInput keys held (read by dinput.c), live-play diagnostics
static unsigned ctl_skip[8], ctl_nskip, ctl_skipn, ctl_skipall, ctl_cut[8], ctl_ncut, ctl_nouni;
void q_set_gpufin(int on);
void q_set_split(int n);
void q_win(uint32_t *wall, uint32_t *idle);
// ctl snap=0/1/2 then snap=9: "which game flag changes when X appears?" Bitmaps of non-zero dwords of the game's data/bss
// (0x5c0000..0x1300000) taken in state 0 (normal), 1 (X visible), 2 (normal again); snap=9 logs the dwords that are the same in 0 and 2 but differ in 1.
extern uint8_t *g_mem;
#define SNAP_LO 0x5c0000u
#define SNAP_N ((0x1300000u - SNAP_LO) / 4)
static void mem_snap(int k) {
    static uint8_t *bm[3];
    if (k >= 0 && k < 3) {
        if (!bm[k]) bm[k] = calloc(1, SNAP_N / 8 + 1);
        if (!bm[k]) return;
        const uint32_t *p = (const uint32_t *)(g_mem + SNAP_LO);
        memset(bm[k], 0, SNAP_N / 8 + 1);
        for (uint32_t i = 0; i < SNAP_N; i++) if (p[i]) bm[k][i >> 3] |= (uint8_t)(1u << (i & 7));
        rt_log("[snap] %d taken\n", k);
    } else if (k == 9 && bm[0] && bm[1] && bm[2]) {
        const uint32_t *p = (const uint32_t *)(g_mem + SNAP_LO);
        unsigned n = 0;
        for (uint32_t i = 0; i < SNAP_N; i++) {
            unsigned a = bm[0][i >> 3] >> (i & 7) & 1, b = bm[1][i >> 3] >> (i & 7) & 1, c = bm[2][i >> 3] >> (i & 7) & 1;
            if (a == c && a != b && n++ < 400) rt_log("[snap] %08x state0=%u state1=%u now=%08x\n", SNAP_LO + i * 4, a, b, p[i]);
        }
        rt_log("[snap] %u candidates\n", n);
    }
}
// ctl dump=N: writes the game data/bss (0x5c0000..0x1300000) to ux0:data/sh3/gdump_N.bin for an offline diff with full values (tools/dump_diff.py).
static void mem_dump(int k) {
    char fn[64]; snprintf(fn, sizeof fn, "ux0:data/sh3/gdump_%d.bin", k);
    FILE *f = fopen(fn, "wb");
    if (!f) { rt_log("[dump] cannot open %s\n", fn); return; }
    size_t total = 0, step = 65536, n = 0x1300000u - SNAP_LO;
    for (size_t o = 0; o < n; o += step) total += fwrite(g_mem + SNAP_LO + o, 1, step < n - o ? step : n - o, f);
    fclose(f);
    rt_log("[dump] %s %u bytes frame %u\n", fn, (unsigned)total, g_pframe);
}
static void ctl_poll(void) {
    static char txt[600], last[600];
    static unsigned cur = ~0u;
    static uint32_t w0, i0;
    memset(txt, 0, sizeof txt);
    FILE *f = fopen("ux0:data/sh3/ctl.txt", "r");
    if (!f) return;
    size_t n = fread(txt, 1, sizeof txt - 1, f); txt[n] = 0; fclose(f);
    uint32_t w, i;
    q_win(&w, &i);
    if (w0) rt_log("[ctl] frame %u variant %u: active %.2f ms/frame\n", g_pframe, cur, (double)((w - w0) - (i - i0)) / 1000.0 / 30.0);
    w0 = w; i0 = i;
    strcpy(last, txt);
    unsigned best = 0, bi = 0, li = 0;
    char *lines[16];
    for (char *l = strtok(last, "\r\n"); l && li < 16; l = strtok(NULL, "\r\n")) lines[li++] = l;
    for (unsigned k = 0; k < li; k++) {
        unsigned fr = (unsigned)strtoul(lines[k], NULL, 10);
        if (fr <= g_pframe && fr >= best) { best = fr; bi = k + 1; }
    }
    static char curtxt[120];   // compare the text, not the line index: the file may be rewritten with the same number of lines
    char *sel = bi ? lines[bi - 1] : "";
    if (bi == cur && !strcmp(sel, curtxt)) return;
    cur = bi; snprintf(curtxt, sizeof curtxt, "%s", sel);
    g_ctl_nkeys = 0; g_ctl_pad_on = 0;
    ctl_nskip = ctl_skipn = ctl_skipall = ctl_ncut = ctl_nouni = ctl_nofog = 0;
    q_set_split(-1);
    if (!bi) return;
    char ln[120]; snprintf(ln, sizeof ln, "%s", lines[bi - 1]);
    for (char *t = strtok(ln, " "); t; t = strtok(NULL, " ")) {
        if (!strncmp(t, "skip=", 5)) { for (char *c = t + 5; *c && ctl_nskip < 8; ) { ctl_skip[ctl_nskip++] = (unsigned)strtoul(c, &c, 16); if (*c == ',') c++; } }
        else if (!strncmp(t, "cut=", 4)) { for (char *c = t + 4; *c && ctl_ncut < 8; ) { ctl_cut[ctl_ncut++] = (unsigned)strtoul(c, &c, 16); if (*c == ',') c++; } }
        else if (!strncmp(t, "nouni=", 6)) ctl_nouni = (unsigned)atoi(t + 6);
        else if (!strncmp(t, "nofog=", 6)) ctl_nofog = (unsigned)atoi(t + 6);
        else if (!strncmp(t, "snap=", 5)) mem_snap(atoi(t + 5));
        else if (!strncmp(t, "dump=", 5)) mem_dump(atoi(t + 5));
        else if (!strncmp(t, "pad=", 4)) { g_ctl_pad_on = 1; sscanf(t + 4, "%d,%d,%d,%d", &g_ctl_pad[0], &g_ctl_pad[1], &g_ctl_pad[2], &g_ctl_pad[3]); }   // test: injected sticks (left x,y right x,y)
        else if (!strncmp(t, "keys=", 5)) { for (char *c = t + 5; *c && g_ctl_nkeys < 8; ) { g_ctl_keys[g_ctl_nkeys++] = (unsigned)strtoul(c, &c, 16); if (*c == ',') c++; } }
        else if (!strncmp(t, "skipn=", 6)) ctl_skipn = (unsigned)atoi(t + 6);
        else if (!strncmp(t, "skipall=", 8)) ctl_skipall = (unsigned)atoi(t + 8);
        else if (!strncmp(t, "tracen=", 7)) ctl_trace_n = (unsigned)atoi(t + 7);
        else if (!strncmp(t, "tracenow=", 9)) { ctl_trace_n = (unsigned)atoi(t + 9); ctl_trace_frame = g_pframe + 2; }   // trace the next N frames from now
        else if (!strncmp(t, "trace=", 6)) ctl_trace_frame = (unsigned)atoi(t + 6);
        else if (!strncmp(t, "split=", 6)) q_set_split(atoi(t + 6));
        else if (!strncmp(t, "gpufin=", 7)) q_set_gpufin(atoi(t + 7));
    }
    rt_log("[ctl] variant %u: %u VS ignored, %u VS cut to 1 triangle, nouni=%u skipn=%u skipall=%u\n", cur, ctl_nskip, ctl_ncut, ctl_nouni, ctl_skipn, ctl_skipall);
}
static int ctl_skip_draw(const Device *d, uint32_t n) {
    if (ctl_skipall || (ctl_skipn && n < ctl_skipn)) return 1;
    for (unsigned i = 0; i < ctl_nskip; i++) if (ctl_skip[i] == d->vs) return 1;
    return 0;
}
// cut=: the draw keeps its states but only processes one triangle (separates per-draw cost from per-vertex cost)
static void ctl_cut_draw(const Device *d, uint32_t prim, uint32_t *n, uint32_t *np) {
    if (prim < 4 || prim > 6) return;
    for (unsigned i = 0; i < ctl_ncut; i++) if (ctl_cut[i] == d->vs) { *n = 3; *np = 1; return; }
}
#else
static void ctl_poll(void) {}
#define ctl_skip_draw(d, n) 0
#define ctl_cut_draw(d, prim, n, np) ((void)0)
#define ctl_nouni 0
#endif
#ifdef __vita__
/* Lens-flare / lamp visibility probe (flashlight halo around Heather). The game draws a one-vertex probe into a 1x1 render
   target that samples the scene colour at the light's screen position, draws a marker at the light's depth into the
   scene, and samples again: first value = scene colour, second = white when the marker passed the depth test (not
   occluded). The Vita never writes anything into the 1x1 target (readback after the draw = the same stale video memory
   even for a constant white output, bench lamp 2026-11-16), so the game read black, concluded "occluded" and dropped
   24 of the 27 halo sprites. The sample is emulated: first sample = the real scene pixel, second = white (visible).
   Limit: occlusion of the light by geometry is not evaluated for this probe. */
static void probe_emulate(Device *d) {
    static unsigned phase_frame = ~0u; static int post;
    if (phase_frame != g_pframe) { phase_frame = g_pframe; post = 0; }
    uint32_t v = 0xffffffffu;   // RGBA bytes ff ff ff ff
    static int noread = -1; if (noread < 0) noread = getenv("SH3_PROBE_NOREAD") != NULL;
    if (!post && !noread) {
        uint8_t sc[4] = {0, 0, 0, 0xff};
        Texture *t = d->tex[0]; uint32_t x = (d->sh3_probe - 1) >> 12, y = (d->sh3_probe - 1) & 0xfff;
        if (t && t->lv && t->lv[0] && x < t->w && y < t->h) {
            unsigned sh = rt_shift(t->lv[0]);
            bind_read(t->lv[0]); glReadPixels((GLint)(x >> sh), (GLint)((t->h - 1 - y) >> sh), 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, sc);
            gfx_bind_target(d);
        }
        memcpy(&v, sc, 4);
    }
    if (!post && noread) v = 0xff000000u;   // no GPU readback: scene sample = black
    post ^= 1;
    d->rt->emu_px = v; d->rt->emu_valid = 1;
    static unsigned nlog; if (nlog++ < 6) rt_log("[probe-emu] n=%u phase=%s value=%08x\n", nlog, post ? "scene" : "visible", v);
}
#endif
void gfx_draw(Device *d, uint32_t prim, uint32_t first, uint32_t nverts, uint32_t nprims, const void *indices,
              int index32, uint32_t min_index, const uint8_t *up, uint32_t up_stride, const uint8_t *const *inl) {
#ifdef __vita__
    if (d->sh3_probe && d->rt && d->rt->w == 1 && d->rt->h == 1 && !ENVFLAG("SH3_NO_PROBE_EMU")) { probe_emulate(d); return; }
    static int probe_fx = -1;
    static int probe_geo = -1;
    if (probe_geo < 0) probe_geo = getenv("SH3_PROBE_GEO") != NULL;
    if (probe_geo && d->rt && d->rt->w == 960 && d->rt->h == 544 &&
        !indices && (d->vs == 0xddu || d->vs == 0x2fu)) {
        static unsigned cuts;
        if (!(cuts++ % 500))
            rt_log("[probe-geo] omitted=%u frame=%u\n", cuts, g_pframe);
        return;
    }
    if (probe_fx < 0) {
        const char *mode = getenv("SH3_PROBE_FX");
        /* Valentin prefers the measured sharp, higher-throughput version.
           Keep an explicit diagnostic override for controlled comparisons. */
        probe_fx = 1;
        if (mode) {
            if (!strcmp(mode, "off")) probe_fx = 0;
            else if (!strcmp(mode, "one")) probe_fx = 4;
            else if (!strcmp(mode, "pair")) probe_fx = 3;
            else if (!strcmp(mode, "wide")) probe_fx = 2;
            else if (!strcmp(mode, "hospital")) probe_fx = 5;
            else probe_fx = 1;
        }
        rt_log("[probe-fx] selected mode=%d (default sharp on Vita)\n", probe_fx);
    }
    if (probe_fx && (probe_fx != 5 || d->sh3_hospital_sharp) &&
        d->rt && ((d->rt->w == 960 && d->rt->h == 544) ||
        (d->rt->w == 720 && d->rt->h == 408)) &&
        d->vs == 0x104 && !d->ps && prim == 5 && nverts == 4 && !indices &&
        d->tex[0] && ((d->tex[0]->w == 256 && d->tex[0]->h == 256) ||
        (probe_fx == 2 && d->tex[0]->w == d->rt->w && d->tex[0]->h == d->rt->h &&
         d->rs[15] && d->rs[52] && d->rs[56] == 6 && d->rs[57] == 64)) &&
        !(d->rs[7] && d->rs[14]) &&
        (!d->rs[52] || !d->rs[59] ||
         (d->rs[53] == 1 && d->rs[54] == 1 && d->rs[55] == 1))) {
        static unsigned cuts;
        static unsigned last_frame = ~0u, eligible_draws;
        if (last_frame != g_pframe) {
            last_frame = g_pframe;
            eligible_draws = 0;
        }
        /* Diagnostic visual compromise: keep the first one or two eligible draws. */
        if (probe_fx < 3 || probe_fx == 5 ||
            eligible_draws++ >= (probe_fx == 3 ? 2u : 1u)) {
            if (!(cuts++ % 500))
                rt_log("[probe-fx] omitted=%u frame=%u\n", cuts, g_pframe);
            return;
        }
    }
#endif
#ifdef SH3_LAZY_DEPTH_TEST_ONLY
    /* Isolated lab: with GL_DEPTH_TEST disabled (apply_states), GL_DEPTH_WRITEMASK
       cannot write depth. Stencil still independently requires the attachment.
       Explicit depth/stencil clears retain their own lazy_depth_need() below. */
    if (lazy_want && (d->rs[7] || d->rs[52])) lazy_depth_need();
    if (lazy_want && lazy_attached && *lazy_attached != lazy_want &&
        !d->rs[7] && !d->rs[52] && d->rs[14]) {
        static unsigned reports;
        if (reports < 4) {
            ++reports;
            rt_log("[depth-test-only] no attachment for disabled depth test, write mask set\n");
        }
    }
#else
    if (lazy_want && (d->rs[7] || d->rs[14] || d->rs[52])) lazy_depth_need();
#endif
#ifdef __vita__
    extern int g_diag_draw_cut;
    if (g_diag_draw_cut) return; /* time-bounded diagnostic, restored by platform */
    extern unsigned g_diag_pass_phase,g_diag_pass_skipped[2];
    unsigned pass_phase=__atomic_load_n(&g_diag_pass_phase,__ATOMIC_RELAXED);
    if ((pass_phase==1 && d->vs==0x144u && !d->ps && nverts<=6 && (prim==5 || prim==6)) ||
        (pass_phase==2 && d->vs==0x2fu && d->ps==4u)) {
        __atomic_fetch_add(&g_diag_pass_skipped[pass_phase-1],1u,__ATOMIC_RELAXED);
        return; /* diagnostic omission only; never a rendering optimization */
    }
#endif
    if (ctl_skip_draw(d, nverts)) return;
    ctl_cut_draw(d, prim, &nverts, &nprims);
    fx_route(d,prim,first,nverts,indices,up,up_stride,inl);
#ifdef __vita__
    static int vramlog = -1;   // SH4_VRAMLOG: vglMemFree walks the heap, too expensive for every draw by default
    if (vramlog < 0) vramlog = getenv("SH4_VRAMLOG") != NULL;
    size_t vm0 = vramlog ? vglMemFree(VGL_MEM_VRAM) : 0;
#endif
    if (ph_on < 0) ph_on = getenv("SH4_PH") != NULL;
    // Per-draw timers (reading the counter costs ~0.8 us on Vita: -2.6 ms/frame in the hospital without them): only with SH4_PH=1.
    uint64_t t0 = ph_on ? SDL_GetPerformanceCounter() : 0;
    ph_t = t0;
    draw_impl(d, prim, first, nverts, nprims, indices, index32, min_index, up, up_stride, inl);
    {   // SH4_TRACE_FRAME: value of the target's centre pixel after each draw
        if (trace_now() && ctl_trace_n <= 1) {   // glReadPixels = GPU sync: not in range mode
            uint8_t px[8] = {0}, px2[8] = {0};
            unsigned sh = rt_shift(d->rt);
            glReadPixels((GLint)((d->rt->w >> sh) / 2), (GLint)((d->rt->h >> sh) / 2), 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px);
            glReadPixels((GLint)((d->rt->w >> sh) / 4), (GLint)((d->rt->h >> sh) / 3), 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px2);
            rt_log("   -> rt=%p tex=%p fbo_tex=%u px(c)=%02x%02x%02x%02x px(q)=%02x%02x%02x%02x\n", (void *)d->rt, (void *)d->rt->tex, d->rt->fbo_tex,
                   px[0], px[1], px[2], px[3], px2[0], px2[1], px2[2], px2[3]);
        }
    }
    uint64_t dt = ph_on ? SDL_GetPerformanceCounter() - t0 : 0;
    g_t_gl += dt;
#ifdef __vita__
    if (vramlog) { size_t vm1 = vglMemFree(VGL_MEM_VRAM); if (vm0 > vm1 && vm0 - vm1 > (512u << 10)) rt_log("[vram] draw -%u KB (left %u KB) prim=%u n=%u tex0=%ux%u rt=%ux%u\n", (unsigned)((vm0 - vm1) >> 10), (unsigned)(vm1 >> 10), prim, nverts, d->tex[0] ? d->tex[0]->w : 0, d->tex[0] ? d->tex[0]->h : 0, d->rt->w, d->rt->h); }
#endif
    if (dt * 1000 / SDL_GetPerformanceFrequency() > 100) rt_log("[slow] draw %u ms : prim=%u n=%u vs=%08x ps=%x tex0=%ux%u rt=%ux%u\n", (unsigned)(dt * 1000 / SDL_GetPerformanceFrequency()), prim, nverts, d->vs, d->ps, d->tex[0] ? d->tex[0]->w : 0, d->tex[0] ? d->tex[0]->h : 0, d->rt->w, d->rt->h);
}
static void draw_impl(Device *d, uint32_t prim, uint32_t first, uint32_t nverts, uint32_t nprims, const void *indices,
                      int index32, uint32_t min_index, const uint8_t *up, uint32_t up_stride, const uint8_t *const *inl) {
    (void)nprims; (void)min_index;
    if (!nverts) return;
    extern unsigned g_stat_draws;
    g_stat_draws++;
    if (g_stat_draws <= 5 || g_stat_draws == 100 || g_stat_draws == 500)
        rt_log("[draw] #%u prim=%u n=%u vs=%08x tex=%p\n", g_stat_draws, prim, nverts, d->vs, (void *)d->tex[0]);
    VShader *vs = vs_get(d->vs);
    if (vs && !vs->glsl) vs = NULL;
    uint32_t fvf = vs ? 0 : d->vs;
    unsigned char save_vertices[64*32] __attribute__((aligned(16)));
    /* Match the native list cache at its first full-canvas panel. The game
       defers UI commands beyond F596190, so CPU function lifetime is not the
       rendering scope. Close the scope when this cache is composited. */
    static Surface *save_target;static Texture *save_texture;
    int save_enabled=__atomic_load_n(&g_diag_save_wide,__ATOMIC_RELAXED);
    if(!save_enabled || (d->sh3_ui_screen!=18 && d->sh3_ui_screen!=19 && d->sh3_ui_screen!=29)) {save_target=NULL;save_texture=NULL;}
    else if(save_target && d->rt!=save_target && d->tex[0]==save_texture) {save_target=NULL;save_texture=NULL;}
    if(save_enabled && (d->sh3_ui_screen==18 || d->sh3_ui_screen==19 || d->sh3_ui_screen==29) && !vs && fvf==0x144u &&
       !d->ps && !indices && nverts<=64 && !d->vp.x && !d->vp.y && d->rt->w==d->vp.w && d->rt->h==d->vp.h) {
        Buffer *vb=d->stream[0].vb;
        const uint8_t *data=up?up:(inl && inl[0]?inl[0]:(vb?g_mem+vb->mem:NULL));
        unsigned stride=up?up_stride:d->stream[0].stride;
        if(data && stride>=28 && stride<=32 && (up || (inl && inl[0]) || (vb && ((uint64_t)first+nverts)*stride<=vb->size))) {
            float xy[128];for(unsigned i=0;i<nverts;i++)memcpy(xy+2*i,data+(size_t)(first+i)*stride,8);
            if(!d->tex[0] && !d->rs[27] && !d->rs[7] && nverts==4 && d->rt->tex) {
                float left=INFINITY,right=-INFINITY,top=INFINITY,bottom=-INFINITY;unsigned color_ok=1;
                for(unsigned i=0;i<4;i++) {uint32_t color;memcpy(&color,data+(size_t)(first+i)*stride+16,4);if(color!=0xff010101u)color_ok=0;
                    left=fminf(left,xy[2*i]);right=fmaxf(right,xy[2*i]);top=fminf(top,xy[2*i+1]);bottom=fmaxf(bottom,xy[2*i+1]);}
                float margin=(d->vp.w-d->vp.h*(4.0f/3.0f))*.5f;
                if(color_ok && fabsf(left-margin)<2 && fabsf(right-(d->vp.w-margin))<2 && top<=0 && bottom>=d->vp.h) {
                    save_target=d->rt;save_texture=d->rt->tex;
                    static unsigned reports;if(reports++<4)rt_log("[save-wide] native list cache matched rt=%p\n",(void*)save_target);
                }
            }
            int glyph=d->rs[27] && d->rs[7] && d->tex[0] && d->tex[0]->w==512 && d->tex[0]->h==512;
            if(d->rt==save_target && diag_wide_save_layout(d->vp.w,d->vp.h,nverts,xy,glyph)) {
                for(unsigned i=0;i<nverts;i++){memcpy(save_vertices+i*stride,data+(size_t)(first+i)*stride,stride);memcpy(save_vertices+i*stride,xy+2*i,8);}
                up=save_vertices;up_stride=stride;first=0;inl=NULL;
            }
        }
    }
    unsigned char ui_vertices[4*32] __attribute__((aligned(16)));
    unsigned char inventory_vertices[32*32] __attribute__((aligned(16)));
    unsigned char footer_vertices[4*32] __attribute__((aligned(16)));
    unsigned footer_stride=0;
    unsigned ui_fill=__atomic_load_n(&g_diag_ui_background_fill,__ATOMIC_RELAXED);
    if(__atomic_load_n(&g_diag_inventory_wide,__ATOMIC_RELAXED) && d->sh3_ui_screen==6 &&
       !vs && fvf==0x144u && !d->ps && !indices && nverts<=32 && !d->vp.x && !d->vp.y &&
       d->rt->w==d->vp.w && d->rt->h==d->vp.h) {
        Buffer *vb=d->stream[0].vb;
        const uint8_t *data=up?up:(inl && inl[0]?inl[0]:(vb?g_mem+vb->mem:NULL));
        unsigned stride=up?up_stride:d->stream[0].stride;
        if(data && stride>=28 && stride<=32 && (up || (inl && inl[0]) || (vb && ((uint64_t)first+nverts)*stride<=vb->size))) {
            float xy[64];for(unsigned i=0;i<nverts;i++)memcpy(xy+2*i,data+(size_t)(first+i)*stride,8);
            if(diag_wide_inventory_layout((float)d->vp.w,(float)d->vp.h,nverts,xy,d->tex[0]!=NULL)) {
                for(unsigned i=0;i<nverts;i++){memcpy(inventory_vertices+i*stride,data+(size_t)(first+i)*stride,stride);memcpy(inventory_vertices+i*stride,xy+2*i,8);}
                up=inventory_vertices;up_stride=stride;first=0;inl=NULL;
            }
        }
    }
    if(ui_fill &&
       !vs && fvf==0x144u && !d->ps && !indices && nverts==4 && prim==5 &&
       !d->rs[7] && !d->rs[27] && d->tex[0] && !d->vp.x && !d->vp.y) {
        Buffer *vb=d->stream[0].vb;
        const uint8_t *data=up?up:(inl && inl[0]?inl[0]:(vb?g_mem+vb->mem:NULL));
        unsigned stride=up?up_stride:d->stream[0].stride;
        if(data && stride>=28 && stride<=32 && (up || (inl && inl[0]) || (vb && ((uint64_t)first+4)*stride<=vb->size))) {
            float xy[8];
            for(unsigned i=0;i<4;i++)memcpy(xy+2*i,data+(size_t)(first+i)*stride,8);
            if(diag_wide_background_fill(d->sh3_ui_screen,(float)d->vp.w,(float)d->vp.h,xy)) {
                /* Full artwork retained: horizontal stretch only, including
                   the title's baked copyright strip. Text draws stay native. */
                if(ui_fill==3)for(unsigned i=0;i<4;i++)memcpy(xy+2*i+1,data+(size_t)(first+i)*stride+4,4);
                /* The title's copyright is baked into the background bitmap.
                   Keep its native bottom strip after uniformly cropping the
                   decorative background. No replacement asset or font. */
                if(ui_fill==2 && d->sh3_ui_screen==1) {
                    float y0=INFINITY,y1=-INFINITY;unsigned top=0,bottom=0;
                    for(unsigned i=0;i<4;i++) {
                        float y;memcpy(&y,data+(size_t)(first+i)*stride+4,4);
                        if(y<y0){y0=y;top=i;}if(y>y1){y1=y;bottom=i;}
                    }
                    float v0,v1;memcpy(&v0,data+(size_t)(first+top)*stride+24,4);memcpy(&v1,data+(size_t)(first+bottom)*stride+24,4);
                    if(y1>y0 && isfinite(v0) && isfinite(v1))for(unsigned i=0;i<4;i++) {
                        const uint8_t *src=data+(size_t)(first+i)*stride;float old_y;memcpy(&old_y,src+4,4);
                        float y=old_y==y0?d->vp.h*(504.0f/544.0f):(float)d->vp.h;
                        float v=v0+(y-y0)*(v1-v0)/(y1-y0);
                        memcpy(footer_vertices+i*stride,src,stride);memcpy(footer_vertices+i*stride+4,&y,4);memcpy(footer_vertices+i*stride+24,&v,4);
                        footer_stride=stride;
                    }
                }
                for(unsigned i=0;i<4;i++){memcpy(ui_vertices+i*stride,data+(size_t)(first+i)*stride,stride);memcpy(ui_vertices+i*stride,xy+2*i,8);}
                up=ui_vertices;up_stride=stride;first=0;inl=NULL;
                static unsigned last_screen;
                if(last_screen!=d->sh3_ui_screen){last_screen=d->sh3_ui_screen;rt_log("[ui-fill] screen=%u opaque backdrop mapped to %ux%u; text unchanged\n",d->sh3_ui_screen,d->vp.w,d->vp.h);}
            }
        }
    }
    if (!vs && (d->vs & 1)) { d3d_log_once("draw with function-less VS: ignored"); return; }
    PShader *ps = ps_get(d->ps);
    { static int seen; if (!seen && d->vs == 0xb) { seen = 1; rt_log("[scene] first draw vs=0x0b at frame %u\n", g_pframe); } }
    Prog *p = get_prog(d, vs, fvf, ps);
    use_prog(p->prog);
    if (uniform_valid(p->c)) {
        if (vs) { if (vs->nc && !ctl_nouni && (!p->sent || p->vsc_gen != d->vsc_gen)) { glUniform4fv(p->c, vs->nc, &d->vsc[0][0]); p->vsc_gen = d->vsc_gen; if (ph_on) g_stat_ubytes += vs->nc * 16u; } }
        else { // fixed XYZ: transposed W*V*P (rows = columns of the D3D matrix)
            float m1[16], m2[16], c4[16];
            for (int i = 0; i < 4; i++)
                for (int j = 0; j < 4; j++) {
                    m1[i * 4 + j] = 0;
                    for (int k = 0; k < 4; k++) m1[i * 4 + j] += d->world[i * 4 + k] * d->view[k * 4 + j];
                }
            for (int i = 0; i < 4; i++)
                for (int j = 0; j < 4; j++) {
                    m2[i * 4 + j] = 0;
                    for (int k = 0; k < 4; k++) m2[i * 4 + j] += m1[i * 4 + k] * d->proj[k * 4 + j];
                }
            for (int i = 0; i < 4; i++)
                for (int j = 0; j < 4; j++) c4[i * 4 + j] = m2[j * 4 + i];
            glUniform4fv(p->c, 4, c4);
        }
    }
    {   // GL uniforms belong to the program: only what changed since its last draw is resent
        uint32_t sig[10] = {d->rs[60], d->rs[34], d->rs[36], d->rs[37], d->rs[38], d->rs[24] & 255, d->vp.x, d->vp.y, d->vp.w, d->vp.h};
        if (!p->sent || memcmp(sig, p->sig, sizeof sig)) {
            if (uniform_valid(p->flip)) glUniform4f(p->flip, -1.0f, 0, 0, 0);
            if (uniform_valid(p->vp)) glUniform4f(p->vp, (float)d->vp.x, (float)d->vp.y, (float)d->vp.w, (float)d->vp.h);
            set_color4(p->tf, d->rs[60]);
            set_color4(p->fogc, d->rs[34]);
            if (uniform_valid(p->fogp)) glUniform4f(p->fogp, rt_f32(d->rs[36]), rt_f32(d->rs[37]), rt_f32(d->rs[38]), 0);
            if (uniform_valid(p->aref)) glUniform1f(p->aref, (float)(d->rs[24] & 255));
            memcpy(p->sig, sig, sizeof sig);
        }
        if (uniform_valid(p->pc) && (!p->sent || p->psc_gen != d->psc_gen)) { glUniform4fv(p->pc, 8, &d->psc[0][0]); p->psc_gen = d->psc_gen; }
        p->sent = 1;
    }
    PH(0);
    for (int s = 0; s < MAX_STAGES; s++) {
        if (!d->tex[s] && !d->cube[s]) continue;
        glActiveTexture(GL_TEXTURE0 + (GLenum)s);
        if (d->cube[s]) glBindTexture(GL_TEXTURE_CUBE_MAP,diag_cube_gl(d->cube[s]));
        else glBindTexture(GL_TEXTURE_2D, tex_gl(d->tex[s]));
        apply_sampler(d, s);
    }
    glActiveTexture(GL_TEXTURE0);
    PH(1);

    Elem e[32];
    int ne = vs ? decl_elems(vs->decl, vs->ndecl, e) : fvf_elems(fvf, e);
    static int fade_trace=-1;
    if (fade_trace<0) fade_trace=getenv("SH3_DIAG_FADETRACE")!=NULL;
    if (fade_trace && g_pframe<2200 && !(g_pframe%15) && nverts<=6 && !indices) {
        rt_log("[fade] frame=%u prim=%u n=%u fvf=%x ps=%x blend=%u/%u/%u atest=%u/%u/%u tf=%08x colorop=%u alphaop=%u args=%u,%u rt=%ux%u tex=%ux%u\n",
               g_pframe,prim,nverts,d->vs,d->ps,d->rs[27],d->rs[19],d->rs[20],d->rs[15],d->rs[25],d->rs[24],d->rs[60],d->tss[0][1],d->tss[0][4],d->tss[0][5],d->tss[0][6],d->rt->w,d->rt->h,d->tex[0]?d->tex[0]->w:0,d->tex[0]?d->tex[0]->h:0);
        if (fvf==0x104u && d->tss[0][1]==5) {
            rt_log("[fade-probe] frame=%u rt_guest=%08x rt_texture=%08x source0=%08x source_gl=%u rt_gl=%u tf_location=%d vp_location=%d color_args=%u,%u,%u filters=%u,%u,%u sent=%u\n",
                   g_pframe,d->rt->com.guest,d->rt->tex?d->rt->tex->com.guest:0,
                   d->tex[0]?d->tex[0]->com.guest:0,d->tex[0]?d->tex[0]->gl:0,d->rt->fbo_tex,
                   p->tf,p->vp,d->tss[0][2],d->tss[0][3],d->tss[0][26],d->tss[0][16],d->tss[0][17],d->tss[0][18],p->sent);
            for(int k=0;k<ne;k++) if(e[k].reg==0 || e[k].reg==7) {
                unsigned stream=e[k].stream;Buffer *vb=d->stream[stream].vb;
                const uint8_t *data=up?up:(inl && inl[stream]?inl[stream]:(vb?g_mem+vb->mem:NULL));
                unsigned stride=up?up_stride:d->stream[stream].stride;
                unsigned floats=e[k].type<4?e[k].type+1:0;
                if(data && stride && floats && (up || (inl && inl[stream]) || (vb && ((uint64_t)first+nverts)*stride<=vb->size)))
                    for(unsigned j=0;j<nverts;j++) {
                        float v[4]={0};memcpy(v,data+(size_t)(first+j)*stride+e[k].off,floats*4);
                        rt_log("[fade-vertex] attr=%d vertex=%u value=%g,%g,%g,%g\n",e[k].reg,j,v[0],v[1],v[2],v[3]);
                    }
            }
        }
        for (int k=0;k<ne;k++) if (e[k].reg==5 && e[k].type==4) {
            unsigned stream=e[k].stream; Buffer *vb=d->stream[stream].vb;
            const uint8_t *data=up?up:(inl && inl[stream]?inl[stream]:(vb?g_mem+vb->mem:NULL));
            uint32_t stride=up?up_stride:d->stream[stream].stride;
            if (data && stride && (up || (inl && inl[stream]) || (vb && (uint64_t)(first+nverts)*stride<=vb->size))) {
                uint32_t color=0; memcpy(&color,data+(size_t)first*stride+e[k].off,4);
                rt_log("[fade] diffuse=%08x stride=%u\n",color,stride);
            }
        }
    }
    // VBO mode: all streams read are stable VBs. Otherwise client arrays (vitaGL copies the vertices on every draw).
    gfx_use_vbo();
    int vbo = use_vbo && !up;
    for (int i = 0; i < ne && vbo; i++) {
        Buffer *vb = d->stream[e[i].stream].vb;
        if (vb && vb->dyn) vbo = 0;
    }
    uint32_t lo = 0, hi = 0;
    IboEnt *ie = NULL;
    if (indices) {
        if (vbo && use_ibo) ie = ibo_get(d, indices, nverts, first, index32);
        if (ie) { lo = ie->lo; hi = ie->hi; index32 = 0; }
        else indices = idx_rewrite(indices, nverts, &index32, vbo, &first, &lo, &hi);
    }
    PH(4);
    uint32_t want = 0;
    for (int i = 0; i < ne; i++) {
        const uint8_t *base;
        uint32_t stride;
        if (up) { if (e[i].stream) continue; base = up; stride = up_stride; }
        else {
            Buffer *vb = d->stream[e[i].stream].vb;
            if (!vb) continue;
            if (vbo) { buf_sync(vb); base = NULL; }
            else base = inl && inl[e[i].stream] ? inl[e[i].stream] : g_mem + vb->mem;
            stride = d->stream[e[i].stream].stride;
        }
        if (!vs && !up && !stride) stride = fvf_stride(fvf);
        if (!vbo) {
            bind_vbo(0);
            base += (size_t)first * stride;
            if (ph_on && !e[i].off) g_stat_vbytes += (size_t)(indices ? hi + 1 : nverts) * stride;
        }
        base += e[i].off;
        static const GLint sz[] = {1, 2, 3, 4, 4, 4, 2, 4};
        static const GLenum ty[] = {GL_FLOAT, GL_FLOAT, GL_FLOAT, GL_FLOAT, GL_UNSIGNED_BYTE, GL_UNSIGNED_BYTE, GL_SHORT, GL_SHORT};
        uint32_t t = e[i].type & 7;
        glVertexAttribPointer((GLuint)e[i].reg, sz[t], ty[t], t == 4, (GLsizei)stride, base);
        want |= 1u << e[i].reg;
    }
    for (int r = 0; r < 16; r++) {
        uint32_t bit = 1u << r;
        if ((want & bit) && !(enabled_attrs & bit)) glEnableVertexAttribArray((GLuint)r);
        if (!(want & bit) && (enabled_attrs & bit)) glDisableVertexAttribArray((GLuint)r);
    }
    enabled_attrs = want;
    PH(2);
    apply_states(d);
    PH(3);
    if (trace_now()) {
        rt_log("[pass] frame=%u rt_guest=%08x rt_tex=%p rt_gl=%u source0=%p source1=%p\n",
               g_pframe+1,d->rt->com.guest,(void *)d->rt->tex,d->rt->fbo_tex,(void *)d->tex[0],(void *)d->tex[1]);
        rt_log("[draw] prim=%u n=%u first=%u idx=%d vs=%08x ps=%x tex0=%p(%ux%u fmt %08x) zen=%u blend=%u(%u,%u) "
               "atest=%u ref=%u cull=%u fog=%u op0=%u/%u a0=%u vp=%u,%u %ux%u rt=%ux%u "
               "zfunc=%u zwrite=%u zbias=%u stencil=%u/%u/%u\n",
               prim, nverts, first, indices != NULL, d->vs, d->ps, (void *)d->tex[0], d->tex[0] ? d->tex[0]->w : 0,
               d->tex[0] ? d->tex[0]->h : 0, d->tex[0] ? d->tex[0]->fmt : 0, d->rs[7], d->rs[27], d->rs[19], d->rs[20],
               d->rs[15], d->rs[24], d->rs[22], d->rs[28], d->tss[0][1], d->tss[0][2], d->tss[0][4], d->vp.x, d->vp.y,
               d->vp.w, d->vp.h, d->rt->w, d->rt->h, d->rs[23], d->rs[14], d->rs[47], d->rs[52], d->rs[56], d->rs[57]);
        if(ps) {
            rt_log("[ps-state] handle=%u uniform=%d generation=%u sent_generation=%u sent=%u\n%s",d->ps,p->pc,d->psc_gen,p->psc_gen,p->sent,ps->glsl_body);
            for(unsigned k=0;k<8;k++)rt_log("[ps-const] c%u=%g,%g,%g,%g\n",k,d->psc[k][0],d->psc[k][1],d->psc[k][2],d->psc[k][3]);
            for(unsigned s=0;s<4;s++) {
                Texture *t=d->tex[s];
                rt_log("[ps-texture] stage=%u guest=%08x size=%ux%u fmt=%u\n",
                    s,t?t->com.guest:0,t?t->w:0,t?t->h:0,t?t->fmt:0);
            }
        }
        if (!vs && (fvf == 0x104u || fvf == 0x204u) && up && !indices &&
            prim == 5 && nverts == 4 && up_stride >= 24 && up_stride <= 32) {
            rt_log("[fx-quad] depth=%u/%u/%u stencil=%u/%u/%u/%u/%u ref=%u masks=%x/%x color=%x alpha=%u/%u/%u\n",
                   d->rs[7], d->rs[23], d->rs[14], d->rs[52], d->rs[56], d->rs[53], d->rs[54], d->rs[55],
                   d->rs[57], d->rs[58], d->rs[59], d->rs[168], d->rs[15], d->rs[25], d->rs[24]);
            for (unsigned i = 0; i < 4; ++i) {
                float vertex[8];
                memcpy(vertex, (const uint8_t *)up + (size_t)(first + i) * up_stride, up_stride);
                rt_log("[fx-vertex] %u xy=%g,%g z=%g rhw=%g uv=%g,%g\n",
                       i, vertex[0], vertex[1], vertex[2], vertex[3], vertex[4], vertex[5]);
            }
        }
        /* Bounded pass survey only: transformed small primitives account for
           substantial fill cost in SH3. A bounding box is not shaded area or
           a batching criterion. No GPU readback or extra work outside trace. */
        if (!vs && fvf==0x144 && !indices && nverts && nverts<=6 && (prim==5 || prim==6)) {
            Buffer *vb=d->stream[0].vb;
            const uint8_t *data=up?up:(inl && inl[0]?inl[0]:(vb?g_mem+vb->mem:NULL));
            uint32_t stride=up?up_stride:d->stream[0].stride;
            if (data && stride>=28 && (up || (inl && inl[0]) || (vb && ((uint64_t)first+nverts)*stride<=vb->size))) {
                float xmin=INFINITY,ymin=INFINITY,xmax=-INFINITY,ymax=-INFINITY;
                unsigned amin=255,amax=0;int finite=1;
                for (uint32_t v=0;v<nverts;v++) {
                    const uint8_t *vertex=data+(size_t)(first+v)*stride;
                    float xy[2];uint32_t color;memcpy(xy,vertex,8);memcpy(&color,vertex+16,4);
                    finite=finite && isfinite(xy[0]) && isfinite(xy[1]);
                    if (xy[0]<xmin) xmin=xy[0];if (xy[0]>xmax) xmax=xy[0];
                    if (xy[1]<ymin) ymin=xy[1];if (xy[1]>ymax) ymax=xy[1];
                    unsigned alpha=color>>24;if (alpha<amin) amin=alpha;if (alpha>amax) amax=alpha;
                }
                if(nverts==4 && xmax-xmin>600 && ymax-ymin>200) {
                    float uv[8];for(unsigned k=0;k<4;k++)memcpy(uv+2*k,data+(size_t)(first+k)*stride+20,8);
                    rt_log("[quad-uv] %.5f,%.5f %.5f,%.5f %.5f,%.5f %.5f,%.5f\n",uv[0],uv[1],uv[2],uv[3],uv[4],uv[5],uv[6],uv[7]);
                }
                rt_log("[small144] frame=%u tex_guest=%08x n=%u finite=%d bbox=%.2f,%.2f,%.2f,%.2f alpha=%u,%u\n",
                       g_pframe+1,d->tex[0]?d->tex[0]->com.guest:0,nverts,finite,xmin,ymin,xmax,ymax,amin,amax);
            } else rt_log("[small144] frame=%u unavailable=1\n",g_pframe+1);
        }
#ifndef __vita__
        if (getenv("SH3_DIAG_TRACE_LINES") && prim==3 && !indices && !vs && fvf==0x144 && nverts<=6) {
            rt_log("[diag-line] alphafunc=%u alphaop=%u alphaarg1=%u alphaarg2=%u\n",
                   d->rs[25],d->tss[0][4],d->tss[0][5],d->tss[0][6]);
            Buffer *vb=d->stream[0].vb;
            const uint8_t *data=up ? up : (inl && inl[0] ? inl[0] : (vb ? g_mem+vb->mem : NULL));
            uint32_t stride=up ? up_stride : d->stream[0].stride;
            uint64_t end=((uint64_t)first+nverts)*stride;
            if (data && stride>=20 && (up || (vb && end<=vb->size))) for (uint32_t v=0;v<nverts;v++) {
                const uint8_t *p=data+(size_t)(first+v)*stride;
                float xy[2]; uint32_t color;
                memcpy(xy,p,sizeof xy); memcpy(&color,p+16,4);
                rt_log("[diag-line] v=%u xy=%.2f,%.2f argb=%08x\n",v,xy[0],xy[1],color);
            }
        }
#endif
        if (ctl_trace_n <= 1) {
            const uint8_t *vb = up ? up : (d->stream[0].vb ? g_mem + d->stream[0].vb->mem : NULL);
            uint32_t st = up ? up_stride : d->stream[0].stride;
            for (uint32_t v = 0; vb && v < nverts && v < 6; v++) {
                const float *f = (const float *)(vb + (size_t)(first + v) * st);
                rt_log("   vtx%u (stride %u): %g %g %g | %g %g %g %g\n", v, st, f[0], f[1], f[2], f[3], f[4], f[5], f[6]);
            }
            rt_log("   c[94]=%g %g %g %g c[95]=%g %g %g %g c[1]=%g %g %g %g\n", d->vsc[94][0], d->vsc[94][1], d->vsc[94][2],
                   d->vsc[94][3], d->vsc[95][0], d->vsc[95][1], d->vsc[95][2], d->vsc[95][3], d->vsc[1][0], d->vsc[1][1],
                   d->vsc[1][2], d->vsc[1][3]);
        }
        if (d->tex[0])
            rt_log("   tex0 tfmt=%d levels=%u usage=%x gl=%u mag/min/mip=%u/%u/%u wrap=%u/%u glerr_checked=%d glerr=%x\n", d->tex[0]->tfmt, d->tex[0]->levels, d->tex[0]->usage,
                   d->tex[0]->gl, d->tss[0][16], d->tss[0][17], d->tss[0][18], d->tss[0][13], d->tss[0][14], ctl_trace_n <= 1, ctl_trace_n <= 1 ? glGetError() : 0);
        if (ctl_trace_n <= 1 && d->tex[0] && d->tex[0]->lv[0]->mem && d->tex[0]->fmt == FMT_A8R8G8B8) {
            SDL_Surface *ts = SDL_CreateRGBSurfaceWithFormatFrom(g_mem + d->tex[0]->lv[0]->mem, (int)d->tex[0]->w, (int)d->tex[0]->h, 32,
                                                                 (int)d->tex[0]->w * 4, SDL_PIXELFORMAT_ARGB8888);
            SDL_SaveBMP(ts, "tex0_dump.bmp");
            SDL_FreeSurface(ts);
        }
        if (ctl_trace_n <= 1 && d->tex[0] && d->tex[0]->lv[0]->mem) {
            const uint32_t *px = (const uint32_t *)(g_mem + d->tex[0]->lv[0]->mem);
            uint32_t n4 = d->tex[0]->w * d->tex[0]->h;
            rt_log("   tex0 pixels: %08x %08x %08x %08x | milieu %08x %08x | fin %08x (dirty=%x)\n", px[0], px[1], px[1000], px[2000],
                   px[n4 / 2], px[n4 / 2 + 321], px[n4 - 1], d->tex[0]->dirty);
        }
        if (ctl_trace_n <= 1 && up && !vs && (fvf & 0xe) == 0x4)
            for (uint32_t v = 0; v < nverts && v < 4; v++) {
                const float *f = (const float *)(up + v * up_stride);
                rt_log("   v%u: %.1f %.1f %.3f rhw=%.3f col=%08x\n", v, f[0], f[1], f[2], f[3], *(const uint32_t *)(f + 4));
            }
    }
    if (indices && !ie) bind_ibo(0);
#ifdef __vita__
    if (rtt_active) ++rtt_draws;
#endif
#ifdef __vita__   // known bounds: vitaGL skips its index scan
    if (indices) glDrawRangeElements(prim_of(prim), lo, hi, (GLsizei)nverts, index32 ? GL_UNSIGNED_INT : GL_UNSIGNED_SHORT, ie ? NULL : indices);
#else
    rtt_draws++;
    if (skip_pass>=0 && (int)pass_idx==skip_pass) return;
    if (indices) glDrawElements(prim_of(prim), (GLsizei)nverts, index32 ? GL_UNSIGNED_INT : GL_UNSIGNED_SHORT, ie ? NULL : indices);
#endif
    else glDrawArrays(prim_of(prim), vbo ? (GLint)first : 0, (GLsizei)nverts);
    PH(5);
    if(footer_stride)draw_impl(d,5,0,4,2,NULL,0,0,footer_vertices,footer_stride,NULL);
}

// sceGxm crashes (data abort) if the clip region exceeds the target: clamp it to the render surface.
static void scissor_clamped(Device *d, int32_t x, int32_t y, int32_t w, int32_t h) {
    unsigned sh = rt_shift(d->rt);
    int32_t rw = (int32_t)(d->rt->w >> sh), rh = (int32_t)(d->rt->h >> sh);
    x >>= sh; y >>= sh; w >>= sh; h >>= sh;
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > rw) w = rw - x;
    if (y + h > rh) h = rh - y;
    if (w < 0) w = 0;
    if (h < 0) h = 0;
    glScissor(x, y, w, h);
}
/* One contiguous additive, non-depth-tested low-alpha effect run per frame.
 * Every original draw/shader is retained; only this optional layer is sampled
 * at half resolution. It is a visual quality experiment, not exact rendering.
 * Flush before any non-draw side effect so command/resource order is retained. */
void gfx_fx_flush(void) {
#ifdef __vita__
    if (!fx_active) return;
    gfx_state_invalidate();
    fx_active=0;fx_closed_frame=g_pframe;++fx_batches;
    gfx_bind_target(&fx_device);
    glViewport(0,0,(GLsizei)fx_device.vp.w,(GLsizei)fx_device.vp.h);
    glDisable(GL_DEPTH_TEST);glDepthMask(GL_FALSE);glDisable(GL_STENCIL_TEST);
    glDisable(GL_CULL_FACE);glDisable(GL_SCISSOR_TEST);glDisable(GL_POLYGON_OFFSET_FILL);
    glColorMask(1,1,1,1);glEnable(GL_BLEND);glBlendEquation(GL_FUNC_ADD);glBlendFunc(GL_ONE,GL_ONE);
    use_prog(g_copy_prog);bind_vbo(0);bind_ibo(0);glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D,fx_texture);
    static const float quad[]={-1,-1,0,0,1,-1,1,0,-1,1,0,1,1,1,1,1};
    for (int r=0;r<16;r++) if (enabled_attrs&(1u<<r)) glDisableVertexAttribArray((GLuint)r);
    enabled_attrs=1;glEnableVertexAttribArray(0);glVertexAttribPointer(0,4,GL_FLOAT,GL_FALSE,16,quad);
    glDrawArrays(GL_TRIANGLE_STRIP,0,4);
#endif
}
static int fx_route(Device *d,uint32_t prim,uint32_t first,uint32_t n,const void *indices,
                    const uint8_t *up,uint32_t up_stride,const uint8_t *const *inl) {
#ifdef __vita__
    if (!(g_diag_perf&262144u)) return 0;
    static int gate_logged;
    if (!gate_logged && d->rt && d->vs==0x144u && !d->ps && prim==6 && n==6 && !d->rs[7] && d->rs[19]==5 && d->rs[20]==2) {
        unsigned textures=0,cubes=0;
        for (int s=0;s<MAX_STAGES;s++) { if (d->tex[s]) textures|=1u<<s;if (d->cube[s]) cubes|=1u<<s; }
        rt_log("[fxhalf-gate] rt=%ux%u fmt=%u vp=%u,%u,%u,%u cw=%u blendop=%u stencil=%u fog=%u bias=%u tss0=%u,%u,%u/%u,%u,%u stage1=%u textures=%x cubes=%x feedback=%u\n",
            d->rt->w,d->rt->h,d->rt->fmt,d->vp.x,d->vp.y,d->vp.w,d->vp.h,d->rs[168],d->rs[171],d->rs[52],d->rs[28],d->rs[47],
            d->tss[0][1],d->tss[0][2],d->tss[0][3],d->tss[0][4],d->tss[0][5],d->tss[0][6],d->tss[1][1],textures,cubes,d->tex[0]==d->rt->tex);
        gate_logged=1;
    }
    int target_size=d->rt && d->rt->w==960 && d->rt->h==544;
#ifdef SH3_FXHALF_RES_LAB
    target_size=target_size || (d->rt && d->rt->w==720 && d->rt->h==408);
#endif
    int eligible=target_size && d->rt->fmt==21 && !rt_shift(d->rt) &&
        !d->vp.x && !d->vp.y && d->vp.w==d->rt->w && d->vp.h==d->rt->h &&
        d->vs==0x144u && !d->ps && !indices && n>=3 && n<=6 && (prim==5 || prim==6) &&
        !d->rs[7] && !d->rs[52] && d->rs[27] && d->rs[19]==5 && d->rs[20]==2 &&
        d->rs[171]==1 && d->rs[168]==15 && !d->rs[28] && !d->rs[47] &&
        /* CURRENT(1) is the initial diffuse at stage0, equivalent to DIFFUSE(0).
           No modifier bits accepted; the original shader still performs it. */
        d->tss[0][1]==4 && d->tss[0][2]==2 && d->tss[0][3]<=1 &&
        d->tss[0][4]==4 && d->tss[0][5]==2 && d->tss[0][6]<=1 && d->tss[1][1]==1 &&
        d->tex[0] && d->tex[0]!=d->rt->tex && !d->cube[0];
    /* Stage 1 is DISABLED (checked above), so D3D ignores every later stage: a texture left bound there (the hospital
       fog particles keep a stale cube map at stage 3) does not take part in the draw. Was "any texture on stages 1+",
       which kept fxhalf off in every hospital room (2026-11-18 drawlog 28:183: 180 additive quads in one run, 0 routed). */
    if (eligible) {
        Buffer *vb=d->stream[0].vb;
        const uint8_t *data=up?up:(inl && inl[0]?inl[0]:(vb?g_mem+vb->mem:NULL));
        uint32_t stride=up?up_stride:d->stream[0].stride;
        eligible=data && stride>=28 && (up || (inl && inl[0]) || (vb && ((uint64_t)first+n)*stride<=vb->size));
        for (uint32_t v=0;eligible && v<n;v++) {
            uint32_t color;memcpy(&color,data+(size_t)(first+v)*stride+16,4);
            if ((color>>24)>64) eligible=0;
        }
    }
    if (fx_active && (!eligible || d->rt!=fx_device.rt)) gfx_fx_flush();
    if (!eligible || (!fx_active && fx_closed_frame==g_pframe)) return 0;
    if (!fx_active) {
        gfx_state_invalidate();
        unsigned divisor=(g_diag_perf & 16777216u) ? 4u : 2u;
        unsigned width=d->vp.w/divisor,height=d->vp.h/divisor;
        if (fx_texture && (fx_width!=width || fx_height!=height)) {
            glDeleteFramebuffers(1,&fx_fbo);glDeleteTextures(1,&fx_texture);
            fx_fbo=fx_texture=0;
        }
        if (!fx_texture) {
            glGenTextures(1,&fx_texture);glBindTexture(GL_TEXTURE_2D,fx_texture);
            glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_S,GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_T,GL_CLAMP_TO_EDGE);
            glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA,(GLsizei)width,(GLsizei)height,0,GL_RGBA,GL_UNSIGNED_BYTE,NULL);
            glGenFramebuffers(1,&fx_fbo);glBindFramebuffer(GL_FRAMEBUFFER,fx_fbo);
            glFramebufferTexture2D(GL_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_TEXTURE_2D,fx_texture,0);
            if (glCheckFramebufferStatus(GL_FRAMEBUFFER)!=GL_FRAMEBUFFER_COMPLETE)
                rt_fatal("fxhalf framebuffer incomplete");
            fx_width=width;fx_height=height;
            rt_log("[fxhalf] isolated additive effect target %ux%u created; scene stays%ux%u\n",width,height,d->vp.w,d->vp.h);
        } else glBindFramebuffer(GL_FRAMEBUFFER,fx_fbo);
        fx_device=*d;fx_active=1;
        glViewport(0,0,(GLsizei)fx_width,(GLsizei)fx_height);glDisable(GL_SCISSOR_TEST);glColorMask(1,1,1,1);
        glClearColor(0,0,0,0);glClear(GL_COLOR_BUFFER_BIT);
    }
    ++fx_draws;return 1;
#else
    (void)d;(void)prim;(void)first;(void)n;(void)indices;(void)up;(void)up_stride;(void)inl;return 0;
#endif
}
void gfx_clear(Device *d, uint32_t flags, uint32_t color, float z, uint32_t stencil, int nrects, const int32_t *rects) {
    gfx_state_invalidate();
    gfx_fx_flush();
    SLOW("clear", flags, nrects);
    extern unsigned g_stat_clears;
    g_stat_clears++;
    GLbitfield m = 0;
    if (flags & 1) { m |= GL_COLOR_BUFFER_BIT; glColorMask(1, 1, 1, 1);
        glClearColor((float)((color >> 16) & 255) / 255.f, (float)((color >> 8) & 255) / 255.f,
                     (float)(color & 255) / 255.f, (float)(color >> 24) / 255.f); }
    if ((flags & 6) && d->ds) lazy_depth_need();
    if ((flags & 2) && d->ds) { m |= GL_DEPTH_BUFFER_BIT; glDepthMask(1); glClearDepth(z); }
    if ((flags & 4) && d->ds) { m |= GL_STENCIL_BUFFER_BIT; glStencilMask(0xff); glClearStencil((GLint)stencil); }
    if (!m) return;
    glEnable(GL_SCISSOR_TEST);
    if (nrects <= 0 || !rects) {
        scissor_clamped(d, (int32_t)d->vp.x, (int32_t)d->vp.y, (int32_t)d->vp.w, (int32_t)d->vp.h);
        glClear(m);
    } else
        for (int i = 0; i < nrects; i++) {
            int32_t x1 = rects[4 * i], y1 = rects[4 * i + 1], x2 = rects[4 * i + 2], y2 = rects[4 * i + 3];
            scissor_clamped(d, x1, y1, x2 - x1, y2 - y1);
            glClear(m);
        }
    glDisable(GL_SCISSOR_TEST);
}

// ---------- copies / relecture ----------
static void bind_read(Surface *s) {
    int level;
    GLuint t = color_tex(s, &level);
    glBindFramebuffer(GL_FRAMEBUFFER, g_readfbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, t, level);
}
static void from_rgba(uint32_t fmt, const uint8_t *src, uint8_t *dst, uint32_t n) {
    for (uint32_t i = 0; i < n; i++, src += 4) {
        if (fmt == FMT_R5G6B5) {
            uint16_t v = (uint16_t)((src[0] >> 3) << 11 | (src[1] >> 2) << 5 | src[2] >> 3);
            memcpy(dst, &v, 2); dst += 2;
        } else { dst[0] = src[2]; dst[1] = src[1]; dst[2] = src[0]; dst[3] = src[3]; dst += 4; }
    }
}
// GL -> guest memory readback. The game allocates the destination, the consumer reads (prep_call: everything before is replayed first).
typedef struct { Surface *src, *dst; } RbArg;
static void rb_cb(void *a) {
    gfx_fx_flush();
    RbArg *r = a;
    static unsigned tiny_reads;
    unsigned tiny_id = r->src->w==1 && r->src->h==1 ? ++tiny_reads : 0;
    int trace_tiny = tiny_id && tiny_id<=16;
    if (trace_tiny) { rt_log("[tiny-read] id=%u stage=begin frame=%u src=%p dst=%p fmt=%u pending=%p\n", tiny_id, g_pframe, (void *)r->src, (void *)r->dst, r->src->fmt, (void *)r->src->pending); fflush(stderr); }
    uint8_t *tmp = malloc((size_t)r->src->w * r->src->h * 4);
    if (!tmp) rt_fatal("[diag] readback temporary allocation failed");
#ifdef __vita__
    /* Commit this producer before binding the separate read FBO. On Vita the
       inventory thumbnail otherwise reads stale zeroes. Keep an opt-out for
       comparing other 64x64 readbacks as the port is still experimental. */
    if (!ENVFLAG("SH3_DIAG_READBACK_NO_FINISH") && r->src->w==64 && r->src->h==64 &&
        r->src->fmt==FMT_A8R8G8B8 && r->src==r->dst) {
        glFinish();
        if (ENVFLAG("SH3_DIAG_READBACK_TRACE")) rt_log("[readback-trace] finished producer before 64x64 read\n");
    }
#endif
    if (trace_tiny) { rt_log("[tiny-read] id=%u stage=before-bind\n", tiny_id); fflush(stderr); }
    bind_read(r->src);
    if (trace_tiny) { rt_log("[tiny-read] id=%u stage=bound\n", tiny_id); fflush(stderr); }
    int trace=ENVFLAG("SH3_DIAG_READBACK_TRACE") && r->src->w==64 && r->src->h==64;
    GLenum prior=trace ? glGetError() : GL_NO_ERROR;
    if (trace) memset(tmp,0,(size_t)r->src->w*r->src->h*4);
    glReadPixels(0, 0, (GLsizei)r->src->w, (GLsizei)r->src->h, GL_RGBA, GL_UNSIGNED_BYTE, tmp);
#ifdef __vita__
    if (r->src->w == 1 && r->src->h == 1 && r->src->emu_valid) memcpy(tmp, &r->src->emu_px, 4);   // emulated lens probe
#endif
    if (trace_tiny) { rt_log("[tiny-read] id=%u stage=pixels rgba=%02x%02x%02x%02x\n", tiny_id, tmp[0], tmp[1], tmp[2], tmp[3]); fflush(stderr); }
    if (tiny_id>16 && tiny_id<=4000) rt_log("[tiny-px] id=%u frame=%u rgba=%02x%02x%02x%02x\n", tiny_id, g_pframe, tmp[0], tmp[1], tmp[2], tmp[3]);   /* lens/lamp probe values, bounded */
    if (trace) {
        GLenum error=glGetError(); uint64_t sum[4]={0}; unsigned nonzero=0;
        for (unsigned i=0;i<64u*64u;i++) {
            for (unsigned j=0;j<4;j++) sum[j]+=tmp[4*i+j];
            if (tmp[4*i] || tmp[4*i+1] || tmp[4*i+2]) nonzero++;
        }
        rt_log("[readback-trace] 64x64 previous_gl=%x read_gl=%x nonzero_rgb=%u sums=%llu,%llu,%llu,%llu\n",
               (unsigned)prior,(unsigned)error,nonzero,(unsigned long long)sum[0],(unsigned long long)sum[1],
               (unsigned long long)sum[2],(unsigned long long)sum[3]);
    }
    from_rgba(r->dst->fmt, tmp, g_mem + r->dst->mem, r->src->w * r->src->h);
    free(tmp);
    gfx_bind_target(g_pdev);
    if (trace_tiny) { rt_log("[tiny-read] id=%u stage=restored\n", tiny_id); fflush(stderr); }
}
/* Deferred 1x1 readback for the lensflare occlusion tests (perf lensasync, docs: B13). The game draws a test point into a
   1x1 target and locks it at once; each lock waits for the GPU to finish the whole frame so far (~6 ms, two per frame in
   the park reached through the intro). Here the lock returns the pixel read after the previous frame's test and queues the
   readback of the current one: the occlusion value is one frame old. Only the 1x1 usage=1 surfaces locked from the two
   known lensflare call sites use it; the very first lock of a surface is synchronous. */
typedef struct { Surface *s; uint32_t px; volatile int valid; } LensSlot;
static LensSlot lens_slot[4];
typedef struct { LensSlot *slot; Surface *s; } LensJob;
static void lens_job(void *a) {
    LensJob *j = a;
    gfx_fx_flush();
    uint8_t tmp[4] = {0}, out[4] = {0};
    bind_read(j->s);
    glReadPixels(0, 0, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, tmp);
#ifdef __vita__
    if (j->s->emu_valid) memcpy(tmp, &j->s->emu_px, 4);   // emulated lens probe
#endif
    from_rgba(j->s->fmt, tmp, out, 1);
    uint32_t v; memcpy(&v, out, 4);
    { static unsigned n; if (++n<=4000) rt_log("[tiny-px] async n=%u frame=%u rgba=%02x%02x%02x%02x\n", n, g_pframe, tmp[0], tmp[1], tmp[2], tmp[3]); }
    __atomic_store_n(&j->slot->px, v, __ATOMIC_RELAXED);
    __atomic_store_n(&j->slot->valid, 1, __ATOMIC_RELEASE);
    free(j);
}
int surf_lens_lock(Surface *s) {
    if (s->pending || !s->mem) return 0;   /* first lock or unusual state: caller takes the normal synchronous path */
    LensSlot *slot = NULL;
    for (int i = 0; i < 4; i++) if (lens_slot[i].s == s) slot = &lens_slot[i];
    if (!slot) {
        for (int i = 0; i < 4 && !slot; i++) if (!lens_slot[i].s) { slot = &lens_slot[i]; slot->s = s; slot->valid = 0; }
        if (!slot) return 0;
    }
    if (!__atomic_load_n(&slot->valid, __ATOMIC_ACQUIRE)) return 0;   /* nothing cached yet: synchronous read primes the cache */
    uint32_t v = __atomic_load_n(&slot->px, __ATOMIC_RELAXED);
    memcpy(g_mem + s->mem, &v, 4);   /* the previous frame's result, as the game would have read it */
    LensJob *j = malloc(sizeof *j);
    if (!j) return 1;
    j->slot = slot; j->s = s;
    prep_call_async(lens_job, j);
    return 1;
}
void surf_lens_prime(Surface *s) {   /* called after a synchronous readback of a lens surface: store its value as the first cache entry */
    LensSlot *slot = NULL;
    for (int i = 0; i < 4; i++) if (lens_slot[i].s == s) slot = &lens_slot[i];
    if (!slot) for (int i = 0; i < 4 && !slot; i++) if (!lens_slot[i].s) { slot = &lens_slot[i]; slot->s = s; slot->valid = 0; }
    if (!slot || !s->mem) return;
    uint32_t v; memcpy(&v, g_mem + s->mem, 4);
    __atomic_store_n(&slot->px, v, __ATOMIC_RELAXED);
    __atomic_store_n(&slot->valid, 1, __ATOMIC_RELEASE);
}
void surf_readback(Surface *s) {
    SLOW("readback", s->w, s->h);
    if (!surf_is_gl(s) || rt_shift(s)) return;
    if (!s->mem) {
        rt_log("[readback] %ux%u fmt %u bb=%d\n", s->w, s->h, s->fmt, s->is_backbuffer);
        s->mem = gheap_calloc(fmt_level_size(s->fmt, s->w, s->h));
        if (!s->mem) return;
    }
    RbArg a = {s, s};
    extern unsigned g_ct_rb, g_ct_rb_tiny; extern uint64_t g_ct_rb_ticks;   // game-thread counters, see diag_perf_report
    uint64_t rb_t0 = SDL_GetPerformanceCounter();
    prep_call(rb_cb, &a);
    g_ct_rb_ticks += SDL_GetPerformanceCounter() - rb_t0; g_ct_rb++; if (s->w == 1 && s->h == 1) g_ct_rb_tiny++;
}

// GL -> system surface copy: deferred as long as nobody reads the destination back (the game copies
// two 4096x4096 textures at startup without ever reading them back: saves 2 x 64 MB).
void surf_flush_pending(Surface *dst) {
    SLOW("flush_pending", dst->w, dst->h);
    Surface *src = dst->pending;
    if (!src) return;
    dst->pending = NULL;
    dst->mem = gheap_calloc(fmt_level_size(dst->fmt, dst->w, dst->h));
    if (dst->mem) { RbArg a = {src, dst}; prep_call(rb_cb, &a); }
    d3d_release(&src->com);
}

// Consumer: per-draw copy of a quad (rects: x1,y1,x2,y2; points: destination, always filled in).
void gfx_copy_gl(Surface *src, Surface *dst, uint32_t n, const int32_t *rects, const int32_t *points) {
    gfx_state_invalidate();
    gfx_fx_flush();
    for (uint32_t i = 0; i < n; i++) {
        int32_t x = rects[4 * i], y = rects[4 * i + 1], w = rects[4 * i + 2] - x, h = rects[4 * i + 3] - y;
        int32_t dx = points[2 * i], dy = points[2 * i + 1];
        // ponytail: GPU blit instead of glCopyTexSubImage2D (vitaGL does it via CPU readback + GPU allocation: slow and OOM)
        int sl, dl;
        GLuint st = color_tex(src, &sl), dt = color_tex(dst, &dl);
        // One FBO per destination: re-attaching a texture of another size recreates the sceGxm target (~2 ms on Vita).
        static struct { GLuint fbo, dt; int dl; } cp[16];
        static int cp_n, cp_seen;
        if (cp_seen != g_att_stale) { cp_seen = g_att_stale; for (int k = 0; k < cp_n; k++) cp[k].dt = 0; }
        int k = 0;
        while (k < cp_n && !(cp[k].dt == dt && cp[k].dl == dl)) k++;
        if (k == cp_n) {
            for (k = 0; k < cp_n && cp[k].dt; k++) {}
            if (k == cp_n) k = cp_n < 16 ? cp_n++ : 15;
            if (!cp[k].fbo) glGenFramebuffers(1, &cp[k].fbo);
            glBindFramebuffer(GL_FRAMEBUFFER, cp[k].fbo);
            glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, dt, dl);
            cp[k].dt = dt; cp[k].dl = dl;
        } else glBindFramebuffer(GL_FRAMEBUFFER, cp[k].fbo);
        static int tc = -1;
        if (tc < 0) tc = getenv("SH4_TRACE_COPY") != NULL;
        if (tc) rt_log("[copy] viewport %d,%d %dx%d src_tex=%u dst_tex=%u dl=%d\n", dx, dy, w, h, st, dt, dl);
        { unsigned dsh = surf_is_gl(dst) ? rt_shift(dst) : 0; glViewport(dx >> dsh, dy >> dsh, w >> dsh, h >> dsh); }
        glDisable(GL_SCISSOR_TEST); glDisable(GL_DEPTH_TEST); glDisable(GL_BLEND); glDisable(GL_CULL_FACE);
        glDisable(GL_STENCIL_TEST);
        glColorMask(1, 1, 1, 1);
        use_prog(g_copy_prog);
        bind_vbo(0);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, st);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        float u0 = (float)x / (float)src->w, u1 = (float)(x + w) / (float)src->w;
        float v0 = (float)y / (float)src->h, v1 = (float)(y + h) / (float)src->h;
        float quad[] = {-1, -1, u0, v0, 1, -1, u1, v0, -1, 1, u0, v1, 1, 1, u1, v1};
        for (int r = 0; r < 16; r++) if (enabled_attrs & (1u << r)) glDisableVertexAttribArray((GLuint)r);
        enabled_attrs = 1;
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(0, 4, GL_FLOAT, GL_FALSE, 16, quad);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
        gfx_bind_target(g_pdev);
    }
}

#define CP_CHUNK 128
void surf_copy_rects(Surface *src, uint32_t rects, uint32_t n, Surface *dst, uint32_t points) {
    SLOW("copy_rects", src->w, dst->w);
    HOSTFN;
    uint64_t t_cp = SDL_GetPerformanceCounter();
    if (src->pending) surf_flush_pending(src);
    if (dst->pending) { d3d_release(&dst->pending->com); dst->pending = NULL; }
    if (surf_is_gl(src) && !surf_is_gl(dst) && !dst->tex && !dst->mem && n <= 1 && !points && src->w == dst->w &&
        src->h == dst->h && src->fmt == dst->fmt && fmt_level_size(dst->fmt, dst->w, dst->h) >= (4u << 20) &&
        !rt_shift(src) && (!rects || (R32(rects) == 0 && R32(rects + 4) == 0 && R32(rects + 8) == src->w && R32(rects + 12) == src->h))) {
        d3d_addref(src);
        dst->pending = src;
        return;
    }
#ifdef __vita__
    // GPU -> system memory copies (freeze/pause capture: 512/256/128 render targets into system surfaces, the 4096 one is dropped anyway)
    // are read-backs in the middle of the frame: on the Vita they break the scene of the FBO being drawn and half of what was drawn
    // is lost (textures vanish when interacting with an item / opening the Start menu, flat gray patches, one black frame). Nothing reads
    // these copies back on the console: skipped. SH4_CPREAD=1 restores them.
    { static int rd = -1; if (rd < 0) rd = getenv("SH4_CPREAD") != NULL; if (!rd && surf_is_gl(src) && !surf_is_gl(dst) && !dst->tex) return; }
#endif
    if (ENVFLAG("SH4_TRACE_COPY") && (src->w >= 512 || dst->w >= 512)) rt_log("[copy] frame %u src %p %ux%u fmt %u tex=%d usage=%u -> dst %p %ux%u tex=%d n=%u\n", g_frame, (void *)src, src->w, src->h, src->fmt, src->tex != NULL, src->usage, (void *)dst, dst->w, dst->h, dst->tex != NULL, n);
    int one = 0;
    if (!n || !rects) { n = 1; one = 1; }
    if (surf_is_gl(src) && (surf_is_gl(dst) || dst->tex)) {   // GPU blit: records for the consumer
        for (uint32_t i0 = 0; i0 < n; i0 += CP_CHUNK) {
            uint32_t m = n - i0 < CP_CHUNK ? n - i0 : CP_CHUNK;
            int32_t rc[CP_CHUNK * 4], pt[CP_CHUNK * 2];
            for (uint32_t j = 0; j < m; j++) {
                uint32_t i = i0 + j;
                int32_t x = 0, y = 0, x2 = (int32_t)src->w, y2 = (int32_t)src->h;
                if (!one) {
                    x = (int32_t)R32(rects + 16 * i); y = (int32_t)R32(rects + 16 * i + 4);
                    x2 = (int32_t)R32(rects + 16 * i + 8); y2 = (int32_t)R32(rects + 16 * i + 12);
                }
                rc[4 * j] = x; rc[4 * j + 1] = y; rc[4 * j + 2] = x2; rc[4 * j + 3] = y2;
                pt[2 * j] = points ? (int32_t)R32(points + 8 * i) : x;
                pt[2 * j + 1] = points ? (int32_t)R32(points + 8 * i + 4) : y;
            }
            prep_copy(src, dst, m, rc, pt);
        }
        g_stat_cp[0]++;
        g_t_copy += SDL_GetPerformanceCounter() - t_cp;
        return;
    }
    if (dst->tex) { extern int g_lockw_site; extern uint32_t g_lockw_flags; g_lockw_site = 3; g_lockw_flags = 0; prep_wait_use(dst->tex->use); }   // the consumer may still read the destination's memory
    for (uint32_t i = 0; i < n; i++) {
        int32_t x = 0, y = 0, w = (int32_t)src->w, h = (int32_t)src->h, dx, dy;
        if (!one) {
            x = (int32_t)R32(rects + 16 * i); y = (int32_t)R32(rects + 16 * i + 4);
            w = (int32_t)R32(rects + 16 * i + 8) - x; h = (int32_t)R32(rects + 16 * i + 12) - y;
        }
        dx = points ? (int32_t)R32(points + 8 * i) : x;
        dy = points ? (int32_t)R32(points + 8 * i + 4) : y;
        if (surf_is_gl(src)) { surf_readback(src); g_stat_cp[1]++; } else g_stat_cp[2]++;
        uint32_t bpp = fmt_bpp(src->fmt) / 8, sp_ = fmt_pitch(src->fmt, src->w), dp = fmt_pitch(dst->fmt, dst->w);
        if (!src->mem || !bpp || src->fmt != dst->fmt) { d3d_log_once("CopyRects: unsupported case"); continue; }
        if (!dst->mem) {
            rt_log("[copyrects] f=%u src %ux%u fmt %u gl=%d -> dst %ux%u fmt %u (rect %dx%d)\n", g_pframe, src->w, src->h, src->fmt,
                   surf_is_gl(src), dst->w, dst->h, dst->fmt, w, h);
            // Vita: the guest heap does not have 64 MB free for a 4096x4096 system surface; a failed allocation would return
            // 0 and the copy would overwrite the game image (.text) at address 0. This copy is dropped (never read back at startup).
            if (fmt_level_size(dst->fmt, dst->w, dst->h) >= (16u << 20) || !(dst->mem = gheap_calloc(fmt_level_size(dst->fmt, dst->w, dst->h)))) {
                d3d_log_once("CopyRects: destination too large, copy skipped");
                continue;
            }
        }
        for (int32_t r = 0; r < h; r++)
            memcpy(mb + dst->mem + (uint32_t)(dy + r) * dp + (uint32_t)dx * bpp,
                   mb + src->mem + (uint32_t)(y + r) * sp_ + (uint32_t)x * bpp, (size_t)w * bpp);
        if (dst->tex) __atomic_fetch_or(&dst->tex->dirty, 1 << dst->level, __ATOMIC_RELAXED);
    }
    g_t_copy += SDL_GetPerformanceCounter() - t_cp;
}

// ---------- init / present ----------
#ifndef __vita__
#define GL_X(type, name) type p_##name;
GL_FUNCS(GL_X)
#undef GL_X
void gl_load(void) {
#define GL_L(type, name) p_##name = (type)SDL_GL_GetProcAddress(#name); if (!p_##name) rt_fatal("OpenGL : %s manquante", #name);
    GL_FUNCS(GL_L)
#undef GL_L
}
#endif

void plat_gl_init(void);
void plat_gl_swap(void);
void plat_drawable_size(int *w, int *h);

void gfx_init(Device *d) {
    static int once;
    if (!once) {
        once = 1;
        plat_gl_init();
        gl_load();
        glGenFramebuffers(1, &g_readfbo);
        const char *vs = GLSL_PRE_VS "attribute vec4 aV0;\nvarying vec2 uv;\n"
                         "void main() { uv = vec2(aV0.z, 1.0 - aV0.w); gl_Position = vec4(aV0.xy, 0.0, 1.0); }\n";
        const char *fs = GLSL_PRE_FS "varying vec2 uv;\nuniform sampler2D s0;\n"
                         "void main() { gl_FragColor = vec4(texture2D(s0, uv).xyz, 1.0); }\n";
        g_blit_prog = link(vs, fs);
        // gamma ramp (SetGammaRamp: Brightness/Gamma options) approximated per channel as o + s * x^e, applied in the final blit
        const char *gfs = GLSL_PRE_FS "varying vec2 uv;\nuniform sampler2D s0;\nuniform vec4 go, gs, ge;\n"
                          "void main() { gl_FragColor = vec4(go.xyz + gs.xyz * pow(max(texture2D(s0, uv).xyz, vec3(0.0001)), ge.xyz), 1.0); }\n";
        g_gamma_prog = link(vs, gfs);
        use_prog(g_gamma_prog);
        glUniform1i(glGetUniformLocation(g_gamma_prog, "s0"), 0);

        use_prog(g_blit_prog);
        glUniform1i(glGetUniformLocation(g_blit_prog, "s0"), 0);
        const char *cvs = GLSL_PRE_VS "attribute vec4 aV0;\nvarying vec2 uv;\n"
                          "void main() { uv = aV0.zw; gl_Position = vec4(aV0.xy, 0.0, 1.0); }\n";
        const char *cfs = GLSL_PRE_FS "varying vec2 uv;\nuniform sampler2D s0;\n"
                          "void main() { gl_FragColor = texture2D(s0, uv); }\n";
        g_copy_prog = link(cvs, cfs);
        use_prog(g_copy_prog);
        glUniform1i(glGetUniformLocation(g_copy_prog, "s0"), 0);
        rt_log("[gl] %s / %s\n", (const char *)glGetString(GL_RENDERER), (const char *)glGetString(GL_VERSION));
    }
    if (!d->fbo) glGenFramebuffers(1, &d->fbo);
    gfx_bind_target(d);
}

unsigned g_stat_draws, g_stat_progs, g_stat_clears, g_frame;   // g_frame: presents issued by the game
static void dump_backbuffer_named(Device *d, const char *name) {
    int level;
    uint32_t w = d->bb_w, h = d->bb_h;
    uint8_t *px = malloc((size_t)w * h * 4);
    GLuint t = color_tex(d->backbuffer, &level);
    glBindFramebuffer(GL_FRAMEBUFFER, g_readfbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, t, 0);
    glReadPixels(0, 0, (GLsizei)w, (GLsizei)h, GL_RGBA, GL_UNSIGNED_BYTE, px);
    for (size_t i = 3; i < (size_t)w * h * 4; i += 4) px[i] = 255;   // no alpha in an X8R8G8B8 back buffer
    SDL_Surface *s = SDL_CreateRGBSurfaceWithFormatFrom(px, (int)w, (int)h, 32, (int)w * 4, SDL_PIXELFORMAT_ABGR8888);
    SDL_SaveBMP(s, name);
    SDL_FreeSurface(s);
    free(px);
    rt_log("[dump] %s\n", name);
}
#ifdef __vita__
static void vgl_mem_report_cb(void *a) {
    (void)a;
    rt_log("[vglmem] free KB: vram %u ram %u phycont %u budget %u\n", (unsigned)(vglMemFree(VGL_MEM_VRAM) >> 10),
           (unsigned)(vglMemFree(VGL_MEM_RAM) >> 10), (unsigned)(vglMemFree(VGL_MEM_PHYCONT) >> 10), (unsigned)(vglMemFree(VGL_MEM_BUDGET) >> 10));
}
#endif
static void dump_backbuffer(Device *d, unsigned n) {
    char name[64];
    snprintf(name, sizeof name, "frame_%04u.bmp", n);
    dump_backbuffer_named(d, name);
}
#ifndef __vita__
// Optional audit reference after the final gamma/scaling pass, like Vita screenshots.
// Keep SH4_DUMP's raw game backbuffer unchanged for the existing regression frames.
static void dump_display_named(const char *name) {
    int w, h;
    plat_drawable_size(&w, &h);
    if (w <= 0 || h <= 0) rt_fatal("[audit-error] invalid display size for capture");
    size_t row = (size_t)w * 4;
    uint8_t *px = malloc(row * h);
    if (!px) rt_fatal("[audit-error] display capture allocation failed");
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, px);
    for (int y = 0; y < h / 2; y++) for (size_t x = 0; x < row; x++) {
        uint8_t tmp = px[y * row + x];
        px[y * row + x] = px[(h - 1 - y) * row + x];
        px[(h - 1 - y) * row + x] = tmp;
    }
    for (size_t i = 3; i < row * h; i += 4) px[i] = 255;
    SDL_Surface *s = SDL_CreateRGBSurfaceWithFormatFrom(px, w, h, 32, w * 4, SDL_PIXELFORMAT_ABGR8888);
    if (!s || SDL_SaveBMP(s, name)) rt_log("[shot-error] display capture %s: %s\n", name, SDL_GetError());
    else rt_log("[shot] %s\n", name);
    if (s) SDL_FreeSurface(s);
    free(px);
}
#endif
#ifdef __vita__
extern void plat_power_gate(void);   /* vita_diag_platform.c: park the render thread across a console suspend */
#endif
void gfx_present(Device *d) {
#ifdef __vita__
    plat_power_gate();
#endif
    if (__atomic_exchange_n(&bench_trace_requested, 0, __ATOMIC_ACQUIRE)) {
        ctl_trace_frame = g_pframe + 2;
        ctl_trace_n = 2; /* No per-draw glReadPixels in range mode. */
        rt_log("[diag] trace requested from frame %u for %u frames\n", ctl_trace_frame, ctl_trace_n);
    }
#ifdef SH3_SAMPLER_CACHE_LAB
    if (g_pframe && !(g_pframe%300u))
        rt_log("[sampler-cache] frame=%u sent=%u saved=%u\n",g_pframe,sampler_sent,sampler_saved);
#endif
    pass_idx=0;
    if (rtt_on>0) { if (rtt_active) { rtt_flush_bind(); rt_log("[rttrace] frame %u:%s\n",rtt_frame,rtt_line); rtt_active=0; } if (!(++rtt_frame%300u)) { glFinish(); rtt_active=1; rtt_len=0; rtt_line[0]=0; rtt_draws=0; } }
    gfx_state_invalidate();
    gfx_fx_flush();
#ifdef __vita__
    if (((g_diag_perf&1048576u) || g_glstatecache_ab) && g_pframe && !(g_pframe%120u))
        rt_log("[glstate-cache] frame=%u hits=%u misses=%u\n",g_pframe,state_hits,state_misses);
    if ((g_diag_perf&262144u) && g_pframe && !(g_pframe%120u)) {
        rt_log("[fxhalf] frame=%u cumulative_draws=%u cumulative_batches=%u\n",g_pframe,fx_draws,fx_batches);
    }
#endif
    SLOW("present", 0, 0);
    int ww, wh, level;
    static unsigned frames, last_ms;
    frames++;
    if (g_audit_enabled && g_audit_frames) {
        static unsigned prev_draws, prev_clears, prev_progs;
        rt_log("[audit-render] frame=%u draws=%u clears=%u new_programs=%u\n", frames,
               g_stat_draws - prev_draws, g_stat_clears - prev_clears, g_stat_progs - prev_progs);
        prev_draws = g_stat_draws; prev_clears = g_stat_clears; prev_progs = g_stat_progs;
    }
    if (frames <= 3) rt_log("[present] frame %u draws=%u progs=%u\n", frames, g_stat_draws, g_stat_progs);
    g_pframe = frames;
    if (frames % 30 == 0) {
        static int poll_flags = -1;
        if (poll_flags < 0) poll_flags = ((!getenv("SH4_LIVE") && !getenv("SH4_CTL")) || getenv("SH4_POLL_DISABLE") ? 1 : 0) | (getenv("SH4_POLL_TRACE") ? 2 : 0);
        if (!(poll_flags & 1)) {
            uint64_t begin = (poll_flags & 2) ? SDL_GetPerformanceCounter() : 0;
            ctl_poll();
            if (poll_flags & 2) rt_log("[poll-ctl] frame=%u us=%llu\n", frames,
                (unsigned long long)((SDL_GetPerformanceCounter() - begin) * 1000000ull / SDL_GetPerformanceFrequency()));
        }
    }
    static int dump_read; static const char *dump_env;   /* once: getenv walks the environment, this runs every frame */
    if (!dump_read) { dump_env = getenv("SH4_DUMP"); dump_read = 1; }
    const char *dump = dump_env;
    if (dump) {
        unsigned every = (unsigned)atoi(dump);
        if (every && frames % every == 0) dump_backbuffer(d, frames);
    }
    static uint64_t pv_ts;
    static unsigned g_frame_ticks[16];
    static unsigned n_long;
    static double ms_max;
    static struct { unsigned f; float ms, body, io, wait, sleep, gl, link, tex; unsigned kb; } lg[12];   // long frames: number, interval, body, disk read, wait, sleep
    {
        extern uint64_t g_t_body, g_t_io, g_t_wait, g_t_sleep, g_io_bytes;
        static uint64_t pv_body, pv_io, pv_wait, pv_sleep, pv_iob, pv_gl, pv_link, pv_tex;
        uint64_t t = SDL_GetPerformanceCounter();
        if (pv_ts) {
            double fq = (double)SDL_GetPerformanceFrequency(), ms = (double)(t - pv_ts) * 1000.0 / fq;
            if (ms > 36.0) { if (n_long < 12) { lg[n_long].f = frames; lg[n_long].ms = (float)ms; lg[n_long].body = (float)((double)(g_t_body - pv_body) * 1000.0 / fq); lg[n_long].io = (float)((double)(g_t_io - pv_io) * 1000.0 / fq); lg[n_long].wait = (float)((double)(g_t_wait - pv_wait) * 1000.0 / fq); lg[n_long].sleep = (float)((double)(g_t_sleep - pv_sleep) * 1000.0 / fq); lg[n_long].kb = (unsigned)((g_io_bytes - pv_iob) >> 10); lg[n_long].gl = (float)((double)(g_t_gl - pv_gl) * 1000.0 / fq); lg[n_long].link = (float)((double)(g_t_link - pv_link) * 1000.0 / fq); lg[n_long].tex = (float)((double)(g_t_tex - pv_tex) * 1000.0 / fq); } n_long++; }
            if (ms > ms_max) ms_max = ms;
            unsigned ticks = (unsigned)(ms * 60.0 / 1000.0 + 0.5);   // frame interval in 60 Hz refresh periods
            g_frame_ticks[ticks > 15 ? 15 : ticks]++;
        }
        pv_ts = t; pv_body = g_t_body; pv_io = g_t_io; pv_wait = g_t_wait; pv_sleep = g_t_sleep; pv_iob = g_io_bytes; pv_gl = g_t_gl; pv_link = g_t_link; pv_tex = g_t_tex;
    }
    if (ENVFLAG("SH4_STATS") && SDL_GetTicks() - last_ms > 2000) {
        extern uint64_t g_t_body;
        static uint64_t t_last, g0, b0;
        static unsigned f0;
        uint64_t now = SDL_GetPerformanceCounter(), hz = SDL_GetPerformanceFrequency();
        if (t_last) {
            double n = frames - f0, wall = (double)(now - t_last) * 1000.0 / hz / n;
            double gl = (double)(g_t_gl - g0) * 1000.0 / hz / n, body = (double)(g_t_body - b0) * 1000.0 / hz / n;
            rt_log("[perf] %.1f ms/frame; frame body %.2f ms of which game code %.2f ms and GL %.2f ms\n", wall,
                   body, body - gl, gl);
            double k = 1000.0 / hz / n;
            rt_log("[perf] frames > 36 ms: %u of %.0f, longest %.1f ms\n", n_long, n, ms_max);
            for (unsigned i = 0; i < n_long && i < 12; i++) rt_log("[long] frame %u: %.1f ms (body %.1f of which GL %.1f links %.1f tex %.1f; disk %.1f ms %u KB; wait %.1f; sleep %.1f)\n", lg[i].f, lg[i].ms, lg[i].body, lg[i].gl, lg[i].link, lg[i].tex, lg[i].io, lg[i].kb, lg[i].wait, lg[i].sleep);
            rt_log("[perf] GL/frame: prog+unif %.2f tex %.2f idx %.2f attr %.2f states %.2f draw %.2f ms; vertices copied %.0f KB; VS uniforms %.0f KB\n",
                   g_t_ph[0] * k, g_t_ph[1] * k, g_t_ph[4] * k, g_t_ph[2] * k, g_t_ph[3] * k, g_t_ph[5] * k,
                   (double)g_stat_vbytes / 1024.0 / n, (double)g_stat_ubytes / 1024.0 / n);
            rt_log("[perf] CopyRects/frame: %.2f ms; GPU blits %.1f readbacks %.1f CPU copies %.1f\n", g_t_copy * k,
                   g_stat_cp[0] / n, g_stat_cp[1] / n, g_stat_cp[2] / n);
#ifdef __vita__
            q_report(n);
#endif
            prep_report(n);
            {   // exact distribution of frame intervals (the [long] list is capped) and time inside the swap
                extern void diag_perf_report(double);
                extern uint64_t g_t_swap;
                static uint64_t swap0;
                char hist[160]; int hn = 0;
                for (int i = 0; i < 16; i++) if (g_frame_ticks[i]) hn += snprintf(hist + hn, sizeof hist - (size_t)hn, " %d:%u", i, g_frame_ticks[i]);
                rt_log("[frames] n=%.0f intervals in 1/60 s (ticks:count)%s; swap %.2f ms/frame\n", n, hist, (double)(g_t_swap - swap0) * k);
                swap0 = g_t_swap;
                memset(g_frame_ticks, 0, sizeof g_frame_ticks);
                diag_perf_report(n);
#ifdef __vita__
                /* Free vitaGL memory per type, read on the renderer thread (heap walk, once per report). */
                prep_call_async(vgl_mem_report_cb, NULL);
#endif
            }
        }
        n_long = 0; ms_max = 0;
        memset(g_t_ph, 0, sizeof g_t_ph); g_stat_vbytes = g_stat_ubytes = 0; g_t_copy = 0; memset(g_stat_cp, 0, sizeof g_stat_cp);
        t_last = now; g0 = g_t_gl; b0 = g_t_body; f0 = frames;
        last_ms = SDL_GetTicks();
        extern uint64_t g_heap_used, g_heap_peak, g_mem_tex, g_mem_vb, g_mem_ds;
        extern void heap_report(void);
        static int rep;
        if (++rep % 10 == 0) heap_report();
        rt_log("[mem] textures %llu MB, vb/ib %llu MB, audio %llu MB\n", (unsigned long long)(g_mem_tex >> 20),
               (unsigned long long)(g_mem_vb >> 20), (unsigned long long)(g_mem_ds >> 20));
        rt_log("[stats] presents=%u draws=%u clears=%u programs=%u heap=%llu MB (peak %llu MB)\n", frames,
               g_stat_draws, g_stat_clears, g_stat_progs, (unsigned long long)(g_heap_used >> 20),
               (unsigned long long)(g_heap_peak >> 20));
    }
    plat_drawable_size(&ww, &wh);
    static int tp = -1;
    if (tp < 0) tp = getenv("SH4_TRACE_COPY") != NULL;
    if (tp) rt_log("[present] frame %u bind fbo 0\n", frames);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    if (tp) rt_log("[present] bind ok\n");
    // Scaling that keeps the aspect ratio.
    Surface *display = d->ui_display ? d->ui_display : d->backbuffer;
    float sa = (float)display->w / (float)display->h, wa = (float)ww / (float)wh;
    int vw = ww, vh = wh;
    if (wa > sa) vw = (int)(wh * sa); else vh = (int)(ww / sa);
#ifndef __vita__
    /* Visual experiment only: stretch the 4:3 intermediate to a wide Mac
       window. Menus and 2D effects are not corrected; never enable on Vita. */
    if (ENVFLAG("SH3_DIAG_WIDE_PREVIEW")) { vw=ww; vh=wh; }
#endif
    glViewport(0, 0, ww, wh);
    if (tp) rt_log("[present] vp1 ok\n");
    glDisable(GL_SCISSOR_TEST);
    glClearColor(0, 0, 0, 1);
    glColorMask(1, 1, 1, 1);
    /* Diagnostic: the full-screen blit overwrites every pixel. Letterbox
       borders still require the clear. This may be slower on tiled GPUs. */
    extern unsigned g_diag_perf;
    if (!(g_diag_perf&16384u) || vw!=ww || vh!=wh) glClear(GL_COLOR_BUFFER_BIT);
    if (tp) rt_log("[present] clear ok\n");
    glViewport((ww - vw) / 2, (wh - vh) / 2, vw, vh);
    if (tp) rt_log("[present] vp2 ok\n");
    glDisable(GL_DEPTH_TEST); glDisable(GL_BLEND); glDisable(GL_CULL_FACE); glDisable(GL_STENCIL_TEST);
    float go[4] = {0}, gs[4] = {0}, ge[4] = {0};
    int gid = 1;
    for (int k = 0; k < 3; k++) {
        const uint16_t *r = d->gamma[k];
        go[k] = r[0] / 65535.0f; gs[k] = (r[255] - r[0]) / 65535.0f;
        float m = gs[k] > 0.001f ? (r[128] / 65535.0f - go[k]) / gs[k] : 0.5f;
        ge[k] = m > 0.001f && m < 0.999f ? logf(m) / logf(128.0f / 255.0f) : 1.0f;
        if (fabsf(go[k]) > 0.004f || fabsf(gs[k] - 1.0f) > 0.004f || fabsf(ge[k] - 1.0f) > 0.01f) gid = 0;
    }
    use_prog(gid ? g_blit_prog : g_gamma_prog);
    if (!gid) { glUniform4fv(glGetUniformLocation(g_gamma_prog, "go"), 1, go); glUniform4fv(glGetUniformLocation(g_gamma_prog, "gs"), 1, gs); glUniform4fv(glGetUniformLocation(g_gamma_prog, "ge"), 1, ge); }
    bind_vbo(0);
    bind_ibo(0);   // each frame starts from a known state (replayed twice by the interpolation)
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, color_tex(display, &level));
    static const float quad[] = {-1, -1, 0, 0, 1, -1, 1, 0, -1, 1, 0, 1, 1, 1, 1, 1};
    for (int r = 0; r < 16; r++) if (enabled_attrs & (1u << r)) glDisableVertexAttribArray((GLuint)r);
    enabled_attrs = 1;
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 4, GL_FLOAT, GL_FALSE, 16, quad);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    uint64_t ts = SDL_GetPerformanceCounter();
    char audit_path[600];
    if (rt_audit_capture(frames, audit_path, sizeof audit_path)) {
        if (g_audit_checks)
            rt_log("[audit-display] frame=%u gamma_identity=%d exponent=%.6f/%.6f/%.6f red=%u/%u/%u\n",
                   frames, gid, ge[0], ge[1], ge[2], d->gamma[0][0], d->gamma[0][128], d->gamma[0][255]);
#ifdef __vita__
        extern void plat_shot_raw(const char *path);
        q_sync("audit capture");
        plat_shot_raw(audit_path);
#else
        if (ENVFLAG("SH4_AUDIT_DISPLAY")) dump_display_named(audit_path);
        else dump_backbuffer_named(d, audit_path);
#endif
    }
    plat_gl_swap();
    g_t_swap += SDL_GetPerformanceCounter() - ts;
    gfx_bind_target(d);
}
