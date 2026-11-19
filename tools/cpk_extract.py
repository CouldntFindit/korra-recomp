#!/usr/bin/env python3
"""cpk_extract.py -- extract individual entries from a CRI CPK (CRILAYLA aware).

Deliberately single-entry: the point is to prove the decode path and to eyeball
individual files, not to bulk-extract 1.9 GB of game data.

    python tools/cpk_extract.py <file.cpk> --out <dir> [--index N ...] [--path dir/name]
                               [--smallest N] [--manifest-only]

CRILAYLA layout (verified against the real archives):
    0x00 "CRILAYLA"
    0x08 u32le decompressed_body_size   (total size - 0x100)
    0x0C u32le compressed_size
    0x10 .. 0x10+compressed_size        backwards bitstream
    .. .. 0x100-byte verbatim prefix    (becomes the first 0x100 output bytes)

Body decoding (backwards from the last output byte):
    1 bit marker
      0 -> 8-bit literal
      1 -> 13-bit back-reference offset, then a tiered length:
           len = 2 bits        (3 -> +3 bits; 10 -> +5 bits; 41 -> repeated 8-bit +255)
           len += 3, offset is relative to the current write position +3
"""
from __future__ import annotations

import argparse
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from cpk_manifest import build_manifest, utf_start  # noqa: E402

CRILAYLA_MAGIC = b"CRILAYLA"
PREFIX_SIZE = 0x100


class ReverseBitReader:
    """Reads a bitstream MSB-first, walking the buffer backwards."""

    def __init__(self, buf: bytes):
        self.buf = buf
        self.pos = len(buf)
        self.bits = 0
        self.count = 0

    def read(self, n: int) -> int:
        while self.count < n:
            self.pos -= 1
            if self.pos < 0:
                raise ValueError("CRILAYLA bitstream underrun")
            self.bits = (self.bits << 8) | self.buf[self.pos]
            self.count += 8
        self.count -= n
        value = (self.bits >> self.count) & ((1 << n) - 1)
        # Drop the bits already consumed: without this the accumulator grows
        # without bound and the shifts become quadratic on large streams.
        self.bits &= (1 << self.count) - 1
        return value


def crilayla_decompress(blob: bytes) -> bytes:
    if blob[:8] != CRILAYLA_MAGIC:
        raise ValueError("not a CRILAYLA blob")
    if len(blob) < 0x10 + PREFIX_SIZE:
        raise ValueError("CRILAYLA blob too small")
    body_size, comp_size = struct.unpack_from("<II", blob, 8)
    if 0x10 + comp_size + PREFIX_SIZE > len(blob):
        raise ValueError(f"CRILAYLA truncated: need {0x10 + comp_size + PREFIX_SIZE}, "
                         f"have {len(blob)}")
    payload = blob[0x10:0x10 + comp_size]
    prefix = blob[0x10 + comp_size:0x10 + comp_size + PREFIX_SIZE]

    out = bytearray(body_size + PREFIX_SIZE)
    out[:PREFIX_SIZE] = prefix
    if body_size == 0:
        return bytes(out)

    reader = ReverseBitReader(payload)
    write = len(out) - 1
    while write >= PREFIX_SIZE:
        if reader.read(1) == 0:
            out[write] = reader.read(8)
            write -= 1
            continue
        offset = reader.read(13)
        length = reader.read(2)
        if length == 3:
            length += reader.read(3)
            if length == 10:
                length += reader.read(5)
                if length == 41:
                    while True:
                        extra = reader.read(8)
                        length += extra
                        if extra != 0xFF:
                            break
        length += 3
        read = write + offset + 3
        while length > 0:
            if read < PREFIX_SIZE or read >= len(out):
                raise ValueError(f"back-reference out of range (read={read})")
            out[write] = out[read]
            read -= 1
            write -= 1
            length -= 1
            if write < PREFIX_SIZE:
                break
    return bytes(out)


def extract_entry(data, entry) -> tuple[bytes, bool]:
    blob = data[entry["offset"]:entry["offset"] + entry["size"]]
    if blob[:8] == CRILAYLA_MAGIC:
        return crilayla_decompress(blob), True
    return blob, False


def main(argv=None):
    ap = argparse.ArgumentParser(description="Extract single CPK entries")
    ap.add_argument("cpk")
    ap.add_argument("--out", help="output directory (required unless --manifest-only)")
    ap.add_argument("--index", type=int, action="append", default=[])
    ap.add_argument("--path", action="append", default=[])
    ap.add_argument("--smallest", type=int, default=0,
                    help="extract the N smallest entries")
    ap.add_argument("--manifest-only", action="store_true")
    args = ap.parse_args(argv)

    data, header, hrow, toc, entries, base = build_manifest(args.cpk, 8, None)

    if args.manifest_only:
        for e in entries:
            print(f"0x{e['offset']:08x} {e['size']:>10,} {e['extract_size']:>10,} "
                  f"{e['path']}")
        return 0

    if not args.out:
        sys.exit("--out is required unless --manifest-only")

    picks = []
    for i in args.index:
        picks.append(entries[i])
    for p in args.path:
        want = p.replace("\\", "/")
        hit = [e for e in entries if e["path"] == want]
        if not hit:
            sys.exit(f"no entry named {want!r}")
        picks.extend(hit)
    if args.smallest:
        picks.extend(sorted(entries, key=lambda e: e["size"])[:args.smallest])
    if not picks:
        sys.exit("nothing selected: use --index/--path/--smallest")

    os.makedirs(args.out, exist_ok=True)
    ok = bad = 0
    for e in picks:
        blob, compressed = extract_entry(data, e)
        path = os.path.join(args.out, e["path"].replace("/", os.sep))
        os.makedirs(os.path.dirname(path), exist_ok=True)
        with open(path, "wb") as f:
            f.write(blob)
        good = len(blob) == e["extract_size"]
        ok += good
        bad += not good
        head = blob[:8]
        print(f"{'OK ' if good else 'BAD'} {e['path']:<38} "
              f"{'crilayla' if compressed else 'stored  '} "
              f"{e['size']:>9,} -> {len(blob):>10,} "
              f"(want {e['extract_size']:>10,})  {head!r}")
    print(f"{ok} ok, {bad} size mismatches")
    return 0 if bad == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
