"""Isolated copy of unit x86_f047.c where the shadow dispatcher F_00490760 sends types 5/6 (strip volumes F_0048faa0)
to the native builder shadow_strip_native (shadow_native.c) when g_shadow_strip is set, else to the unchanged translation
(renamed F_00490760_translated; its caller F_00490cf0 is in the same unit, hence the copy instead of --wrap).
The rename is checked by reversing it: every other byte of the unit is identical."""
import argparse
from pathlib import Path

HEAD = 'void F_00490760(CPU *c) {'


def transform(src):
    if src.count(HEAD) != 1: raise SystemExit('F_00490760 definition not found exactly once')
    out = src.replace(HEAD, 'void F_00490760_translated(CPU *c) {', 1)
    assert out.replace('void F_00490760_translated(CPU *c) {', HEAD, 1) == src
    return out + '''
/* Native strip shadow volumes switch (shadow_native.c); g_ss_native counts native calls per A/B window. */
extern int g_shadow_strip; extern unsigned g_ss_native; int shadow_strip_native(CPU *c);
void F_00490760(CPU *c) {
    if (g_shadow_strip && shadow_strip_native(c)) { g_ss_native++; return; }
    F_00490760_translated(c);
}
'''


if __name__ == '__main__':
    ap = argparse.ArgumentParser(description=__doc__); ap.add_argument('source', type=Path); ap.add_argument('output', type=Path); a = ap.parse_args()
    if a.output.resolve().parent == a.source.resolve().parent: raise SystemExit('Isolated copy required')
    a.output.parent.mkdir(parents=True, exist_ok=True); a.output.write_text(transform(a.source.read_text(encoding='utf-8')), encoding='utf-8')
