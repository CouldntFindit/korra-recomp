"""Locate vtable entries pointing at a VA in the ORIGINAL exe.

Usage: python tools/vtable_scan.py <LoK.exe> <hexva> [<hexva> ...]

The analysis copy only restores the relocation directory, but this reads the
shipped file so file offsets and VAs come from one section map. Prints every
dword that equals one of the targets, with the surrounding table resolved to
text/other VAs.
"""
import struct
import sys

import pefile


def main() -> int:
    if len(sys.argv) < 3:
        print(__doc__)
        return 2
    pe = pefile.PE(sys.argv[1], fast_load=True)
    pe.parse_data_directories()
    base = pe.OPTIONAL_HEADER.ImageBase
    targets = [int(a, 16) for a in sys.argv[2:]]
    # section VA ranges (file-backed)
    secs = []
    for s in pe.sections:
        va = base + s.VirtualAddress
        secs.append((va, va + s.Misc_VirtualSize, s.PointerToRawData,
                     s.SizeOfRawData, s.Name.rstrip(b"\0").decode("latin1")))

    def read_dwords(va, n):
        for sva, end, raw, size, name in secs:
            if sva <= va < end and va + 4 * n <= end:
                off = raw + (va - sva)
                return struct.unpack_from("<%dI" % n, pe.__data__, off)
        return None

    hits = {}
    for sva, end, raw, size, name in secs:
        data = pe.__data__[raw:raw + size]
        for i in range(0, len(data) - 3, 4):
            v = struct.unpack_from("<I", data, i)[0]
            if v in targets:
                hits.setdefault(v, []).append(sva + i)

    for t in targets:
        print("target %08X: %d reference(s)" % (t, len(hits.get(t, []))))
        for va in hits.get(t, []):
            print("  at %08X" % va)
            start = va - 0x40
            words = read_dwords(start & ~3, 33)
            if not words:
                continue
            for k, w in enumerate(words):
                if w == t:
                    mark = "  <== target (slot %d)" % k
                elif 0x401000 <= w < 0x900000:
                    mark = "  [text]"
                else:
                    mark = ""
                print("    %08X: %08X%s" % ((start & ~3) + 4 * k, w, mark))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
