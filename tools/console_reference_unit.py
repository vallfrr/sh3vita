#!/usr/bin/env python3
"""Replace PC binding capture/draw with fixed SH3 Vita reference page."""
import argparse, hashlib, re
from pathlib import Path
FUNCTIONS={
 '005edc50':('9d9bce1298c6a176f6a8268f9eee8b5acaf1c404da5493d6dbd879aec13a9461','diag_controls_reference_input'),
 '005ee080':('49b7392d3f842a7d0f876bfab0da11330b061b4e04c9f3989c95bde66d22fa3e','diag_controls_reference_draw'),
}
def replace(source):
    count=0
    for name,(sha,helper) in FUNCTIONS.items():
        m=re.search(r'void F_'+name+r'\(CPU \*c\) \{.*?\n\}',source,re.S)
        if not m: continue
        if hashlib.sha256(m[0].encode()).hexdigest()!=sha: raise ValueError('Unsupported controls page '+name)
        source=source[:m.start()]+'void F_'+name+'(CPU *c) { '+helper+'(c); }'+source[m.end():]
        source='#include "x86rt.h"\nextern void '+helper+'(CPU *c);\n'+source
        count+=1
    # Brightness screen commit (sets disp.ini gamma_r/g/b): the PC game writes disp.ini only on a clean exit (F_005f0a80),
    # which never happens on the Vita, so write it right here. Keeps the tail call to F_00402400 then saves.
    m=re.search(r'void F_005ed0f0\(CPU \*c\) \{.*?\n\}',source,re.S)
    if m:
        if hashlib.sha256(m[0].encode()).hexdigest()!='4a02f353910765b8e24bbd0fc0edf1ca734d7d6bef4700110c49b0cc34a86bfc': raise ValueError('Unsupported brightness commit')
        tail=' F_00402400(c); return;\n}'
        assert m[0].endswith(tail)
        body=m[0][:-len(tail)]+' F_00402400(c); { uint32_t a=c->eax,cx=c->ecx,d=c->edx; c->esp-=4; W32(c->esp,0u); F_005f0a80(c); c->eax=a;c->ecx=cx;c->edx=d; } return; /* save disp.ini now */\n}'
        source=source[:m.start()]+'void F_005f0a80(CPU *c);\n'+body+source[m.end():]
    if count!=1: raise ValueError('Expected exactly one reference-page function')
    return source
if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('source',type=Path);p.add_argument('output',type=Path);a=p.parse_args()
    if a.output.resolve().is_relative_to(a.source.resolve().parent): raise SystemExit('Output must be outside generated directory')
    data=replace(a.source.read_text(encoding='utf-8'));a.output.parent.mkdir(parents=True,exist_ok=True);a.output.write_text(data,encoding='utf-8')
