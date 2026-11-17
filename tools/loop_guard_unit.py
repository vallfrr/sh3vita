#!/usr/bin/env python3
"""Bound two collision loops in signature-checked copies of units 059 and 067 (the game froze, 2026-11-17).

* F_004ca820 (unit 067): "extend the ray until F_004b18f0 reports a wall": each pass doubles the distance of the probe point
  (p = 2*p - origin) and repeats while there is no hit. With no collision data for the room (seen in the subway after a warp)
  there is never a hit, the distance overflows to infinity and the game thread stops presenting. After LIMIT_RAY passes the
  search is abandoned as if the last probe had hit, and the registers are logged once (diag_loop_guard_trip).
* F_004b18f0 (unit 059): the list walk it calls (step 8 until a zero word, dwords until -1); abandoned after LIMIT_WALK steps
  in one call on an unterminated list.
Usage: loop_guard_unit.py x86_f059.c|x86_f067.c OUTPUT (the unit is chosen from the file name)."""
import argparse, hashlib, re
from pathlib import Path
LIMIT_WALK = 200000
LIMIT_RAY = 512
TRIP = 'diag_loop_guard_trip(0x%06xu,ecx,eax,esi,edi); '


def generate_059(source):
    sha = '0743859331f1d06c8edde9c1e6f77d7920a198ba85b21607102344ea5c831688'
    m = re.search(r'void F_004b18f0\(CPU \*c\) \{.*?\n\}', source, re.S)
    if not m or hashlib.sha256(m[0].encode()).hexdigest() != sha:
        raise ValueError('Collision list walk changed: F_004b18f0')
    body = m[0]
    guard = '  if (++sh3_guard>%du) { %sgoto L_sh3_guard_bail; }\n' % (LIMIT_WALK, TRIP % 0x4b18f0)
    for label in ('L_004b1960:;\n', 'L_004b1980:;\n'):
        assert body.count(label) == 1
        body = body.replace(label, label + guard, 1)
    pop = '  // 0x4b19c8: pop edi\n'
    assert body.count(pop) == 1
    body = body.replace(pop, 'L_sh3_guard_bail:;\n' + pop, 1)
    decl = '  uint32_t eax=c->eax,'
    assert body.count(decl) == 1
    return source[:m.start()] + body.replace(decl, '  unsigned sh3_guard=0;\n' + decl, 1) + source[m.end():]


def generate_067(source):
    sha = '8fca0b20a94309312f95130f0c1ecf01e58efc6287f477b883c28c5321c0a922'
    m = re.search(r'void F_004ca820\(CPU \*c\) \{.*?\n\}', source, re.S)
    if not m or hashlib.sha256(m[0].encode()).hexdigest() != sha:
        raise ValueError('Ray extension function changed: F_004ca820')
    body = m[0]
    g1 = '  if (++sh3_ray>%du) { %sgoto L_sh3_ray_bail1; }\n' % (LIMIT_RAY, TRIP % 0x4ca820)
    g2 = '  if (++sh3_ray>%du) { %sgoto L_004caa21; }\n' % (LIMIT_RAY, TRIP % 0x4ca820)
    for label, g in (('L_004ca990:;\n', g1), ('L_004ca9d2:;\n', g2)):
        assert body.count(label) == 1
        body = body.replace(label, label + g, 1)
    pop = '  // 0x4ca9cf: pop edi\n'
    assert body.count(pop) == 1
    body = body.replace(pop, 'L_sh3_ray_bail1:;\n' + pop, 1)
    decl = '  uint32_t eax=c->eax,'
    assert body.count(decl) == 1
    return source[:m.start()] + body.replace(decl, '  unsigned sh3_ray=0;\n' + decl, 1) + source[m.end():]


if __name__ == '__main__':
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('source', type=Path); p.add_argument('output', type=Path); a = p.parse_args()
    if a.output.resolve().is_relative_to(a.source.resolve().parent):
        p.error('Keep output outside generated source tree')
    name = a.source.name
    gen = generate_059 if name == 'x86_f059.c' else generate_067 if name == 'x86_f067.c' else None
    if gen is None:
        p.error('Unit must be x86_f059.c or x86_f067.c')
    result = 'extern void diag_loop_guard_trip(unsigned,unsigned,unsigned,unsigned,unsigned);\n' + gen(a.source.read_text(encoding='utf-8'))
    a.output.parent.mkdir(parents=True, exist_ok=True); a.output.write_text(result, encoding='utf-8')
