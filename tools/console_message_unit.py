#!/usr/bin/env python3
"""Expose one native message compatibility wrapper; never edit generated source in place."""
import argparse, hashlib, re
from pathlib import Path
EXPECTED = 'b9e5e6e248a5d5ac7e12dfd1b74cab74a62759876a0a74f5405b3d61b12a625c'
def replace(source):
    render = re.search(r'void F_005fe4a0\(CPU \*c\) \{.*?\n\}', source, re.S)
    if render:
        if hashlib.sha256(render[0].encode()).hexdigest() != '53a64b8631f34e3ddb9c23491f71256e4ae1a9f8f5dd7cac671c013984a50a33':
            raise ValueError('Unsupported pause renderer; no unit generated')
        body = render[0].replace('  // 0x5fe4c1: test eax, eax',
            '  diag_console_msg_trace(R32(esp+4u),R16(esp+8u),eax,R32(esp));\n  eax=diag_console_option_message(eax,R16(esp+8u),R32(esp));\n  eax=diag_console_pause_message(eax,R32(esp));\n  // 0x5fe4c1: test eax, eax', 1)
        return '#include <stdint.h>\nextern uint32_t diag_console_option_message(uint32_t p,uint32_t id,uint32_t caller);\nextern uint32_t diag_console_pause_message(uint32_t p,uint32_t caller);\nextern void diag_console_msg_trace(uint32_t table,uint32_t id,uint32_t p,uint32_t caller);\n'+source[:render.start()]+body+source[render.end():]
    match = re.search(r'void F_005fa400\(CPU \*c\) \{.*?\n\}', source, re.S)
    if not match or hashlib.sha256(match[0].encode()).hexdigest() != EXPECTED:
        raise ValueError('Unsupported UI label function; no unit generated')
    body = match[0].replace('void F_005fa400(', 'void F_005fa400_orig(', 1)
    return source[:match.start()] + body + source[match.end():]
if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('source', type=Path); parser.add_argument('output', type=Path)
    args = parser.parse_args()
    if args.output.resolve().is_relative_to(args.source.resolve().parent):
        raise SystemExit('Output must be outside generated directory')
    data = replace(args.source.read_text(encoding='utf-8'))
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(data, encoding='utf-8')
