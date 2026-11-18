#!/usr/bin/env python3
"""Record every world-to-screen projection of the effect system, in a signature-checked copy of unit 116.

F_0059ef90 and its twin F_0059ee30 project a world point and write its integer GS X, Y, Z (and a flag word) to their first
argument; the effect producers (blood, footprints, other floor/air effects) put these in the GS packets that the generic decoder
later converts with F_00682ba0. diag_widescreen_hook.c keeps those vertices out of the 4:3 interface squeeze, otherwise they
slide against the scene when the camera moves (lamp halos, blood and footprints all did). F_0059ee30 is the one the bloody
footprints use (F_005c7a00; issue #3: they still slid with only F_0059ef90 recorded)."""
import argparse, hashlib, re
from pathlib import Path
PROJECTIONS = {
    '0059ef90': ('943e7a5969fc0a09b965e835578883d9f6608b087f90f374ad1071ab04a90780', '  // 0x59f0e8: ret \n'),
    '0059ee30': ('ccba6a61a467d0c5d563ca025180028af0f8f4a1f3bbb7d2d19d71f52335d4ca', '  // 0x59ef88: ret \n'),
}


def generate(source):
    for name, (sha, ret) in PROJECTIONS.items():
        m = re.search(r'void F_' + name + r'\(CPU \*c\) \{.*?\n\}', source, re.S)
        if not m or hashlib.sha256(m[0].encode()).hexdigest() != sha:
            raise ValueError('Projection function changed: F_' + name)
        if m[0].count(ret) != 1:
            raise ValueError('Projection return not found: F_' + name)
        body = m[0].replace(ret, ret + '  diag_world_note_xyz(R32(esp+4u));\n', 1)   # esp is back at the return address: arg 1
        source = source[:m.start()] + body + source[m.end():]
    return 'extern void diag_world_note_xyz(unsigned);\n' + source


if __name__ == '__main__':
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('source', type=Path); p.add_argument('output', type=Path); a = p.parse_args()
    if a.output.resolve().is_relative_to(a.source.resolve().parent):
        p.error('Keep output outside generated source tree')
    result = generate(a.source.read_text(encoding='utf-8'))
    a.output.parent.mkdir(parents=True, exist_ok=True); a.output.write_text(result, encoding='utf-8')
