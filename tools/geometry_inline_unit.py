"""Isolated inlining of the existing clip/Vec3 call chain inside F0048e140.

No arithmetic or guest stack operation is rewritten. Private copies retain
the existing lazy D3DX dispatcher and its uncommon targets. The audited
host CPU / guest arena separation is required at the parent entry point.
"""
import argparse
import hashlib
import json
import re
from pathlib import Path

SPECS = [
    ('171', '006483a7', 'bfe7c418f2aa0f793c36bd31da9ca8355e043975f358a461a84568284b9a83ba'),
    ('157', '0062a867', 'f886726c44c58458438bb906e7e3e8b25f95d05378bf8ead602bab109f27f66f'),
    ('thunks', '0062a874', '17eea7b433f820eca8276eb682410ea1f3eb67cb0041e9692f4ab1286b30cfa8'),
    ('045', '0048c290', 'c2ebcff3b15ff108b79c017f8fa8ad6b7fba2fbe791c3bc74d51599178e9dc5a'),
    ('046', '0048e140', '8fd2eff8fb78a162b27bfddca73c07e92cc9ddf41b5a0b417375951a007c1f6a'),
]


def extract(source, address):
    pattern = r'void F_' + address + r'\(CPU \*c\) \{'
    start = re.search(pattern, source)
    if not start:
        raise ValueError('Missing function ' + address)
    # These signature-locked generated bodies contain balanced C braces.
    depth = 1
    pos = start.end()
    while depth:
        depth += (source[pos] == '{') - (source[pos] == '}')
        pos += 1
    return source[start.start():pos]


def generate(directory):
    originals = {}
    for unit, address, expected in SPECS:
        body = extract((directory / ('x86_' + ('thunks' if unit == 'thunks' else 'f'+unit) + '.c')).read_text(), address)
        actual = hashlib.sha256(body.encode()).hexdigest()
        if actual != expected:
            raise ValueError(address + ': retained source changed; re-audit')
        originals[address] = body
    private = {a: 'geom_inline_' + a for a in originals if a != '0048e140'}
    def rename(body):
        for address, name in private.items():
            body = re.sub(r'\bF_' + address + r'\b', name, body)
        return body
    helpers = []
    for address in private:
        body = rename(originals[address])
        body = body.replace('void '+private[address]+'(', 'static inline __attribute__((always_inline)) void '+private[address]+'(', 1)
        # Reverse the mechanical transformation: every statement must survive.
        reverse = body.replace('static inline __attribute__((always_inline)) void ', 'void ', 1)
        for a, name in private.items():
            reverse = re.sub(r'\b'+name+r'\b', 'F_'+a, reverse)
        assert reverse == originals[address]
        helpers.append(body)
    parent = rename(originals['0048e140']).replace('CPU *c', 'CPU *__restrict c', 1)
    source = (directory/'x86_f046.c').read_text()
    source = source.replace(originals['0048e140'], '\n'.join(helpers)+'\n'+parent, 1)
    return source, {a: hashlib.sha256(b.encode()).hexdigest() for a,b in originals.items()}


if __name__ == '__main__':
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('directory', type=Path);p.add_argument('output', type=Path)
    a = p.parse_args()
    if a.output.resolve().is_relative_to(a.directory.resolve()):
        raise SystemExit('Isolated output required')
    source, signatures = generate(a.directory)
    a.output.parent.mkdir(parents=True, exist_ok=True)
    a.output.write_text(source)
    a.output.with_suffix('.inline.json').write_text(json.dumps(signatures, indent=2))
