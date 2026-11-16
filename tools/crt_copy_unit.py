#!/usr/bin/env python3
"""Repair internal computed jumps of the retained MSVC overlap-safe copy.

Keep every original instruction and ABI effect. The old translation treated
some alignment/tail jump-table destinations as external function calls.
"""
import argparse,hashlib,re
from pathlib import Path
SIGNATURE='69b06efc44915fec65416dd7316813e69c70dfa7f00693e938a3d3b0f86f2020'
# Original PC .text jump-table words, validated against instruction boundaries.
TARGETS=(0x661d90,0x661dbc,0x661de0,0x661e20,0x661e28,0x661e30,0x661e38,0x661e40,0x661e48,0x661e50,0x661e63,0x661e7c,0x661e84,0x661e90,0x661ea4,0x661f1c,0x661f40,0x661f68,0x661fbc,0x661fc4,0x661fcc,0x661fd4,0x661fdc,0x661fe4,0x661fec,0x661fff,0x662018,0x662020,0x662030,0x662044)
def fixed_body(source):
 start=source.index('void F_00661d20(CPU *c) {');stop=source.index('\n}',start)+2;body=source[start:stop]
 if hashlib.sha256(body.encode()).hexdigest()!=SIGNATURE:raise ValueError('CRT copy body changed; re-audit computed jumps')
 body=body.replace('  uint8_t *const mb = g_mem;','  uint8_t *const mb = g_mem;\n  uint32_t crt_copy_target=0;',1)
 pattern=r'  \{ uint32_t t=R32\(([^\n]+?)\);[^\n]+rt_call\(c,t\); return;[^\n]*\n'
 body,count=re.subn(pattern,lambda m:'  { crt_copy_target=R32('+m[1]+'); goto L_crt_copy_dispatch; }\n',body)
 if count!=16:raise ValueError('Expected 16 retained internal table jumps')
 for target in TARGETS:
  label='L_'+format(target,'08x')
  if label+':;' not in body:
   needle='  // 0x'+format(target,'x')+':'
   if needle not in body:raise ValueError('Destination instruction missing')
   body=body.replace(needle,label+':;\n'+needle,1)
 dispatch='\nL_crt_copy_dispatch:;\n  switch(crt_copy_target) {\n'
 dispatch+=''.join('    case 0x%08xu: goto L_%08x;\n'%(t,t) for t in TARGETS)
 dispatch+='    default: c->eax=eax;c->ecx=ecx;c->edx=edx;c->ebx=ebx;c->esp=esp;c->ebp=ebp;c->esi=esi;c->edi=edi;rt_call(c,crt_copy_target);return;\n  }\n'
 body=body[:-2]+dispatch+'}'
 return start,stop,body
def replace(source):
 a,b,body=fixed_body(source);return source[:a]+body+source[b:]
if __name__=='__main__':
 ap=argparse.ArgumentParser(description=__doc__);ap.add_argument('source',type=Path);ap.add_argument('output',type=Path);ap.add_argument('--function-only',action='store_true');a=ap.parse_args()
 if a.output.resolve().is_relative_to(a.source.resolve().parent):raise SystemExit('Output must be outside generated directory')
 s=a.source.read_text();a.output.parent.mkdir(parents=True,exist_ok=True);a.output.write_text(fixed_body(s)[2] if a.function_only else replace(s))
