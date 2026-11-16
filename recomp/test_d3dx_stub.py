#!/usr/bin/env python3
"""Differential test of the D3DX stub fast path (x86rec.d3dx_stub_fast, AUDIT-FABLE-STRUCTUREL C2).

usage: test_d3dx_stub.py BASE_GEN_DIR NEW_GEN_DIR
Every stub of NEW that carries the fast path is run against its BASE translation (which really calls the
translated F_0063b6aa) on two identical 8 MiB arenas: dispatch target, whole CPU and whole memory must match.
Only the initialised-table states are exercised; state 0xffff takes the unchanged original body in both.
Needs gcc; no game data other than the generated C (never versioned).
"""
import pathlib, re, subprocess, sys, tempfile

ROOT = pathlib.Path(__file__).resolve().parents[1]


def function(text, name):
    a = text.index(f"void {name}(CPU *c) {{")
    return text[a:text.index("\n}", a) + 2]


def find(gen, name):
    for p in sorted(gen.glob("x86_f*.c")):
        t = p.read_text(encoding="utf-8")
        if f"void {name}(CPU *c) {{" in t:
            return function(t, name)
    raise KeyError(name)


def main():
    base, new = pathlib.Path(sys.argv[1]), pathlib.Path(sys.argv[2])
    stubs = []
    for p in sorted(new.glob("x86_f*.c")):
        t = p.read_text(encoding="utf-8")
        for m in re.finditer(r"void (F_[0-9a-f]{8})\(CPU \*c\) \{\n  \{ uint8_t \*const mb = g_mem; uint32_t st_ = ", t):
            stubs.append((m.group(1), function(t, m.group(1))))
    assert len(stubs) == 14, len(stubs)   # the 14 pure three-instruction stubs of the audited executable
    parts = ['#include "x86rt.h"', "#include <stdio.h>", "#include <stdlib.h>",
             "static uint32_t rec_t; static void rec(uint32_t t) { rec_t = t; }",
             "void rt_call(CPU *c, uint32_t t) { (void)c; rec(t); }", "uint8_t *g_mem;"]
    targets, table = set(), []
    for name, body in stubs:
        targets |= set(re.findall(r"case 0x[0-9a-f]+u: (F_[0-9a-f]{8})\(c\); return;", body))
    for t in sorted(targets):
        parts.append(f"static void T_{t}(CPU *c) {{ (void)c; rec(0x{t[2:]}u); }}")
    init = find(base, "F_0063b6aa")
    callees = set(re.findall(r"\b(F_[0-9a-f]{8})\(c\)", init)) - {"F_0063b6aa"}
    for t in sorted(callees):
        parts.append(f"static void {t}(CPU *c) {{ (void)c; puts(\"init callee reached\"); exit(2); }}")
    parts.append(init)
    for name, body in stubs:
        old = find(base, name).replace(f"void {name}(CPU *c)", f"static void old_{name}(CPU *c)", 1)
        body = body.replace(f"void {name}(CPU *c)", f"static void new_{name}(CPU *c)", 1)
        body = re.sub(r"(case 0x[0-9a-f]+u: )(F_[0-9a-f]{8})\(c\)", r"\1T_\2(c)", body)
        slot = re.search(r", t_ = R32\((0x[0-9a-f]+)u\)", body).group(1)
        cases = re.findall(r"case (0x[0-9a-f]+)u:", body)
        parts += [old, body]
        table.append(f"{{\"{name}\", old_{name}, new_{name}, {slot}u, {len(cases)}, {{{','.join(c + 'u' for c in cases) or '0'}}}}}")
    parts.append("static const struct { const char *n; gfn o, f; uint32_t slot; unsigned nc; uint32_t c[8]; } S[] = {\n  "
                 + ",\n  ".join(table) + "};")
    parts.append(r"""
#define N 0x800000u
static uint64_t x = 0x9e3779b97f4a7c15ull;
static uint32_t rnd(void) { x ^= x << 13; x ^= x >> 7; x ^= x << 17; return (uint32_t)(x >> 16); }
int main(void) {
    uint8_t *A = malloc(N), *B = malloc(N); unsigned cases = 0, direct = 0;
    for (unsigned i = 0; i < N; i++) A[i] = (uint8_t)rnd();
    memcpy(B, A, N);
    for (unsigned s = 0; s < sizeof S / sizeof *S; s++) for (unsigned k = 0; k < 96; k++) {
        CPU a; memset(&a, 0, sizeof a);
        a.eax = rnd(); a.ecx = rnd(); a.edx = rnd(); a.ebx = rnd(); a.ebp = rnd(); a.esi = rnd(); a.edi = rnd();
        a.esp = 0x100000u + rnd() % 0x500000u; if (k & 1) a.esp &= ~3u;
        a.ftop = rnd() & 7; a.fsw = rnd() & 0xffff; a.fcw = 0x27f; a.df = k % 7 == 0;
        for (unsigned i = 0; i < 8; i++) a.st[i] = (double)(int32_t)rnd();
        uint32_t st = k % 5 == 4 ? rnd() : k % 5; if (st == 0xffffu) st = 0xfffe;
        uint32_t t = k % 3 == 2 || !S[s].nc ? 0x401000u + (rnd() & 0xffff0u) : S[s].c[rnd() % S[s].nc];
        memcpy(A + 0x72601c, &st, 4); memcpy(A + S[s].slot, &t, 4);
        memcpy(B + 0x72601c, &st, 4); memcpy(B + S[s].slot, &t, 4);
        CPU b = a; uint32_t ta, tb;
        g_mem = A; rec_t = 0; S[s].o(&a); ta = rec_t;
        g_mem = B; rec_t = 0; S[s].f(&b); tb = rec_t;
        if (ta != t || tb != t || memcmp(&a, &b, sizeof a) || memcmp(A, B, N)) {
            printf("FAIL %s case %u: target %08x/%08x want %08x eax %08x/%08x esp %08x/%08x\n", S[s].n, k, ta, tb, t, a.eax, b.eax, a.esp, b.esp);
            return 1;
        }
        cases++; direct += k % 3 != 2 && S[s].nc;
    }
    printf("D3DX stub fast path: %u stubs, %u differential cases (%u direct dispatches) passed\n", (unsigned)(sizeof S / sizeof *S), cases, direct);
    free(A); free(B); return 0;
}
""")
    with tempfile.TemporaryDirectory(prefix="sh3-d3dx-stub-") as td:
        src, exe = pathlib.Path(td) / "t.c", pathlib.Path(td) / "t"
        src.write_text("\n".join(parts), encoding="utf-8")
        for opt in ("-Os", "-O2"):
            subprocess.run(["gcc", opt, "-std=gnu11", "-w", "-fsanitize=address,undefined", "-fno-sanitize-recover=all",
                            "-I", str(ROOT / "port/include"), str(src), "-o", str(exe), "-lm"], check=True)
            subprocess.run([str(exe)], check=True)


if __name__ == "__main__":
    main()
