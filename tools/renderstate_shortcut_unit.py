"""Avoid CPU save/reload around the verified native SetRenderState only.

Preserve the target load, guest return-address write, argument bytes, native
prep_set and fallback call literally. Only EAX/ESP differ after this COM
method. Every other target and unsupported caller retains the original path.
"""
import argparse,hashlib,json,re
from pathlib import Path

CALL = re.compile(r'(?P<head>\{ uint32_t t=R32\([^\n]+?\); esp-=4; W32\(esp,0x[0-9a-f]+u\); )(?P<call>c->eax=eax;[^\n]+?rt_call\(c,t\);[^\n]+?)(?P<tail> \})')

def transform(source):
    sites=[]
    def visit(m):
        # Restrict this experiment to explicit offset200 device call sites.
        if '+0xc8u' not in m['head']:return m[0]
        sites.append({'sha256':hashlib.sha256(m[0].encode()).hexdigest(),'ret':re.search(r'W32\(esp,(0x[0-9a-f]+u)\)',m['head'])[1]})
        return (m['head']+'if (t && t==g_diag_renderstate_target && '
                'diag_renderstate_shortcut(c,t,R32(esp+4u),R32(esp+8u),R32(esp+12u))) '
                '{ eax=0;esp+=16u; } else { '+m['call']+' }'+m['tail'])
    result=CALL.sub(visit,source)
    if sites:
        result=('extern unsigned g_diag_renderstate_target;\n'
                'extern int diag_renderstate_shortcut(CPU *,unsigned,unsigned,unsigned,unsigned);\n')+result
        # CPU is declared by the original include: declarations must follow it.
        prefix,rest=result.split('#include "x86rt.h"',1)
        result='#include "x86rt.h"\n'+prefix+rest
    return result,{'source_sha256':hashlib.sha256(source.encode()).hexdigest(),'sites':sites}

if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('source',type=Path);p.add_argument('output',type=Path);a=p.parse_args()
    if a.output.resolve().is_relative_to(a.source.resolve().parent):raise SystemExit('Isolated copy required')
    source,manifest=transform(a.source.read_text());a.output.parent.mkdir(parents=True,exist_ok=True)
    a.output.write_text(source);a.output.with_suffix('.shortcut.json').write_text(json.dumps(manifest,indent=2))
