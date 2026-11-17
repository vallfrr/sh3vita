#!/usr/bin/env python3
"""Verify opt-in particle drawing omission and guest cdecl stack behavior."""
import os
from pathlib import Path
import subprocess
import tempfile

source = (Path(__file__).resolve().parent / 'diag_runtime.c').read_text(encoding='utf8')
start = source.index('static int audit_skip_particle_draw(')
end = source.index('\nvoid __wrap_F_00479e30', start)
shim = r'''
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
typedef struct { uint32_t esp, eax, eflags; } CPU;
static unsigned g_audit_particle_draw_skipped;
'''
test = r'''
#include <assert.h>
int main(int argc, char **argv) {
    (void)argc;
    CPU c={0x1000u,0x12345678u,0x246u}; int expected=atoi(argv[1]);
    assert(audit_skip_particle_draw(&c)==expected);
    assert(c.esp==0x1000u+(expected?4u:0u));
    assert(c.eax==0x12345678u && c.eflags==0x246u);
    return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='sh3-particle-skip-') as tmp:
    unit = Path(tmp) / 'test.c'; binary = Path(tmp) / 'test'
    unit.write_text(shim + source[start:end] + test, encoding='utf8')
    subprocess.run([os.environ.get('CC', 'cc'), '-std=c11', '-Wall', '-Wextra',
                    '-Werror', str(unit), '-o', str(binary)], check=True)
    for enabled in (False, True):
        env = os.environ.copy(); env.pop('SH3_AUDIT_PARTICLE_DRAW_OFF', None)
        if enabled: env['SH3_AUDIT_PARTICLE_DRAW_OFF']='1'
        subprocess.run([str(binary), str(int(enabled))], env=env, check=True,
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
print('particle draw gate and cdecl stack test passed')
