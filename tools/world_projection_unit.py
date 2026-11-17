#!/usr/bin/env python3
"""Record every world-to-screen projection of the effect system, in a signature-checked copy of unit 116.

F_0059ef90 projects a world point and writes its integer GS X, Y, Z (and a flag word) to its first argument; the effect
producers (blood, footprints, other floor/air effects) put these in the GS packets that the generic decoder later converts with
F_00682ba0. diag_widescreen_hook.c keeps those vertices out of the 4:3 interface squeeze, otherwise they slide against the scene
when the camera moves (lamp halos, blood and footprints all did)."""
import argparse, hashlib, re
from pathlib import Path
SHA = '943e7a5969fc0a09b965e835578883d9f6608b087f90f374ad1071ab04a90780'
RET = '  // 0x59f0e8: ret \n'


def generate(source):
    m = re.search(r'void F_0059ef90\(CPU \*c\) \{.*?\n\}', source, re.S)
    if not m or hashlib.sha256(m[0].encode()).hexdigest() != SHA:
        raise ValueError('Projection function changed: F_0059ef90')
    if m[0].count(RET) != 1:
        raise ValueError('Projection return not found')
    body = m[0].replace(RET, RET + '  diag_world_note_xyz(R32(esp+4u));\n', 1)   # esp is back at the return address: arg 1
    return 'extern void diag_world_note_xyz(unsigned);\n' + source[:m.start()] + body + source[m.end():]


if __name__ == '__main__':
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('source', type=Path); p.add_argument('output', type=Path); a = p.parse_args()
    if a.output.resolve().is_relative_to(a.source.resolve().parent):
        p.error('Keep output outside generated source tree')
    result = generate(a.source.read_text(encoding='utf-8'))
    a.output.parent.mkdir(parents=True, exist_ok=True); a.output.write_text(result, encoding='utf-8')
