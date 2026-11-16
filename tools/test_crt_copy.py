#!/usr/bin/env python3
"""Linux/WSL regression of the actual retained CRT copy against host memmove.
Generated game instructions and PE tables are written only under build/.
"""
import argparse,os,re,struct,subprocess
from pathlib import Path
from crt_copy_unit import fixed_body,TARGETS
ROOT=Path(__file__).resolve().parents[1]
def main():
 ap=argparse.ArgumentParser(description=__doc__);ap.add_argument('--out',type=Path,default=ROOT/'build/tests/crt-copy');ap.add_argument('--sanitize',action='store_true');a=ap.parse_args();out=a.out.resolve();out.mkdir(parents=True,exist_ok=True)
 body=fixed_body((ROOT/'port/gen/x86_f180.c').read_text())[2]
 for i,target in enumerate(TARGETS):body=body.replace('case 0x%08xu: goto'%target,'case 0x%08xu: crt_seen[%u]++; goto'%(target,i))
 (out/'crt-copy-function.inc').write_text(body)
 pebytes=(ROOT/'game/sh3.exe').read_bytes();pe=struct.unpack_from('<I',pebytes,60)[0];n=struct.unpack_from('<H',pebytes,pe+6)[0];sz=struct.unpack_from('<H',pebytes,pe+20)[0];fixture=None
 for i in range(n):
  o=pe+24+sz+40*i;vs,va,rs,ro=struct.unpack_from('<IIII',pebytes,o+8)
  if va<=0x261d20 and 0x262060<=va+rs:
   start=ro+0x261d20-va;fixture=pebytes[start:start+0x340];break
 if fixture is None or len(fixture)!=0x340:raise ValueError('CRT original .text fixture missing')
 words={struct.unpack_from('<I',fixture,i)[0] for i in range(0,len(fixture),4)}
 if not set(TARGETS)<=words:raise ValueError('Original PE table targets changed')
 (out/'tables.bin').write_bytes(fixture)
 command=[os.environ.get('CC','gcc'),'-std=gnu11','-O2','-I'+str(ROOT/'port/include'),'-I'+str(out),str(ROOT/'port/test_crt_copy.c'),'-lm','-o',str(out/'test')]
 if a.sanitize:command.insert(1,'-fsanitize=address,undefined')
 subprocess.run(command,check=True);subprocess.run([str(out/'test'),str(out/'tables.bin')],check=True)
if __name__=='__main__':main()
