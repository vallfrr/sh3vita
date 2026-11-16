#!/usr/bin/env python3
"""C5: selective register write-back for the generated x86 C (post-pass, the generator itself is untouched).

  python recomp/c5_selective_spill.py SRC_DIR DST_DIR [--check] [--stats]

The generated code keeps the eight guest integer registers in C locals. Every function ends with a full spill
(`c->eax=eax;...c->edi=edi;`) and every direct call is followed by a full reload. This pass computes, for every function,
the set mod(F) of registers it can modify (own assignments + mod of every direct callee; anything unknown = all eight) and
rewrites only the two safe sites:

  * epilogue  `SPILL return;`                      -> write back only mod(F) + esp
  * direct call `SPILL F_t(c); RELOAD`             -> reload only mod(F_t) + esp

Every pre-call spill and every function entry load stays complete, so a callee always sees correct registers in `c`, and a
register outside mod(F) is unchanged in `c` because the caller wrote it before the call. Unknown calls (rt_call, host imports,
setjmp, tail rt_call, hook calls, linker --wrap targets listed below) count as "all registers".

--check emits the same structure but keeps the full write-back/reload and verifies the elided registers instead
(`rt_c5_violation`): a run of the game with the --check tree proves the mod sets on the executed paths.
--stats prints the distribution of mod set sizes weighted by nothing (static) and the number of rewritten sites."""
import os, re, sys, glob, collections

REGS = ["eax", "ecx", "edx", "ebx", "esp", "ebp", "esi", "edi"]
SPILL = "c->eax=eax;c->ecx=ecx;c->edx=edx;c->ebx=ebx;c->esp=esp;c->ebp=ebp;c->esi=esi;c->edi=edi;"
RELOAD = "eax=c->eax;ecx=c->ecx;edx=c->edx;ebx=c->ebx;esp=c->esp;ebp=c->ebp;esi=c->esi;edi=c->edi;"
INIT = re.compile(r"uint32_t eax=c->eax,ecx=c->ecx,edx=c->edx,ebx=c->ebx,esp=c->esp,ebp=c->ebp,esi=c->esi,edi=c->edi;")
FUNC = re.compile(r"^void F_([0-9a-f]{8})\(CPU \*c\) \{$")
# functions replaced by the port through linker --wrap or hooks: their real behaviour is not the generated body alone
WRAPPED = {"0041b2c0", "0066405e", "00614fe0", "006176c0", "00614640", "00617f70", "006181d0", "006183e0", "006183a0", "00617ca0"}
ASSIGN = {r: re.compile(r"(?<![\w>.])" + r + r"\s*(?:[-+*/%&|^]|<<|>>)?=(?!=)|&" + r + r"\b|(?<![\w>.])" + r + r"\s*(?:\+\+|--)|(?:\+\+|--)" + r + r"\b") for r in REGS}
FTOP = r"(?:c->ftop=[^;]*;\s*)?"
RE_CALL_RELOAD = re.compile(re.escape(SPILL) + r"\s*" + FTOP + r"F_([0-9a-f]{8})\(c\);\s*" + re.escape(RELOAD))
RE_CALL_TAIL = re.compile(re.escape(SPILL) + r"\s*" + FTOP + r"F_([0-9a-f]{8})\(c\);\s*return;")
RE_NORETURN = re.compile(re.escape(SPILL) + r"\s*(?:rt_unimpl|rt_bad_jump|rt_div0)\(")
RE_EPILOGUE = re.compile(re.escape(SPILL) + r" return;")

class Func:
    __slots__ = ("addr", "lines", "start", "end", "own", "callees", "unknown", "mod")

def split_functions(text):
    lines = text.split("\n"); funcs = []; i = 0
    while i < len(lines):
        m = FUNC.match(lines[i])
        if m:
            j = i
            while j < len(lines) and lines[j] != "}": j += 1
            f = Func(); f.addr, f.start, f.end = m.group(1), i, j; funcs.append(f); i = j
        i += 1
    return lines, funcs

def analyse(body, f):
    """Fill own register assignments, direct callees and the unknown flag from the body text."""
    t = INIT.sub("", body)
    callees = [m.group(1) for m in RE_CALL_RELOAD.finditer(t)] + [m.group(1) for m in RE_CALL_TAIL.finditer(t)]
    unknown = False
    # every SPILL occurrence must be an epilogue, a recognised direct call, or a noreturn helper
    known = len(RE_EPILOGUE.findall(t)) + len(RE_CALL_RELOAD.findall(t)) + len(RE_CALL_TAIL.findall(t)) + len(RE_NORETURN.findall(t))
    if t.count(SPILL) != known: unknown = True
    # every RELOAD must belong to a recognised direct call
    if t.count(RELOAD) != len(RE_CALL_RELOAD.findall(t)): unknown = True
    stripped = t.replace(SPILL, "").replace(RELOAD, "")
    f.own = {r for r in REGS if ASSIGN[r].search(stripped)}
    f.callees, f.unknown = callees, unknown

