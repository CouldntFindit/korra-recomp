#!/usr/bin/env python3
"""Tiny offset-annotated hex dump: hexpeek.py <file> [offset] [length]"""
import sys

path = sys.argv[1]
off = int(sys.argv[2], 0) if len(sys.argv) > 2 else 0
n = int(sys.argv[3], 0) if len(sys.argv) > 3 else 256

with open(path, "rb") as f:
    f.seek(off)
    data = f.read(n)

for i in range(0, len(data), 16):
    chunk = data[i:i + 16]
    hexs = " ".join(f"{b:02x}" for b in chunk)
    asc = "".join(chr(b) if 32 <= b < 127 else "." for b in chunk)
    print(f"{off + i:08x}  {hexs:<47}  {asc}")
