#!/usr/bin/env python3
"""Tag native blood packet coordinates in isolated, signature-checked units."""
import argparse, hashlib, re
from pathlib import Path
SPECS = {
    '005ba050': ('d36ceb7663d00bb15e3e1133252546cb37f266a2c0dd445bebfe6b7145f09dad', 'L_005ba85e:;'),
    '004bc200': ('7d2a155d496a7be05fee809c99220f127a13e4249e7ea8643cd6585562f3e0be', 'L_004bcacf:;'),
    '005c7a00': ('1d432ec99e384b50966035319f84615d3c5bc2ccb5744187744a7130566fedfc',
                 '  // 0x5c7cdf: mov dword ptr [edx + 0xc], ecx\n  W32((uint32_t)(edx+0xcu),ecx);'),
    '005c8700': ('c58cbb216295e7df318a265bb5d679c4d9a5f9b5f25c17e04641795d30aef7ac',
                 '  // 0x5c89c2: mov dword ptr [eax + 0xc], edx\n  W32((uint32_t)(eax+0xcu),edx);'),
    '005c9510': ('2e4b392f191259e51a216725c01f5b995965e2f3b8307aae5e406afeb6d08155',
                 '  // 0x5c972b: mov dword ptr [edx + 0xc], ecx\n  W32((uint32_t)(edx+0xcu),ecx);'),
}
def generate(source):
    count=0
    for name,(sha,label) in SPECS.items():
        m=re.search(r'void F_'+name+r'\(CPU \*c\) \{.*?\n\}',source,re.S)
        if not m:continue
        if hashlib.sha256(m[0].encode()).hexdigest()!=sha:raise ValueError('Blood function changed: '+name)
        assert m[0].count(label)==1
        # Footprints bias their GS depth by 0xa0 after projection. Register
        # the final XYZ packet, not the earlier unbiased projection result.
        if name in ('005c7a00','005c8700','005c9510'):
            note='diag_world_note_xyz('+('eax' if name=='005c8700' else 'edx')+');'+(' diag_fp_count(edx);' if name=='005c7a00' else '')
        else:note='diag_blood_note_packet(R32(ebx));'
        body=m[0].replace(label,label+'\n  '+note,1)
        if name=='005c7a00':   # footprint depth bias (0xa0): adjustable for the Vita flicker investigation (issue 3), default unchanged
            assert body.count('b=0xa0u')==1
            body=body.replace('b=0xa0u','b=diag_fp_bias()',1)
        source=source[:m.start()]+body+source[m.end():];count+=1
    expected=3 if 'void F_005c7a00(CPU *c)' in source else 1
    if count!=expected:raise ValueError('Expected complete native decal/blood producer set')
    return 'extern void diag_blood_note_packet(unsigned);\nextern void diag_world_note_xyz(unsigned);\nextern void diag_fp_count(unsigned);\nextern unsigned diag_fp_bias(void);\n'+source
if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('source',type=Path);p.add_argument('output',type=Path);a=p.parse_args()
    if a.output.resolve().is_relative_to(a.source.resolve().parent):p.error('Keep output outside generated source tree')
    result=generate(a.source.read_text(encoding='utf-8'));a.output.parent.mkdir(parents=True,exist_ok=True);a.output.write_text(result,encoding='utf-8')
