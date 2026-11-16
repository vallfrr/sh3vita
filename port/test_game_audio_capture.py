#!/usr/bin/env python3
"""Exercise the actual host-only game audio capture, including no-overwrite."""
import os
import struct
import subprocess
import tempfile
from pathlib import Path

source = (Path(__file__).parent / 'sh4base/dx/dsound.c').read_text(encoding='utf-8')
start = source.index('static void diag_capture_game_audio(')
end = source.index('\n#endif', start)
shim = '''
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#define OUT_RATE 48000
#define rt_log(...) ((void)0)
'''
test = '''
int main(int argc, char **argv) {
    if (argc != 2 || setenv("SH3_DIAG_GAME_AUDIO_CAPTURE", argv[1], 1)) return 1;
    const int16_t samples[] = { 0, 1234, -1234, 32767, -32768, 0 };
    diag_capture_game_audio(samples, 3);
    return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='sh3-game-audio-') as directory:
    root = Path(directory)
    (root / 'test.c').write_text(shim + source[start:end] + test, encoding='utf-8')
    subprocess.run([os.environ.get('CC', 'cc'), '-std=gnu11', '-Wall', '-Wextra',
                    '-Werror', str(root / 'test.c'), '-o', str(root / 'test')], check=True)
    output = root / 'audio.raw'
    subprocess.run([str(root / 'test'), str(output)], check=True)
    assert struct.unpack('<6h', output.read_bytes()) == (0, 1234, -1234, 32767, -32768, 0)
    output.write_bytes(b'preserve')
    subprocess.run([str(root / 'test'), str(output)], check=True)
    assert output.read_bytes() == b'preserve'
    print('game audio capture output and no-overwrite passed')
