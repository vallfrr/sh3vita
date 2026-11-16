#!/usr/bin/env python3
"""Check the actual shadow wrapper gate and guest cdecl stack behavior."""
import os
from pathlib import Path
import subprocess
import tempfile

source = (Path(__file__).resolve().parent / 'diag_runtime.c').read_text(encoding='utf-8')
start = source.index('static int audit_skip_shadow(')
end = source.index('\nvoid __wrap_F_0048e140', start)
shim = r'''
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef struct { uint32_t esp, eax, eflags; } CPU;
'''
test = r'''
#include <assert.h>
int main(int argc, char **argv) {
    (void)argc;
    CPU c={0x1000u,0x12345678u,0x246u};
    int expected=atoi(argv[1]);
    assert(audit_skip_shadow(&c)==expected);
    assert(c.esp==0x1000u+(expected ? 4u : 0u));
    assert(c.eax==0x12345678u && c.eflags==0x246u);
    return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='sh3-shadow-skip-') as tmp:
    unit = Path(tmp) / 'test.c'
    binary = Path(tmp) / 'test'
    unit.write_text(shim + source[start:end] + test, encoding='utf-8')
    cc = os.environ.get('CC', 'cc')
    for name, flags, default in [('vita', ['-D__vita__', '-DSH3_PLAYTEST_PROFILE'], 1),
                                  ('other', [], 0)]:
        exe = binary.with_name(name)
        subprocess.run([cc, '-std=c11', '-Wall', '-Wextra', '-Werror', *flags,
                        str(unit), '-o', str(exe)], check=True)
        for override, expected in [(None, default), ('0', 0), ('1', 1)]:
            env = os.environ.copy()
            env.pop('SH3_AUDIT_SHADOW_OFF', None)
            if override is not None:
                env['SH3_AUDIT_SHADOW_OFF'] = override
            subprocess.run([str(exe), str(expected)], env=env, check=True,
                           stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
print('shadow gate and cdecl stack test passed')
