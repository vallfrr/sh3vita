#!/usr/bin/env python3
"""Differential tests of emitted SSE move/XOR code against Unicorn x86."""
import ctypes
from pathlib import Path
import random
import subprocess
import tempfile
import unittest

import capstone
from unicorn import Uc, UC_ARCH_X86, UC_MODE_32
from unicorn.x86_const import UC_X86_REG_EAX, UC_X86_REG_XMM0, UC_X86_REG_XMM1
import x86rec

ROOT = Path(__file__).resolve().parents[1]
CASES = {
    "movaps_rr": ("0f28c1", 0x3000), "movups_rr": ("0f10c1", 0x3000),
    "movss_rr": ("f30f10c1", 0x3000), "xorps_rr": ("0f57c1", 0x3000),
    "movaps_load": ("0f2800", 0x3000), "movaps_store": ("0f2900", 0x3000),
    "movups_load": ("0f1000", 0x3003), "movups_store": ("0f1100", 0x3003),
    "movss_load": ("f30f1000", 0x3003), "movss_store": ("f30f1100", 0x3003),
    "xorps_load": ("0f5700", 0x3000), "xorps_self": ("0f57c0", 0x3000),
    "movaps_self": ("0f28c0", 0x3000),
}


class CPU(ctypes.Structure):
    _fields_ = [(n, ctypes.c_uint32) for n in ("eax", "ecx", "edx", "ebx", "esp", "ebp", "esi", "edi", "df", "eflags_ext", "fs_base")] + [
        ("st", ctypes.c_double * 8), ("ftop", ctypes.c_uint32), ("fsw", ctypes.c_uint32), ("fcw", ctypes.c_uint32),
        ("xmm", (ctypes.c_uint8 * 16) * 8),
    ]


class Notes:
    def note(self, *args):
        pass


class SseDifferential(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory(prefix="sh3-sse-test-")
        d = Path(cls.tmp.name)
        decoder = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
        decoder.detail = True
        wrappers = ['#include "x86rt.h"', '#include <stdlib.h>',
                    'void rt_unimpl(CPU *c, uint32_t pc, const char *what) { (void)c; (void)pc; (void)what; abort(); }']
        for name, (encoded, _) in CASES.items():
            ins = next(decoder.disasm(bytes.fromhex(encoded), 0x1000))
            f = object.__new__(x86rec.Func)
            f.out, f.rec, f.static = [], Notes(), False
            if not f.sse_move_logic(ins):
                raise AssertionError(f"unsupported test instruction {name}: {ins.mnemonic} {ins.op_str}")
            wrappers.append(f'void {name}(CPU *c, uint8_t *mb) {{ '
                            'uint32_t eax=c->eax,ecx=c->ecx,edx=c->edx,ebx=c->ebx,esp=c->esp,ebp=c->ebp,esi=c->esi,edi=c->edi; '
                            + '\n'.join(f.out) + ' }')
        src, lib = d / "test.c", d / "test.dylib"
        src.write_text('\n'.join(wrappers))
        subprocess.run(["clang", "-std=c11", "-shared", "-fPIC", "-I", str(ROOT / "port/include"), str(src), "-o", str(lib)], check=True)
        cls.lib = ctypes.CDLL(str(lib))
        for name in CASES:
            getattr(cls.lib, name).argtypes = [ctypes.POINTER(CPU), ctypes.POINTER(ctypes.c_uint8)]

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def test_against_x86(self):
        rng = random.Random(0x534833)
        for name, (encoded, addr) in CASES.items():
            with self.subTest(instruction=name):
                for _ in range(100):
                    c = CPU(); c.eax = addr
                    x0 = rng.randbytes(16); x1 = rng.randbytes(16); mem = rng.randbytes(64)
                    c.xmm[0][:] = x0; c.xmm[1][:] = x1
                    ram = (ctypes.c_uint8 * 0x5000)()
                    ram[0x3000:0x3040] = mem
                    u = Uc(UC_ARCH_X86, UC_MODE_32); u.mem_map(0x1000, 0x4000)
                    code = bytes.fromhex(encoded); u.mem_write(0x1000, code); u.mem_write(0x3000, mem)
                    u.reg_write(UC_X86_REG_EAX, addr)
                    u.reg_write(UC_X86_REG_XMM0, int.from_bytes(x0, "little"))
                    u.reg_write(UC_X86_REG_XMM1, int.from_bytes(x1, "little"))
                    u.emu_start(0x1000, 0x1000 + len(code))
                    getattr(self.lib, name)(ctypes.byref(c), ram)
                    self.assertEqual(bytes(c.xmm[0]), u.reg_read(UC_X86_REG_XMM0).to_bytes(16, "little"))
                    self.assertEqual(bytes(c.xmm[1]), u.reg_read(UC_X86_REG_XMM1).to_bytes(16, "little"))
                    self.assertEqual(bytes(ram[0x3000:0x3040]), bytes(u.mem_read(0x3000, 64)))


if __name__ == "__main__":
    unittest.main()
