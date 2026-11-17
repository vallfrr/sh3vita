#!/usr/bin/env python3
"""Tag native blood packet coordinates in isolated, signature-checked units."""
import argparse, hashlib, re
from pathlib import Path
SPECS = {
    '005ba050': ('d36ceb7663d00bb15e3e1133252546cb37f266a2c0dd445bebfe6b7145f09dad', 'L_005ba85e:;'),
    '004bc200': ('7d2a155d496a7be05fee809c99220f127a13e4249e7ea8643cd6585562f3e0be', 'L_004bcacf:;'),
}
def generate(source):
    count=0
    for name,(sha,label) in SPECS.items():
        m=re.search(r'void F_'+name+r'\(CPU \*c\) \{.*?\n\}',source,re.S)
        if not m:continue
        if hashlib.sha256(m[0].encode()).hexdigest()!=sha:raise ValueError('Blood function changed: '+name)
        assert m[0].count(label)==1
        body=m[0].replace(label,label+'\n  diag_blood_note_packet(R32(ebx));',1)
        source=source[:m.start()]+body+source[m.end():];count+=1
    if count!=1:raise ValueError('Expected one blood producer')
    return 'extern void diag_blood_note_packet(unsigned);\n'+source
if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('source',type=Path);p.add_argument('output',type=Path);a=p.parse_args()
    if a.output.resolve().is_relative_to(a.source.resolve().parent):p.error('Keep output outside generated source tree')
    result=generate(a.source.read_text(encoding='utf-8'));a.output.parent.mkdir(parents=True,exist_ok=True);a.output.write_text(result,encoding='utf-8')
