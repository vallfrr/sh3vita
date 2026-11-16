"""Isolated copy of unit x86_f040.c with three native switches:
- F_00478ac0 dispatches to the native particle grid kernel (particle_native.c) when g_particle_native is set, else to the
  unchanged translation (renamed F_00478ac0_translated);
- F_00479340 (particle / cylinder collision) dispatches to particle_cyl_native when g_pcyl_native is set;
- in F_00479e30, the per-particle loop (0x47a0b3..0x47a836) runs natively (particle_draw_native.c) when
  g_particle_draw_native is set and its preconditions hold, else the translated loop runs.
Both edits are checked by reversing them: every other byte of the unit is identical."""
import argparse
from pathlib import Path

HEAD = 'void F_00478ac0(CPU *c) {'
CYL = 'void F_00479340(CPU *c) {'
LOOP = '  // 0x47a0b3: mov ebx, 0x712e408\n'
SYNC_OUT = 'c->eax=eax;c->ecx=ecx;c->edx=edx;c->ebx=ebx;c->esp=esp;c->ebp=ebp;c->esi=esi;c->edi=edi;'
SYNC_IN = 'eax=c->eax;ecx=c->ecx;edx=c->edx;ebx=c->ebx;esp=c->esp;ebp=c->ebp;esi=c->esi;edi=c->edi;'
DRAW = ('  if (g_particle_draw_native) { ' + SYNC_OUT + ' if (particle_draw_native(c)) { ' + SYNC_IN +
        ' goto L_0047a836; } }   /* native per-particle loop (particle_draw_native.c) */\n')


def transform(src):
    if src.count(HEAD) != 1: raise SystemExit('F_00478ac0 definition not found exactly once')
    if src.count(LOOP) != 1 or src.count('L_0047a836:;') != 1: raise SystemExit('F_00479e30 loop entry not found exactly once')
    if src.count(CYL) != 1: raise SystemExit('F_00479340 definition not found exactly once')
    out = src.replace(HEAD, 'void F_00478ac0_translated(CPU *c) {', 1).replace(LOOP, DRAW + LOOP, 1).replace(CYL, 'void F_00479340_translated(CPU *c) {', 1)
    assert out.replace('void F_00478ac0_translated(CPU *c) {', HEAD, 1).replace(DRAW, '', 1).replace('void F_00479340_translated(CPU *c) {', CYL, 1) == src
    return out.replace(
        'void F_00479e30(CPU *c) {', 'extern int g_particle_draw_native; int particle_draw_native(CPU *c);\nvoid F_00479e30(CPU *c) {', 1) + '''
/* Native particle grid kernel switch (particle_native.c). */
extern int g_particle_native; int particle_grid_native(CPU *c);
void F_00478ac0(CPU *c) {
    if (g_particle_native && particle_grid_native(c)) return;
    F_00478ac0_translated(c);
}
/* Native particle / cylinder collision switch (particle_native.c). */
extern int g_pcyl_native; int particle_cyl_native(CPU *c);
void F_00479340(CPU *c) {
    if (g_pcyl_native && particle_cyl_native(c)) return;
    F_00479340_translated(c);
}
'''


if __name__ == '__main__':
    ap = argparse.ArgumentParser(description=__doc__); ap.add_argument('source', type=Path); ap.add_argument('output', type=Path); a = ap.parse_args()
    if a.output.resolve().parent == a.source.resolve().parent: raise SystemExit('Isolated copy required')
    a.output.parent.mkdir(parents=True, exist_ok=True); a.output.write_text(transform(a.source.read_text(encoding='utf-8')), encoding='utf-8')
