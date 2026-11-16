#!/usr/bin/env python3
"""Keep the original archive decoder under a private name, in an isolated unit."""
import argparse
import hashlib
from pathlib import Path

SIGNATURE = '935b60c8a867fa21b1f1142eab8f218d540aacc90c1e204af953412aac5f0494'


def replace(source):
    start = source.index('void F_00608f80(CPU *c) {')
    stop = source.index('\n}', start) + 2
    body = source[start:stop]
    if hashlib.sha256(body.encode()).hexdigest() != SIGNATURE:
        raise ValueError('Archive reader changed; re-audit before enabling bounded reads.')
    return source[:start] + body.replace('void F_00608f80(', 'void diag_archive_read_retained(', 1) + source[stop:]


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('source', type=Path)
    parser.add_argument('output', type=Path)
    args = parser.parse_args()
    if args.output.resolve().is_relative_to(args.source.resolve().parent):
        raise SystemExit('Isolated output outside generated directory required.')
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(replace(args.source.read_text()))
