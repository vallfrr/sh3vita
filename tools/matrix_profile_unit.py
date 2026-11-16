#!/usr/bin/env python3
"""Isolated, signature-checked scalarization of SH3 F00648639; generated files stay in build/."""
from pathlib import Path
import argparse
import hashlib
import re

SIGNATURE='16a9dc9bb58f4b5492caf8cf109e8aaca26defbb3a55a1867be621d1e60add78'


def extract(source, name='F_00648639'):
    a=source.index('void '+name+'(CPU *c) {')
    return source[a:source.index('\n}',a)+2]


def optimize(original, fallback='F_00648639'):
    # Exact retained translation, not just a matching address/name.
    expected=SIGNATURE
    if hashlib.sha256(original.rstrip().encode()).hexdigest()!=expected:
        raise ValueError('F00648639 translation changed; re-audit before scalarization')
    body=original
    for outer,inner,outer_end,inner_end in [
        ('0064865f','00648674','006486ab','0064869d'),
        ('006486cb','006486db','00648713','00648702')]:
        start=body.index('L_'+outer+':;')
        middle=body.index('L_'+inner+':;',start)
        stop=body.index('  if (!zf) goto L_'+outer+';',middle)+len('  if (!zf) goto L_'+outer+';')
        inner_stop=body.index('  if (!zf) goto L_'+inner+';',middle)+len('  if (!zf) goto L_'+inner+';')
        pre=body[start+len('L_'+outer+':;'):middle]
        loop=body[middle+len('L_'+inner+':;'):inner_stop]
        loop=loop.replace('  if (!zf) goto L_'+inner+';','')
        post=body[inner_stop:stop].replace('  if (!zf) goto L_'+outer+';','')
        expanded=(pre+loop*4+post)*4
        depth=0
        def scalar(m):
            nonlocal depth
            token=m.group(0)
            if token=='FPUSH(':
                depth+=1
                return 'q%d=(' % ((-depth)%8)
            if token=='FPOP()':
                depth-=1
                return '(void)0'
            return 'q%d' % ((int(m.group(1))-depth)%8)
        expanded=re.sub(r'ST\((\d+)\)|FPUSH\(|FPOP\(\)',scalar,expanded)
        assert depth==16
        body=body[:start]+expanded+body[stop:]
    # Host compilers may choose different NaN payloads when register allocation
    # changes. Retain the original for nonfinite operands and unusual stack/DF
    # overlap; normal finite geometry keeps identical arithmetic and writes.
    guard='''  uint32_t matrix_a=R32(c->esp+8),matrix_b=R32(c->esp+12),matrix_d=R32(c->esp+4);
  if (c->df || (uint32_t)(matrix_a-(c->esp-144u))<224u ||
      (uint32_t)(matrix_b-(c->esp-144u))<224u || (uint32_t)(matrix_d-(c->esp-144u))<224u) {
    FALLBACK(c); return;
  }
  if ((matrix_d!=matrix_a && ((uint32_t)(matrix_d-matrix_a)<64u || (uint32_t)(matrix_a-matrix_d)<64u)) ||
      (matrix_d!=matrix_b && ((uint32_t)(matrix_d-matrix_b)<64u || (uint32_t)(matrix_b-matrix_d)<64u))) {
    FALLBACK(c); return;
  }
  for (unsigned i=0;i<64;i+=4) {
    /* Finite and <= ~1e18: all four-term products then fit in float stores.
       Avoid creating NaNs through overflow on uncommon aliasing paths. */
    if ((R32(matrix_a+i)&0x7fffffffu)>0x5d5e0b6bu || (R32(matrix_b+i)&0x7fffffffu)>0x5d5e0b6bu) {
      FALLBACK(c); return;
    }
  }
'''.replace('FALLBACK',fallback)
    init=guard+'  const uint32_t matrix_ft=c->ftop;\n'+''.join(
        '  double q%d=c->st[(matrix_ft+%d)&7u];\n'%(i,i) for i in range(8))
    body=body.replace('  uint8_t *const mb = g_mem;','  uint8_t *const mb = g_mem;\n'+init,1)
    flush=''.join('c->st[(matrix_ft+%d)&7u]=q%d;'%(i,i) for i in range(8))
    # Only the retained function's final return, never fallback returns.
    pos=body.rindex(' return;');body=body[:pos]+flush+body[pos:]
    assert not re.search(r'\b(ST\(|FPUSH\(|FPOP\()',body)
    return body.replace('void F_00648639(', 'static void matrix_scalar(')


def generate(source):
    original=extract(source)
    retained=original.replace('void F_00648639(', 'static void matrix_retained(')
    candidate=optimize(original,'matrix_retained')
    wrapper='''extern unsigned g_diag_perf;
void F_00648639(CPU *c) {
    if (g_diag_perf & 256u) matrix_scalar(c); else matrix_retained(c);
}'''
    return source.replace(original,retained+'\n'+candidate+'\n'+wrapper,1)


if __name__=='__main__':
    ap=argparse.ArgumentParser(description=__doc__)
    ap.add_argument('source',type=Path);ap.add_argument('output',type=Path)
    args=ap.parse_args()
    # Refuse to overwrite the original translation.
    if args.source.resolve()==args.output.resolve():raise SystemExit('output must be isolated')
    args.output.parent.mkdir(parents=True,exist_ok=True)
    args.output.write_text(generate(args.source.read_text(encoding='utf-8')),encoding='utf-8')
    print(hashlib.sha256(args.output.read_bytes()).hexdigest())
