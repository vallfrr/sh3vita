"""Isolate the original clipping translation and a native finite candidate."""
from pathlib import Path
import argparse
import hashlib
from numeric_profile_units import function


def replace(source):
    original=function(source,'F_0048c290')
    if hashlib.sha256(original.encode()).hexdigest()!='c2ebcff3b15ff108b79c017f8fa8ad6b7fba2fbe791c3bc74d51599178e9dc5a':
        raise ValueError('Unexpected SH3 clipping translation')
    retained=original.replace('void F_0048c290(', 'static void clip_retained(')
    wrapper='''
extern int g_diag_native_clip;
void F_0048c290(CPU *c) {
    static int hit_logged;
    if(g_diag_native_clip && native_clip(c)) {
        if(!__atomic_load_n(&hit_logged,__ATOMIC_RELAXED) && !__atomic_exchange_n(&hit_logged,1,__ATOMIC_RELAXED))
            fprintf(stderr,"[native-clip] first finite path executed\\n");
    } else clip_retained(c);
}
'''
    return '#include "native_clip.h"\n#include <stdlib.h>\n#include <stdio.h>\n'+source.replace(original,retained+wrapper,1)


if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('source',type=Path);p.add_argument('output',type=Path);a=p.parse_args()
    if a.output.resolve().is_relative_to(a.source.resolve().parent):raise SystemExit('Isolated copy required')
    result=replace(a.source.read_text(encoding='utf-8'));a.output.parent.mkdir(parents=True,exist_ok=True);a.output.write_text(result,encoding='utf-8')