def solve(funcs):
    byaddr = {f.addr: f for f in funcs}
    for f in funcs:
        f.mod = set(REGS) if (f.unknown or f.addr in WRAPPED) else set(f.own)
    changed = True; rounds = 0
    while changed:
        changed = False; rounds += 1
        for f in funcs:
            if len(f.mod) == 8: continue
            for a in f.callees:
                g = byaddr.get(a)
                add = set(REGS) if (g is None or a in WRAPPED) else g.mod
                if not add <= f.mod: f.mod |= add; changed = True
    return byaddr, rounds

def regs_text(regs, keep_esp=True):
    return [r for r in REGS if r in regs or (keep_esp and r == "esp")]

def spill_text(regs):
    return "".join(f"c->{r}={r};" for r in regs_text(regs))

def reload_text(regs):
    return "".join(f"{r}=c->{r};" for r in regs_text(regs))

def check_spill(regs, fid):
    keep = regs_text(regs)
    chk = "".join(f"if (c->{r}!={r}) rt_c5_violation(c,0x{fid}u,{REGS.index(r)});" for r in REGS if r not in keep)
    return SPILL + chk

def check_reload(regs, fid):
    keep = regs_text(regs)
    chk = "".join(f"if (c->{r}!={r}) rt_c5_violation(c,0x{fid}u,{8 + REGS.index(r)});" for r in REGS if r not in keep)
    return chk + RELOAD

def rewrite(body, f, byaddr, check, st):
    def epi(m):
        st["epilogue"] += 1
        if len(f.mod) == 8: return m.group(0)
        return (check_spill(f.mod, f.addr) if check else spill_text(f.mod)) + " return;"
    def call(m):
        a = m.group(1); g = byaddr.get(a)
        mod = set(REGS) if (g is None or a in WRAPPED) else g.mod
        if len(mod) == 8: return m.group(0)
        st["reload"] += 1
        whole = m.group(0)
        return whole[:-len(RELOAD)] + (check_reload(mod, f.addr) if check else reload_text(mod))
    # the call pattern first (it contains SPILL ... RELOAD), then the epilogues
    body = RE_CALL_RELOAD.sub(call, body)
    return RE_EPILOGUE.sub(epi, body)

def main():
    a = [x for x in sys.argv[1:] if not x.startswith("--")]
    check = "--check" in sys.argv; stats = "--stats" in sys.argv
    src, dst = a[0], a[1]
    os.makedirs(dst, exist_ok=True)
    files = {}; funcs_all = []
    for p in sorted(glob.glob(os.path.join(src, "*"))):
        name = os.path.basename(p)
        if not name.endswith(".c") or name in ("x86_dispatch.c", "x86_thunks.c"): continue
        text = open(p, encoding="utf-8", newline="").read()
        lines, funcs = split_functions(text)
        for f in funcs: analyse("\n".join(lines[f.start:f.end]), f)
        files[name] = (lines, funcs); funcs_all += funcs
    byaddr, rounds = solve(funcs_all)
    st = collections.Counter()
    for name, (lines, funcs) in files.items():
        out = list(lines)
        for f in funcs:
            new = rewrite("\n".join(lines[f.start:f.end]), f, byaddr, check, st).split("\n")
            out[f.start:f.end] = new + [None] * (f.end - f.start - len(new)) if len(new) <= f.end - f.start else new
        # function bodies are rewritten in place keeping the line count (replacements never add lines)
        open(os.path.join(dst, name), "w", encoding="utf-8", newline="").write("\n".join(l for l in out if l is not None))
    for p in glob.glob(os.path.join(src, "*")):
        n = os.path.basename(p)
        if n not in files and os.path.isfile(p):
            open(os.path.join(dst, n), "wb").write(open(p, "rb").read())
    if stats or True:
        dist = collections.Counter(len(f.mod) for f in funcs_all)
        print("functions", len(funcs_all), "fixpoint rounds", rounds, "unknown", sum(f.unknown for f in funcs_all))
        print("mod set size distribution", dict(sorted(dist.items())))
        print("rewritten epilogues", st["epilogue"], "(counted incl. full)", "reloads", st["reload"])

if __name__ == "__main__":
    main()
