"""Isolated copy of a generated unit where some functions dispatch to native kernels.
Each SPEC is FUNC:FLAG:NATIVE (e.g. F_00436430:g_texfill_native:texfill_native): the translated FUNC is renamed
FUNC_translated and a new FUNC calls NATIVE(c) when FLAG is set, falling back to the translation when NATIVE returns 0.
Needed when callers live in the same unit (a --wrap only intercepts calls between units). The renames are checked by
reversing them: every other byte of the unit is identical.
Usage: native_unit.py SOURCE OUTPUT SPEC [SPEC...]"""
import sys
from pathlib import Path


def transform(src, specs):
    out, tail = src, []
    for spec in specs:
        fn, flag, native = spec.split(':')
        head = f'void {fn}(CPU *c) {{'
        if out.count(head) != 1: raise SystemExit(f'{fn} definition not found exactly once')
        out = out.replace(head, f'void {fn}_translated(CPU *c) {{', 1)
        tail.append(f'extern int {flag}; int {native}(CPU *c);\n'
                    f'void {fn}(CPU *c) {{ if ({flag} && {native}(c)) return; {fn}_translated(c); }}\n')
    back = out
    for spec in specs:
        fn = spec.split(':')[0]
        back = back.replace(f'void {fn}_translated(CPU *c) {{', f'void {fn}(CPU *c) {{', 1)
    assert back == src
    return out + '\n/* Native dispatchers (tools/native_unit.py) */\n' + ''.join(tail)


if __name__ == '__main__':
    if len(sys.argv) < 4: raise SystemExit(__doc__)
    src, dst = Path(sys.argv[1]), Path(sys.argv[2])
    if dst.resolve().parent == src.resolve().parent: raise SystemExit('Isolated copy required')
    dst.parent.mkdir(parents=True, exist_ok=True)
    dst.write_text(transform(src.read_text(encoding='utf-8'), sys.argv[3:]), encoding='utf-8')
