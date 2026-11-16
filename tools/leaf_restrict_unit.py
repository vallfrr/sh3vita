"""Isolated no-alias CPU parameter experiment, only for audited pure leaves.

Runtime invariant: CPU lives on the host stack/in a host thread structure;
guest addresses refer exclusively to the separate g_mem allocation. Exclude
guest dispatch, external CPU callbacks and any global except g_mem. This is
not a change to floating arithmetic, spills, the guest ABI or game data.
"""
import argparse
import hashlib
import json
import re
from pathlib import Path

FUNCTION = re.compile(r'void F_([0-9a-f]{8})\(CPU \*c\) \{.*?\n\}', re.S)
SAFE = set('if else for while switch sizeof R8 R16 R32 R64 W8 W16 W32 W64 '
           'PF ST FPUSH FPOP rt_f32 rt_u32f rt_f64 rt_u64f rt_pf rt_fxam '
           'rt_fcom rt_fnstsw rt_fnstsw_t rt_ftol '
           'fabs sqrt sin cos tan atan atan2 asin acos exp log log10 pow '
           'floor ceil fmod ldexp memcpy memmove memset'.split())


def transform(source, include_callers=False):
    changed = []
    rejected = {}
    def visit(match):
        original = match[0]
        body = original[original.index('{')+1:]
        body = re.sub(r'/\*.*?\*/|//[^\n]*', '', body, flags=re.S)
        calls = set(re.findall(r'\b([A-Za-z_]\w*)\s*\(', body))
        globals_ = set(re.findall(r'\bg_\w+', body)) - {'g_mem'}
        unknown = sorted((calls - SAFE) | globals_)
        if include_callers:
            # Separate experiment after auditing the runtime: guest calls and
            # host imports forward the same c argument, not an independently
            # dereferenced global CPU alias. Preserve the complete bodies.
            unknown = sorted(globals_)
        if '#' in body or re.search(r'\bHOSTFN\b', body):
            unknown.append('non-generated macro context')
        if unknown:
            rejected[match[1]] = unknown
            return original
        changed.append({'address': match[1], 'sha256': hashlib.sha256(original.encode()).hexdigest()})
        return original.replace('CPU *c', 'CPU *__restrict c', 1)
    result = FUNCTION.sub(visit, source)
    return result, {'scope': 'cpu_arena_contract' if include_callers else 'pure_leaves',
                    'source_sha256': hashlib.sha256(source.encode()).hexdigest(),
                    'qualified_leaves': changed, 'excluded': rejected}


if __name__ == '__main__':
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('source',type=Path);p.add_argument('output',type=Path)
    p.add_argument('--include-callers',action='store_true',help='Separate lab: use audited host CPU/guest arena invariant also through forwarded CPU calls')
    a=p.parse_args()
    if a.output.resolve().is_relative_to(a.source.resolve().parent):raise SystemExit('Isolated copy required')
    text,manifest=transform(a.source.read_text(encoding='utf-8'),a.include_callers)
    a.output.parent.mkdir(parents=True,exist_ok=True)
    if not a.output.exists() or a.output.read_text(encoding='utf-8')!=text:
        a.output.write_text(text,encoding='utf-8')
    a.output.with_suffix('.leaf.json').write_text(json.dumps(manifest,indent=2),encoding='utf-8')
