#!/usr/bin/env python3
"""Exercise SH3's generated CRT memcpy/memmove across alignments and overlaps.

Requires generated C and the local PE. The reference is Python's snapshot copy;
indirect branches outside the generated function fail explicitly.
"""
import ctypes
from pathlib import Path
import struct
import subprocess
import tempfile
import unittest

from pe import PE
class CPU(ctypes.Structure):
    _fields_ = [(n, ctypes.c_uint32) for n in ('eax', 'ecx', 'edx', 'ebx', 'esp', 'ebp', 'esi', 'edi', 'df', 'eflags_ext', 'fs_base')] + [
        ('st', ctypes.c_double * 8), ('ftop', ctypes.c_uint32), ('fsw', ctypes.c_uint32), ('fcw', ctypes.c_uint32),
        ('xmm', (ctypes.c_uint8 * 16) * 8),
    ]

ROOT = Path(__file__).resolve().parents[1]

class GeneratedMemcpy(unittest.TestCase):
    def test_copy(self):
        sources = list((ROOT / 'port/gen').glob('x86_f*.c'))
        for path in sources:
            text = path.read_text()
            start = text.find('void F_0066f9c0(CPU *c) {')
            if start >= 0:
                end = text.find('\nvoid F_', start + 1)
                body = text[start:end if end >= 0 else None]
                break
        else:
            self.fail('generated memcpy not found')
        pe = PE(str(ROOT / 'game/sh3.exe'))
        code = pe.read(0x66f900, 0x500)
        self.assertEqual(len(code), 0x500)
        with tempfile.TemporaryDirectory(prefix='sh3-memcpy-test-') as td:
            src, lib = Path(td)/'memcpy.c', Path(td)/'memcpy.dylib'
            src.write_text('#include "x86rt.h"\nuint8_t *g_mem; uint32_t bad;\n'
                           'void rt_call(CPU *c,uint32_t target){(void)c;bad=target;}\n' + body)
            subprocess.run(['clang','-std=gnu11','-w','-shared','-fPIC','-I',str(ROOT/'port/include'),str(src),'-o',str(lib)],check=True)
            dll=ctypes.CDLL(str(lib)); run=dll.F_0066f9c0; run.argtypes=[ctypes.POINTER(CPU)]
            memory=(ctypes.c_ubyte * (8*1024*1024))()
            ctypes.c_void_p.in_dll(dll,'g_mem').value=ctypes.addressof(memory)
            memory[0x66f900:0x66fe00]=code
            region=0x20000; stack=0x10000; count=0
            for src_off in (0,1,2,3,16,32,48):
                for dst_off in (0,1,2,3,4,7,16,32,48,80):
                    for n in range(65):
                        data=bytearray((i*37+11)&255 for i in range(160))
                        expected=data[:]; expected[dst_off:dst_off+n]=data[src_off:src_off+n]
                        memory[region:region+160]=data
                        memory[stack:stack+16]=struct.pack('<4I',0,region+dst_off,region+src_off,n)
                        c=CPU(); c.esp=stack; c.ebp=0x12345678; c.esi=0x11111111; c.edi=0x22222222
                        ctypes.c_uint32.in_dll(dll,'bad').value=0
                        run(ctypes.byref(c))
                        bad=ctypes.c_uint32.in_dll(dll,'bad').value
                        self.assertEqual(bad,0, f'unresolved {bad:08x} src={src_off} dst={dst_off} n={n}')
                        self.assertEqual(bytes(memory[region:region+160]),bytes(expected),f'src={src_off} dst={dst_off} n={n}')
                        self.assertEqual((c.eax,c.esp,c.ebp,c.esi,c.edi),(region+dst_off,stack+4,0x12345678,0x11111111,0x22222222))
                        count+=1
            print(f'generated memcpy/memmove: {count} cases passed')

if __name__=='__main__': unittest.main()
