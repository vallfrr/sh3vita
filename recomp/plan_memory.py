#!/usr/bin/env python3
"""Propose a non-overlapping contiguous guest layout for an x86 PE32 image.

This is a static budget check, not a runtime allocator or a measurement of
the game's peak RAM use. Address zero remains reserved; the stack grows down.
"""
import argparse
from pathlib import Path
from pe import PE

MIB = 1024 * 1024


def plan(image_base, image_size, memory_size, stack_top=0x300000, stack_size=MIB, align=0x10000):
    if image_base <= 0 or image_size <= 0 or memory_size <= 0 or align <= 0 or align & (align - 1):
        raise ValueError("invalid image, memory or alignment")
    end = image_base + image_size
    if end > 0x100000000 or memory_size > 0x100000000:
        raise ValueError("guest address space overflow")
    if stack_size <= 0 or stack_top <= stack_size or stack_top > memory_size:
        raise ValueError("invalid stack range")
    stack_start = stack_top - stack_size
    if stack_start < end and image_base < stack_top:
        raise ValueError("guest stack overlaps image")
    heap_start = (max(end, stack_top) + align - 1) & -align
    if heap_start >= memory_size:
        raise ValueError("no guest heap remains")
    return stack_start, stack_top, heap_start, memory_size


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("exe", type=Path)
    ap.add_argument("--ram-mib", type=int, default=200)
    args = ap.parse_args()
    pe = PE(args.exe)
    stack_lo, stack_hi, heap_lo, heap_hi = plan(pe.image_base, pe.image_size, args.ram_mib * MIB)
    print(f"image {pe.image_base:#010x}..{pe.image_base + pe.image_size:#010x} "
          f"({pe.image_size / MIB:.2f} MiB)")
    print(f"stack {stack_lo:#010x}..{stack_hi:#010x}")
    print(f"heap  {heap_lo:#010x}..{heap_hi:#010x} ({(heap_hi - heap_lo) / MIB:.2f} MiB maximum)")
    print("Static budget only: allocations, graphics memory and actual Vita behavior not measured.")


if __name__ == "__main__":
    main()
