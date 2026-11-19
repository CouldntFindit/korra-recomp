#!/usr/bin/env python3
"""Report SSE4.2 string-instruction sites (pcmpistri/pcmpestrm) in a PE32.

Read-only. Used by docs/ANDROID_BRINGUP.md to size the arm64 portability risk:
on the Windows x86 host recomp_pcmpistri() executes the real instruction, on
arm64 it falls back to a portable C body, so the imm8 histogram decides which
aggregation/format cases actually matter.

  python tools/scan_pcmpistri.py "The Legend of Korra\\LoK.exe"

imm8 layout (Intel SDM, PCMPESTRI/PCMPISTRI control byte):
  [1:0] data format  00 unsigned bytes, 01 unsigned words,
                     10 signed bytes,   11 signed words
  [3:2] aggregation  00 equal any, 01 ranges, 10 equal each, 11 equal ordered
  [5:4] polarity     00 positive, 01 negative, 10 masked positive, 11 masked negative
  [6]   index        0 least significant, 1 most significant
"""
from __future__ import annotations

import collections
import sys

import capstone
import pefile

FORMATS = {0: "ubyte", 1: "uword", 2: "sbyte", 3: "sword"}
AGGS = {0: "equal-any", 1: "ranges", 2: "equal-each", 3: "equal-ordered"}
POLS = {0: "positive", 1: "negative", 2: "masked-positive", 3: "masked-negative"}

# Byte patterns of the four SSE4.2 string instructions (66 0F 3A 6x).
PATTERNS = [
    (b"\x66\x0f\x3a\x60", "pcmpestrm"),
    (b"\x66\x0f\x3a\x61", "pcmpestri"),
    (b"\x66\x0f\x3a\x62", "pcmpistrm"),
    (b"\x66\x0f\x3a\x63", "pcmpistri"),
]


def text_section(pe: pefile.PE):
    image_base = pe.OPTIONAL_HEADER.ImageBase
    for s in pe.sections:
        if s.Name.rstrip(b"\x00") == b".text":
            return image_base + s.VirtualAddress, s.get_data(), image_base
    raise SystemExit("no .text section")


def main(argv: list[str]) -> int:
    if len(argv) < 2:
        print(__doc__)
        return 2
    pe = pefile.PE(argv[1], fast_load=True)
    base, code, image_base = text_section(pe)
    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
    md.detail = True

    # Linear disassembly desynchronises inside CRT blobs, so scan the raw bytes
    # and decode at each hit: the immediate is the last byte of the decoded
    # instruction (ModRM + SIB + displacement make a fixed offset wrong).
    hits = []
    for pattern, name in PATTERNS:
        start = 0
        while True:
            i = code.find(pattern, start)
            if i < 0:
                break
            start = i + 1
            addr = base + i
            for insn in md.disasm(code[i:i + 12], addr):
                if insn.mnemonic != name:
                    break
                imm = insn.operands[2].imm & 0xFF if len(insn.operands) > 2 else -1
                hits.append((insn.address, insn.mnemonic, imm,
                             " ".join("%02X" % b for b in insn.bytes)))
                break

    hits.sort()
    print("image base 0x%08X, .text %d bytes" % (image_base, len(code)))
    print("%d SSE4.2 string instruction(s)" % len(hits))
    for m, n in sorted(collections.Counter(h[1] for h in hits).items()):
        print("  %-10s %d" % (m, n))

    print("\nsites:")
    print("  address    mnemonic    imm8  fmt   agg            polarity         idx")
    for addr, m, imm, raw in hits:
        fmt, agg, pol, msb = imm & 3, (imm >> 2) & 3, (imm >> 4) & 3, (imm >> 6) & 1
        print("  0x%08X %-11s 0x%02X  %-5s %-14s %-16s %s"
              % (addr, m, imm, FORMATS[fmt], AGGS[agg], POLS[pol],
                 "msb" if msb else "lsb"))

    per_imm = collections.Counter(h[2] for h in hits if h[1] == "pcmpistri")
    print("\nimm8 histogram (pcmpistri):")
    for imm, n in sorted(per_imm.items()):
        fmt, agg, pol, msb = imm & 3, (imm >> 2) & 3, (imm >> 4) & 3, (imm >> 6) & 1
        print("  0x%02X  n=%3d  %s / %s / %s / %s"
              % (imm, n, FORMATS[fmt], AGGS[agg], POLS[pol], "msb" if msb else "lsb"))

    # The portable body in pcrecomp/runtime/recomp32/recomp_types.h implements
    # unsigned bytes only, and hardware flag/valid-bit rules apply to every
    # other format too.
    words = [(a, m, i) for a, m, i, _ in hits if (i & 3) != 0]
    print("\nsites whose imm8 data format is not unsigned bytes: %d" % len(words))
    for addr, m, imm in words:
        print("  0x%08X %s imm8=0x%02X" % (addr, m, imm))

    # Values the x86 fast path in recomp_pcmpistri() covers; anything else uses
    # the portable body even on x86, so no hardware reference exists for it.
    fast = {0x00, 0x04, 0x08, 0x0C, 0x0D, 0x10, 0x14, 0x18, 0x1C, 0x20,
            0x30, 0x38, 0x40, 0x44, 0x48, 0x4C}
    uncovered = [(a, m, i) for a, m, i, _ in hits if m == "pcmpistri" and i not in fast]
    print("\nsites outside the x86 fast-path switch: %d" % len(uncovered))
    for addr, m, imm in uncovered:
        print("  0x%08X %s imm8=0x%02X" % (addr, m, imm))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
