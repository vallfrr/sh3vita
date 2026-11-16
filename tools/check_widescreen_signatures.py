#!/usr/bin/env python3
"""Read-only signature audit for the SH3 PC diagnostic executable.

Patterns are from ThirteenAG/WidescreenFixesPack, SilentHill3.FusionFix,
Widescreen.ixx, commit fc975db74c99fef6b1a248fcd35816b48e65edf3.
The patterns locate candidate hooks; they do not apply a widescreen patch.
"""
import argparse
import re
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'recomp'))
from pe import PE

PATTERNS = {
    'render_target': ('A1 ? ? ? ? 53 8B 5C 24 0C 3B C3 55 8B 6C 24 14', 1),
    'present_size': ('BF ? ? ? ? F3 AB 52', 1),
    'display_size': ('8B 0D ? ? ? ? 8B 44 24 04 3B C8 8B 4C 24 08 75 08 39 0D', 2),
    'projection': ('6A 02 E8 ? ? ? ? 83 C4 04 85 C0 74 07 D9 05 ? ? ? ? C3 D9 05 ? ? ? ? D8 0D ? ? ? ? C3', 2),
    'create_device': ('89 15 ? ? ? ? 8B 10 50 FF 52 3C 85 C0', 1),
}

def audit(path):
    pe = PE(str(path))
    all_ok = True
    for label, (pattern, expected) in PATTERNS.items():
        regex = b''.join(b'.' if token == '?' else re.escape(bytes([int(token, 16)])) for token in pattern.split())
        matches = []
        for _, va, _, raw, size, flags in pe.sections:
            if flags & 0x20000000:
                matches.extend(va + m.start() for m in re.finditer(regex, pe.data[raw:raw+size], re.DOTALL))
        print(f'{label}: {len(matches)} hits', ', '.join(f'{a:08x}' for a in matches))
        all_ok &= len(matches) == expected
    return all_ok

if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('exe', type=Path)
    sys.exit(0 if audit(parser.parse_args().exe) else 1)
