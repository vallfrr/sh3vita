// Runtime for the recompiled x86 code: CPU state, guest memory, x87 FPU.
// Included by the generated code (port/gen/) and by the host implementations (Win32, D3D8...).
#pragma once
#include <stdint.h>
#include <string.h>
#include <math.h>
#include <setjmp.h>

typedef struct CPU {
    uint32_t eax, ecx, edx, ebx, esp, ebp, esi, edi;
    uint32_t df;          // direction flag
    uint32_t eflags_ext;  // EFLAGS bits outside arithmetic (ID...) for PUSHFD/POPFD
    uint32_t fs_base;     // guest address of this thread's TIB
    double st[8];
    uint32_t ftop;
    uint32_t fsw, fcw;
    uint8_t xmm[8][16]; /* x86 SSE registers; kept per guest thread */
} CPU;

typedef void (*gfn)(CPU *c);

// Guest memory: guest address a -> g_mem + a.
extern uint8_t *g_mem;

typedef uint16_t u16u __attribute__((aligned(1), may_alias));
typedef uint32_t u32u __attribute__((aligned(1), may_alias));
typedef uint64_t u64u __attribute__((aligned(1), may_alias));

#define R8(a)  (*(uint8_t *)(mb + (uint32_t)(a)))
#define R16(a) (*(u16u *)(mb + (uint32_t)(a)))
#define R32(a) (*(u32u *)(mb + (uint32_t)(a)))
#define R64(a) (*(u64u *)(mb + (uint32_t)(a)))
#define W8(a, v)  (R8(a) = (uint8_t)(v))
#define W16(a, v) (R16(a) = (uint16_t)(v))
#define W32(a, v) (R32(a) = (uint32_t)(v))
#define W64(a, v) (R64(a) = (uint64_t)(v))
#define HOSTPTR(a) ((void *)(g_mem + (uint32_t)(a)))

static inline float  rt_f32(uint32_t v) { float f; memcpy(&f, &v, 4); return f; }
static inline uint32_t rt_u32f(float f) { uint32_t v; memcpy(&v, &f, 4); return v; }
static inline double rt_f64(uint64_t v) { double f; memcpy(&f, &v, 8); return f; }
static inline uint64_t rt_u64f(double f) { uint64_t v; memcpy(&v, &f, 8); return v; }

/* Helpers of the x87 compare idiom are forced inline: at -Os GCC outlined them (7131 bl rt_fcom, 6978 bl rt_fnstsw*,
   4603 bl __paritysi2 in the generated objects, docs/AUDIT-FABLE-STRUCTUREL.md C1). */
/* SH3_RT_OUTLINE (cmake -DSH3_X87_INLINE=OFF) restores the previous code shape for A/B baselines only. */
#ifdef SH3_RT_OUTLINE
#define RT_INLINE static inline
#else
#define RT_INLINE static inline __attribute__((always_inline))
#endif
/* x86 PF of the low byte (1 = even parity) without libgcc __paritysi2: 0x9669 bit n = even parity of nibble n. */
#ifdef SH3_RT_OUTLINE
RT_INLINE uint32_t rt_pf(uint32_t r) { return (uint32_t)!__builtin_parity(r & 0xffu); }
#else
RT_INLINE uint32_t rt_pf(uint32_t r) { return (0x9669u >> ((r ^ (r >> 4)) & 15u)) & 1u; }
#endif
#define PF(r) rt_pf(r)

// --- x87 ---
#define ST(i) (c->st[(c->ftop + (i)) & 7])
#define FPUSH(v) do { double _v = (v); c->ftop = (c->ftop - 1) & 7; c->st[c->ftop] = _v; } while (0)
#define FPOP() (c->ftop = (c->ftop + 1) & 7)
#define FSW_C0 0x0100u
#define FSW_C2 0x0400u
#define FSW_C3 0x4000u

/* Approximation: slots are doubles and x87 empty/tag state is not modelled. */
RT_INLINE void rt_fxam(CPU *c, double value) {
    uint32_t flags=signbit(value)?0x0200u:0;
    switch (fpclassify(value)) {
    case FP_NAN: flags|=FSW_C0; break;
    case FP_INFINITE: flags|=FSW_C0|FSW_C2; break;
    case FP_ZERO: flags|=FSW_C3; break;
    /* Binary64 subnormals become normal when held in an x87 extended register. */
    case FP_SUBNORMAL: flags|=FSW_C2; break;
    case FP_NORMAL: flags|=FSW_C2; break;
    }
    c->fsw=(c->fsw&~(FSW_C0|0x0200u|FSW_C2|FSW_C3))|flags;
}

RT_INLINE void rt_fcom(CPU *c, double a, double b) {
    uint32_t s = c->fsw & ~(FSW_C0 | FSW_C2 | FSW_C3);
    /* Ordered cases first: one compare when a<b instead of two NaN tests before it; unordered falls through. */
    if (a < b) s |= FSW_C0;
    else if (a > b) ;
    else if (a == b) s |= FSW_C3;
    else s |= FSW_C0 | FSW_C2 | FSW_C3;
    c->fsw = s;
}
RT_INLINE uint32_t rt_fnstsw(CPU *c) { return (c->fsw & ~0x3800u) | ((c->ftop & 7) << 11); }
// recompiler static mode: TOP known by the caller (ftop is only up to date at calls/returns)
RT_INLINE uint32_t rt_fnstsw_t(CPU *c, uint32_t top) { return (c->fsw & ~0x3800u) | ((top & 7) << 11); }
// the CRT's __ftol inlined by the recompiler: truncation toward zero, 0x8000000000000000 out of range or NaN (as fistp)
RT_INLINE int64_t rt_ftol(double v) { return (v >= -9223372036854775808.0 && v < 9223372036854775808.0) ? (int64_t)v : INT64_MIN; }

double rt_fround(CPU *c, double v);          // according to fcw's rounding mode
int32_t rt_fist32(CPU *c, double v);
int16_t rt_fist16(CPU *c, double v);
int64_t rt_fist64(CPU *c, double v);
double rt_f80_load(const uint8_t *p);
void rt_f80_store(uint8_t *p, double v);
void rt_fsave(CPU *c, uint32_t addr);
void rt_frstor(CPU *c, uint32_t addr);

// --- divers ---
void rt_cpuid(uint32_t *a, uint32_t *b, uint32_t *c, uint32_t *d);
uint32_t rt_flags_pack(CPU *c, uint32_t cf, uint32_t pf, uint32_t zf, uint32_t sf, uint32_t of);

// --- control flow ---
// guest setjmp (__setjmp3): the generated code arms a host setjmp on the return address; the
// final `jmp [eip]` of the guest _longjmp arrives there via rt_call, which does the host longjmp.
jmp_buf *rt_setjmp_slot(CPU *c, uint32_t ret_addr);
void rt_call(CPU *c, uint32_t target);       // indirect call/jump by guest address
gfn rt_lookup(uint32_t target);
uint32_t rt_host_addr(gfn f, const char *name); // synthetic guest address for a host function

// --- erreurs fatales ---
_Noreturn void rt_unimpl(CPU *c, uint32_t pc, const char *what);
/* c5_selective_spill.py --check trees: a register the pass assumed unchanged differs between c and the local copy. */
void rt_c5_violation(CPU *c, uint32_t fn, int reg);
_Noreturn void rt_bad_jump(CPU *c, uint32_t pc, uint32_t target);
_Noreturn void rt_div0(CPU *c, uint32_t pc);
