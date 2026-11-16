#!/usr/bin/env python3
"""Inline two exact retained CRT leaves into an isolated floor unit."""
from pathlib import Path
import argparse
import hashlib

EXPECTED = {
    'F_006634a0': '90aa6e62e3a0f82e9f1512da8f71ea2a64c6486a75a8be85875ba4172dd7882b',
    'F_00670c35': 'efba93236ed207267a17de7f5c3687cf4f3b33fd9408c41e3dcb9cda8eff858b',
    'F_006741ad': '02b699a2175505477a5085ab8b88ffb04f886d35ae0a93d9707b690ba69b38ff',
}

def function(source, name):
    a = source.index('void ' + name + '(CPU *c) {')
    result = source[a:source.index('\n}', a) + 2]
    if hashlib.sha256(result.encode()).hexdigest() != EXPECTED[name]:
        raise ValueError('Retained translation changed: ' + name)
    return result

def transform(source, control, rounding):
    old = function(source, 'F_006634a0')
    leaves = []
    for text, name, new in [(control, 'F_00670c35', 'floor_control_inline'),
                             (rounding, 'F_006741ad', 'floor_round_inline')]:
        body = function(text, name).replace('void ' + name + '(',
            'static __attribute__((always_inline)) inline void ' + new + '(')
        leaves.append(body.replace('double ;', ''))
    retained = old.replace('void F_006634a0(', 'static void floor_retained(')
    candidate = old.replace('void F_006634a0(', 'static void floor_inlined(')
    candidate = candidate.replace('F_00670c35(c)', 'floor_control_inline(c)').replace('F_006741ad(c)', 'floor_round_inline(c)')
    wrapper = '\nextern unsigned g_diag_perf;\nvoid F_006634a0(CPU *c) { if (g_diag_perf & 524288u) floor_inlined(c); else floor_retained(c); }'
    return source.replace(old, '\n'.join(leaves) + '\n' + retained + '\n' + candidate + wrapper, 1)

if __name__ == '__main__':
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('generated', type=Path)
    ap.add_argument('output', type=Path)
    a = ap.parse_args()
    if a.output.resolve().is_relative_to(a.generated.resolve()):
        raise SystemExit('Output must stay outside the generated source directory')
    result = transform((a.generated/'x86_f180.c').read_text(),
                       (a.generated/'x86_f186.c').read_text(),
                       (a.generated/'x86_f188.c').read_text())
    a.output.parent.mkdir(parents=True, exist_ok=True)
    a.output.write_text(result)
