#!/usr/bin/env python3
"""Verify the experimental playtest default and its one-shot opt-out."""
import os
from pathlib import Path
import subprocess
import tempfile

source = (Path(__file__).resolve().parent / 'diag_runtime.c').read_text(encoding='utf-8')
start = source.index('#ifdef SH3_PLAYTEST_PERFDEFAULT_LAB\n    /* Isolated candidate')
end = source.index('\n#endif', start) + len('\n#endif')
shim = r'''
#define _POSIX_C_SOURCE 200809L
#define SH3_PLAYTEST_PERFDEFAULT_LAB 1
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static int g_pace60_park, g_pace60_clamp, g_pace60_all_rooms, marker;
static int fake_unlink(const char *p) { (void)p; return marker ? 0 : -1; }
#define unlink fake_unlink
static void apply(void) {
'''
test = r'''
}
#include <assert.h>
int main(int argc, char **argv) {
    (void)argc;
    marker=atoi(argv[1]);
    if (atoi(argv[2])) setenv("SH3_ALL_NATIVE","0",1);
    apply();
    assert(g_pace60_park == !marker && g_pace60_clamp == !marker && g_pace60_all_rooms == !marker);
    const char *eager=getenv("SH3_TEX_EAGER"), *native=getenv("SH3_ALL_NATIVE");
    if (marker) assert(!eager && (!native || !strcmp(native,"0")));
    else { assert(eager && !strcmp(eager,"1")); assert(native && !strcmp(native,atoi(argv[2])?"0":"1")); }
    return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='sh3-perfdefault-') as tmp:
    unit = Path(tmp) / 'test.c'
    binary = Path(tmp) / 'test'
    unit.write_text(shim + source[start:end] + test, encoding='utf8')
    subprocess.run([os.environ.get('CC', 'cc'), '-std=c11', '-Wall', '-Wextra',
                    '-Werror', str(unit), '-o', str(binary)], check=True)
    for marker in (0, 1):
        for explicit in (0, 1):
            env = os.environ.copy()
            for key in ('SH3_ALL_NATIVE', 'SH3_TEX_EAGER', 'SH3_PERFDEFAULT_OFF'):
                env.pop(key, None)
            subprocess.run([str(binary), str(marker), str(explicit)], env=env,
                           check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
print('experimental performance defaults and opt-out test passed')
