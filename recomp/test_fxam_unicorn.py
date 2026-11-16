#!/usr/bin/env python3
"""Compare FXAM after FLD m64fp to Unicorn; empty tags/80-bit values excluded."""
import ctypes
from pathlib import Path
import struct
import subprocess
import tempfile
import unittest
from unicorn import Uc, UC_ARCH_X86, UC_MODE_32
from unicorn.x86_const import UC_X86_REG_EAX
from test_memcpy_generated import CPU

ROOT=Path(__file__).resolve().parents[1]
class FxamDifferential(unittest.TestCase):
    def test_values(self):
        vals=[0,0x8000000000000000,0x3ff0000000000000,0xbff0000000000000,
              0x7ff0000000000000,0xfff0000000000000,0x7ff8000000000000,
              1,0x8000000000000001,0x000fffffffffffff,0x0010000000000000]
        with tempfile.TemporaryDirectory(prefix='sh3-fxam-') as td:
            src=Path(td)/'fxam.c'; lib=Path(td)/'fxam.dylib'
            src.write_text('#include "x86rt.h"\nvoid run(CPU *c,double x){rt_fxam(c,x);}\n')
            subprocess.run(['clang','-std=c11','-shared','-fPIC','-I',str(ROOT/'port/include'),str(src),'-o',str(lib)],check=True)
            fn=ctypes.CDLL(str(lib)).run; fn.argtypes=[ctypes.POINTER(CPU),ctypes.c_double]
            code=bytes.fromhex('dd0500200000d9e5dfe0')
            for bits in vals:
                u=Uc(UC_ARCH_X86,UC_MODE_32); u.mem_map(0x1000,0x2000)
                u.mem_write(0x1000,code); u.mem_write(0x2000,struct.pack('<Q',bits)); u.emu_start(0x1000,0x1000+len(code))
                expected=u.reg_read(UC_X86_REG_EAX)&0x4700
                c=CPU(); fn(ctypes.byref(c),struct.unpack('<d',struct.pack('<Q',bits))[0])
                self.assertEqual(c.fsw&0x4700,expected,hex(bits))
        print('FXAM FLD-double classification: 11 Unicorn comparisons passed')
if __name__=='__main__': unittest.main()
