#!/usr/bin/env python3
"""Differential test of emitted x86 rotates against Unicorn, including flags."""
import ctypes
from pathlib import Path
import random
import subprocess
import tempfile
import unittest
import capstone
from unicorn import Uc, UC_ARCH_X86, UC_MODE_32
from unicorn.x86_const import UC_X86_REG_EAX, UC_X86_REG_ECX, UC_X86_REG_EFLAGS
import x86rec
from test_memcpy_generated import CPU

ROOT=Path(__file__).resolve().parents[1]
CASES={'rol_ch_1':'d0c5','ror_ch_1':'d0cd','rol_ch_cl':'d2c5','ror_ch_cl':'d2cd',
       'rol_ax_cl':'66d3c0','ror_ax_cl':'66d3c8','rol_eax_cl':'d3c0','ror_eax_cl':'d3c8'}

class RotateDifferential(unittest.TestCase):
    def test_rotates(self):
        md=capstone.Cs(capstone.CS_ARCH_X86,capstone.CS_MODE_32); md.detail=True
        parts=['#include "x86rt.h"']
        for name,code in CASES.items():
            ins=next(md.disasm(bytes.fromhex(code),0x1000))
            f=object.__new__(x86rec.Func); f.out=[]; f.static=False; f.flagsrc=None
            f.insn(ins)
            parts.append('void '+name+'(CPU *c) { uint32_t eax=c->eax,ecx=c->ecx; '
                         'uint32_t cf=1,pf=1,zf=1,sf=1,of=1; uint8_t *mb=0; (void)mb; '
                         + '\n'.join(f.out) + '\nc->eax=eax;c->ecx=ecx;c->eflags_ext=cf|(pf<<2)|(zf<<6)|(sf<<7)|(of<<11); }')
        with tempfile.TemporaryDirectory(prefix='sh3-rotate-test-') as td:
            src,lib=Path(td)/'rotate.c',Path(td)/'rotate.dylib'; src.write_text('\n'.join(parts))
            subprocess.run(['clang','-std=gnu11','-shared','-fPIC','-I',str(ROOT/'port/include'),str(src),'-o',str(lib)],check=True)
            dll=ctypes.CDLL(str(lib)); rng=random.Random(0x534833)
            for name,code in CASES.items():
                fun=getattr(dll,name); fun.argtypes=[ctypes.POINTER(CPU)]
                for _ in range(500):
                    eax=rng.getrandbits(32); ecx=rng.getrandbits(32); count=1 if name.endswith('_1') else ecx&0xff
                    c=CPU(); c.eax=eax; c.ecx=ecx; fun(ctypes.byref(c))
                    u=Uc(UC_ARCH_X86,UC_MODE_32); u.mem_map(0x1000,0x1000); u.mem_write(0x1000,bytes.fromhex(code))
                    u.reg_write(UC_X86_REG_EAX,eax); u.reg_write(UC_X86_REG_ECX,ecx); u.reg_write(UC_X86_REG_EFLAGS,0x8c7)
                    u.emu_start(0x1000,0x1000+len(bytes.fromhex(code)))
                    self.assertEqual((c.eax,c.ecx),(u.reg_read(UC_X86_REG_EAX),u.reg_read(UC_X86_REG_ECX)),(name,eax,ecx))
                    flags_mask=0xc5 | (0x800 if (count&31)<=1 else 0)
                    self.assertEqual(c.eflags_ext&flags_mask,u.reg_read(UC_X86_REG_EFLAGS)&flags_mask,(name,eax,ecx,count))
        print('generated ROL/ROR: 4000 Unicorn comparisons passed')

if __name__=='__main__': unittest.main()
