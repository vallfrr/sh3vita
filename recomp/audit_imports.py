#!/usr/bin/env python3
"""Conservative source-level comparison of SH3 PE imports with sh4vita bridge definitions.

This reports matching symbol names, not ABI or behavioral compatibility. Macro
definitions are detected per source file; aliases and generated definitions may
be missed. No game executable is run.
"""
import argparse
from collections import Counter
from pathlib import Path
import re

from pe import PE


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("exe", type=Path)
    parser.add_argument("sh4_src", type=Path)
    args = parser.parse_args()
    exe = PE(args.exe)
    found = set()
    for path in args.sh4_src.rglob("*.c"):
        text = path.read_text(encoding="utf-8", errors="replace")
        found.update(re.findall(r"\bvoid\s+(imp_[A-Za-z0-9_]+)\s*\(", text))
        for macro, prefix in re.findall(
            r"^\s*#\s*define\s+(\w+)\(name\)\s+void\s+(imp_[A-Za-z0-9_]+)##name\s*\(",
            text, re.MULTILINE,
        ):
            found.update(prefix + name for name in re.findall(
                r"^\s*" + re.escape(macro) + r"\(([A-Za-z_][A-Za-z0-9_]*)\)\s*\{", text, re.MULTILINE
            ))
    imports = exe.imports()
    result = Counter()
    for slot, qualified in sorted(imports.items()):
        dll, name = qualified.split("!", 1)
        if name.startswith("#"):
            name = "ord" + name[1:]
        symbol = "imp_" + dll.split(".")[0] + "_" + re.sub(r"[^A-Za-z0-9_]", "_", name)
        status = "MATCH" if symbol in found else "UNMATCHED"
        result[status] += 1
        print(f"{status:9} {slot:08x} {qualified:52} {symbol}")
    print(f"\nPE image_base={exe.image_base:#x} image_size={exe.image_size:#x} "
          f"end={exe.image_base + exe.image_size:#x} entry={exe.entry:#x}")
    print(f"import slots={len(imports)} source-name matches={result['MATCH']} "
          f"unmatched={result['UNMATCHED']} (name audit only)")


if __name__ == "__main__":
    main()
