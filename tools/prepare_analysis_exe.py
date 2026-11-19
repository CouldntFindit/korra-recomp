#!/usr/bin/env python3
"""Write an ANALYSIS copy of the game executable with a usable base-relocation
directory.

Why this exists: the retail binary keeps its IMAGE_BASE_RELOCATION blocks in a
section but leaves data directory [5] (BASERELOC) zeroed or pointing nowhere.
pcrecomp's PE reader walks relocations through the data directory, so the
analysis copy restores it:

  * find the section whose bytes parse as a chain of IMAGE_BASE_RELOCATION
    blocks (8-byte header + 2-byte HIGHLOW entries, terminators included),
  * point DataDirectory[BASERELOC] at that section and its parsed length,
  * name the section ".reloc" if it is unnamed.

Nothing else is touched: code and data bytes are byte-identical. The source
file is never written; the copy is expected under work/ (gitignored).

Usage:
  python tools/prepare_analysis_exe.py <LoK.exe> [work/LoK.analysis.exe]
"""
from __future__ import annotations

import hashlib
import os
import struct
import sys

import pefile

RELOC_DIR_INDEX = pefile.DIRECTORY_ENTRY["IMAGE_DIRECTORY_ENTRY_BASERELOC"]
IMAGE_SCN_MEM_DISCARDABLE = 0x02000000


def parse_reloc_chain(data: bytes, size_of_image: int) -> int:
    """Return the length of a valid IMAGE_BASE_RELOCATION block chain, or 0."""
    off = 0
    blocks = 0
    while off + 8 <= len(data):
        va, size = struct.unpack_from("<II", data, off)
        if size < 8 or va == 0 or va >= size_of_image:
            break
        if size % 4:
            break
        end = off + size
        if end > len(data):
            break
        # every entry must be HIGHLOW to a VA inside the image, or the
        # IMAGE_REL_BASED_ABSOLUTE (0) padding that aligns blocks to 4 bytes
        for e in range(off + 8, end, 2):
            (entry,) = struct.unpack_from("<H", data, e)
            rtype, rva = entry >> 12, entry & 0xFFF
            if rtype not in (0, 3):
                return 0
            if rtype == 3 and va + rva >= size_of_image:
                return 0
        off = end
        blocks += 1
    return off if blocks else 0


def find_reloc_section(pe: pefile.PE, preferred_range: tuple[int, int] | None = None):
    size_of_image = pe.OPTIONAL_HEADER.SizeOfImage
    best = None
    for sec in pe.sections:
        data = sec.get_data()
        if len(data) < 12:
            continue
        size = parse_reloc_chain(data, size_of_image)
        if not size:
            continue
        score = (size,)
        if preferred_range and preferred_range[0] <= sec.VirtualAddress <= preferred_range[1]:
            score = (1 << 30, size)
        if best is None or score > best[0]:
            best = (score, sec, size)
    return (best[1], best[2]) if best else (None, 0)


def main(argv: list[str]) -> int:
    if len(argv) < 2:
        print(__doc__)
        return 2
    src = argv[1]
    dst = argv[2] if len(argv) > 2 else os.path.join("work", "LoK.analysis.exe")
    os.makedirs(os.path.dirname(os.path.abspath(dst)), exist_ok=True)

    pe = pefile.PE(src)
    dd = pe.OPTIONAL_HEADER.DATA_DIRECTORY[RELOC_DIR_INDEX]

    # Is the directory already usable? Then the copy is a straight byte copy.
    if dd.VirtualAddress and dd.Size:
        for sec in pe.sections:
            if sec.VirtualAddress == dd.VirtualAddress:
                parsed = parse_reloc_chain(sec.get_data()[: dd.Size], pe.OPTIONAL_HEADER.SizeOfImage)
                if parsed:
                    print("reloc directory already valid (VA %#x, size %#x)" % (dd.VirtualAddress, dd.Size))
                    print("copying unchanged -> %s" % dst)
                    with open(src, "rb") as f, open(dst, "wb") as g:
                        g.write(f.read())
                    report(src, dst)
                    return 0
        print("reloc directory present (VA %#x, size %#x) but unparsable" % (dd.VirtualAddress, dd.Size))

    pref = (dd.VirtualAddress - 0x10000, dd.VirtualAddress + 0x10000) if dd.VirtualAddress else None
    sec, size = find_reloc_section(pe, pref)
    if sec is None:
        sys.exit("no section in %s parses as a base-relocation chain" % src)

    old = (dd.VirtualAddress, dd.Size)
    dd.VirtualAddress, dd.Size = sec.VirtualAddress, size
    if not sec.Name.rstrip(b"\0"):
        sec.Name = b".reloc\0\0"

    pe.write(dst)
    print("reloc directory: %#x/%#x -> %#x/%#x (section %r at RVA %#x)"
          % (old[0], old[1], dd.VirtualAddress, dd.Size, sec.Name.rstrip(b"\0").decode("latin-1"),
             sec.VirtualAddress))
    report(src, dst)
    return 0


def report(src: str, dst: str) -> None:
    for label, path in (("src", src), ("dst", dst)):
        with open(path, "rb") as f:
            print("sha256 %s %s" % (label, hashlib.sha256(f.read()).hexdigest()))
    print("wrote %s (%d bytes)" % (dst, os.path.getsize(dst)))


if __name__ == "__main__":
    sys.exit(main(sys.argv))
