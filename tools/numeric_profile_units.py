#!/usr/bin/env python3
"""Build isolated generated units for numeric experiments; never overwrite port/gen."""
from pathlib import Path
import argparse
import hashlib


def function(source,name):
    a=source.index('void '+name+'(CPU *c) {')
    return source[a:source.index('\n}',a)+2]


def ftol_unit(source):
    old=function(source,'F_00662c40')
    if hashlib.sha256(old.rstrip().encode()).hexdigest()!='b6d022665ec04917a32f1a95b61716c89b9f0d59b4f21a5ca3c1d08d100c15b7':
        raise ValueError('FTOL retained translation signature changed')
    return source.replace(old,old.replace('void F_00662c40(', 'void diag_ftol_retained('),1)


def geometry_unit(source,name):
    expected={'F_0048c290':'c2ebcff3b15ff108b79c017f8fa8ad6b7fba2fbe791c3bc74d51599178e9dc5a',
              'F_0048e140':'8fd2eff8fb78a162b27bfddca73c07e92cc9ddf41b5a0b417375951a007c1f6a'}
    old=function(source,name)
    if hashlib.sha256(old.rstrip().encode()).hexdigest()!=expected[name]:raise ValueError('geometry translation changed')
    retained=old.replace('void '+name+'(', 'static __attribute__((noinline)) void '+name+'_retained(')
    candidate=old.replace('void '+name+'(', 'static __attribute__((noinline,optimize("O2"))) void '+name+'_o2(')
    wrapper='\nextern unsigned g_diag_perf;\nvoid '+name+'(CPU *c) { if(g_diag_perf&2048u) '+name+'_o2(c); else '+name+'_retained(c); }'
    return source.replace(old,retained+'\n'+candidate+wrapper,1)


if __name__=='__main__':
    ap=argparse.ArgumentParser(description=__doc__)
    ap.add_argument('kind',choices=['ftol','F_0048c290','F_0048e140']);ap.add_argument('source',type=Path);ap.add_argument('output',type=Path)
    args=ap.parse_args()
    if args.source.resolve()==args.output.resolve():raise SystemExit('isolated output required')
    args.output.parent.mkdir(parents=True,exist_ok=True)
    source=args.source.read_text(encoding='utf-8')
    args.output.write_text(ftol_unit(source) if args.kind=='ftol' else geometry_unit(source,args.kind),encoding='utf-8')
