#!/usr/bin/env python3
"""Tag the frames where a cursor screen (puzzles, events) is updated, in a signature-checked copy of unit 072.

F_004dd130 is the game's own cursor update (position 0xbb3184/0xbb3188, from the pad axes, the D-pad or, when the mouse is
"active", from the mouse). The Vita has no mouse: the runtime drives a virtual mouse (left stick = move, Cross = left click)
only while this function runs, see diag_runtime.c (diag_cursor_ui_tick / GetCursorPos / GetKeyState)."""
import argparse, hashlib, re
from pathlib import Path
SHA = '658c9c4a4784148e1e36799dcaed4d034274196f437d17e834774f6314524b8a'


def generate(source):
    m = re.search(r'void F_004dd130\(CPU \*c\) \{.*?\n\}', source, re.S)
    if not m or hashlib.sha256(m[0].encode()).hexdigest() != SHA:
        raise ValueError('Cursor update function changed: F_004dd130')
    body = m[0].replace('{\n', '{\n  diag_cursor_ui_tick();\n', 1)
    return 'extern void diag_cursor_ui_tick(void);\n' + source[:m.start()] + body + source[m.end():]


if __name__ == '__main__':
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('source', type=Path); p.add_argument('output', type=Path); a = p.parse_args()
    if a.output.resolve().is_relative_to(a.source.resolve().parent):
        p.error('Keep output outside generated source tree')
    result = generate(a.source.read_text(encoding='utf-8'))
    a.output.parent.mkdir(parents=True, exist_ok=True); a.output.write_text(result, encoding='utf-8')
