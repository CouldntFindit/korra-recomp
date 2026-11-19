#!/usr/bin/env python3
"""cpk_manifest.py -- CRI CPK inventory: parse header + TOC, emit a file manifest.

Reads only the container headers/table-of-contents and (optionally) the first
bytes of each entry to detect CRILAYLA compression. It never extracts payloads.

    python tools/cpk_manifest.py <file.cpk> [--json out.json] [--markdown out.md]
                                 [--sample-magic N] [--limit N]

Format notes (verified against the real archives, see docs/assets.md):
  * Everything is big-endian.
  * CPK: 0x00 "CPK ", 0x08 u32 table_size, UTF table at 0x10.
  * UTF table (0x20-byte header): magic, table_size(+8), u16 version,
    u16 rows_offset(+8), u32 strings_offset(+8), u32 data_offset(+8),
    u32 table_name_offset, u16 column_count, u16 row_width, u32 row_count.
    All offsets are relative to table_start + 8.
  * Column entry = u8 info (high nibble flags: 0x10 Name / 0x20 Default /
    0x40 Row, low nibble type), u32 name offset (when Name), then an optional
    default value (when Default). Columns with the Row flag are packed in the
    rows, in declaration order.
"""
from __future__ import annotations

import argparse
import json
import os
import struct
import sys
from collections import Counter, OrderedDict

UTF_MAGIC = b"@UTF"
CRILAYLA_MAGIC = b"CRILAYLA"

# type id -> (name, struct format, size)
TYPES = {
    0x00: ("u8", ">B", 1),
    0x01: ("s8", ">b", 1),
    0x02: ("u16", ">H", 2),
    0x03: ("s16", ">h", 2),
    0x04: ("u32", ">I", 4),
    0x05: ("s32", ">i", 4),
    0x06: ("u64", ">Q", 8),
    0x07: ("s64", ">q", 8),
    0x08: ("f32", ">f", 4),
    0x09: ("f64", ">d", 8),
    0x0A: ("str", ">I", 4),   # offset into the string table
    0x0B: ("vldata", ">QQ", 16),  # CRI VLData: offset + size, both u64 here
    0x0C: ("guid", None, 16),
}
FLAG_NAME, FLAG_DEFAULT, FLAG_ROW = 0x10, 0x20, 0x40


class UtfTable:
    """A parsed CRI UTF table."""

    def __init__(self, buf: bytes, start: int):
        if buf[start:start + 4] != UTF_MAGIC:
            raise ValueError(f"no @UTF at 0x{start:x} (found {buf[start:start+4]!r})")
        (self.table_size, self.version, self.rows_offset, self.strings_offset,
         self.data_offset, self.name_offset, self.column_count, self.row_width,
         self.row_count) = struct.unpack_from(">IHHIIIHHI", buf, start + 4)
        self.table_size += 8
        self.rows_offset += 8
        self.strings_offset += 8
        self.data_offset += 8
        self.start = start
        self._buf = buf
        strings_end = self.data_offset if self.data_offset > self.strings_offset else self.table_size
        self._strings = buf[start + self.strings_offset: start + strings_end]
        self.name = self.string(self.name_offset)
        self.columns = []  # list of dicts: name, type, flags, row_offset
        self._parse_schema(buf, start)

    def string(self, offset: int) -> str:
        end = self._strings.find(b"\0", offset)
        if end < 0:
            end = len(self._strings)
        return self._strings[offset:end].decode("utf-8", "replace")

    def _parse_schema(self, buf: bytes, start: int):
        pos = start + 0x20
        row_off = 0
        for _ in range(self.column_count):
            info = buf[pos]
            flags, type_id = info & 0xF0, info & 0x0F
            name_offset = struct.unpack_from(">I", buf, pos + 1)[0]
            pos += 5
            type_name, fmt, size = TYPES[type_id]
            col = {"name": self.string(name_offset), "type": type_name,
                   "flags": flags, "row_offset": None, "default": None}
            if flags & FLAG_DEFAULT:
                col["default"] = self._decode(buf[pos:pos + size], type_id)
                pos += size
            if flags & FLAG_ROW:
                col["row_offset"] = row_off
                row_off += size
            self.columns.append(col)
        if row_off != self.row_width:
            raise ValueError(
                f"row width mismatch at 0x{start:x}: schema says {row_off}, header says {self.row_width}")

    def _decode(self, raw: bytes, type_id: int):
        type_name, fmt, size = TYPES[type_id]
        if type_id == 0x0A:
            return self.string(struct.unpack(">I", raw)[0])
        if type_id == 0x0B:
            off, size_ = struct.unpack(">QQ", raw)
            return {"data_offset": off, "size": size_}
        if type_id == 0x0C:
            return raw.hex()
        return struct.unpack(fmt, raw)[0]

    def rows(self):
        base = self.start + self.rows_offset
        for r in range(self.row_count):
            row = {}
            for col in self.columns:
                if col["row_offset"] is None:
                    row[col["name"]] = col["default"]
                    continue
                off = base + r * self.row_width + col["row_offset"]
                type_id = [k for k, v in TYPES.items() if v[0] == col["type"]][0]
                _, _, size = TYPES[type_id]
                row[col["name"]] = self._decode(self._buf[off:off + size], type_id)
            yield row

    def schema_report(self) -> str:
        rows = []
        for c in self.columns:
            in_row = "-" if c["row_offset"] is None else f"@ +{c['row_offset']:#04x}"
            rows.append(f"  {c['name']:<20} {c['type']:<7} {in_row:<9} "
                        f"{'' if c['default'] is None else repr(c['default'])}")
        return (f"table {self.name!r}: {self.column_count} columns, "
                f"{self.row_count} rows, row_width {self.row_width}, "
                f"version {self.version}\n" + "\n".join(rows))


def open_cpk(path: str):
    with open(path, "rb") as f:
        data = f.read()
    if data[:4] != b"CPK ":
        raise ValueError(f"{path}: not a CPK (magic {data[:4]!r})")
    header = UtfTable(data, 0x10)
    return data, header


def build_manifest(path: str, sample_magic: int, limit: int | None):
    data, header = open_cpk(path)
    hrow = next(header.rows())
    if hrow.get("CpkMode"):
        raise ValueError(f"CpkMode={hrow['CpkMode']} (UTF/TOC encryption) not handled")
    toc_offset = hrow["TocOffset"]
    content_offset = hrow["ContentOffset"]
    toc = UtfTable(data, toc_offset)

    entries = []
    for r in toc.rows():
        dirname = r.get("DirName") or ""
        fname = r.get("FileName") or ""
        path_ = (dirname.rstrip("/\\") + "/" + fname) if dirname else fname
        path_ = path_.replace("\\", "/")
        offset = r.get("FileOffset") or 0
        fsize = r.get("FileSize") or 0
        xsize = r.get("ExtractSize") or fsize
        magic = ""
        if sample_magic and offset:
            magic = data[offset:offset + 8]
            magic = "".join(chr(b) if 32 <= b < 127 else "." for b in magic)
        entries.append({
            "path": path_,
            "ext": os.path.splitext(fname)[1].lower().lstrip(".") or "(none)",
            "id": r.get("ID"),
            "offset": offset,
            "size": fsize,
            "extract_size": xsize,
            "crilayla": magic.startswith("CRILAYLA") if magic else (xsize > fsize),
            "magic": magic,
            "crc": r.get("CRC"),
            "user_string": r.get("UserString") or "",
        })
        if limit and len(entries) >= limit:
            break

    return data, header, hrow, toc, entries


def summarise(path: str, hrow, entries) -> dict:
    ext_count = Counter(e["ext"] for e in entries)
    ext_bytes = Counter()
    for e in entries:
        ext_bytes[e["ext"]] += e["extract_size"]
    dirs = Counter(e["path"].split("/")[0] if "/" in e["path"] else "(root)" for e in entries)
    compressed = [e for e in entries if e["crilayla"]]
    return {
        "cpk": os.path.basename(path),
        "bytes": os.path.getsize(path),
        "codec": hrow.get("Codec") or "none",
        "cpk_mode": hrow.get("CpkMode"),
        "version": hrow.get("Version"),
        "revision": hrow.get("Revision"),
        "cpk_tools": hrow.get("Comment"),
        "align": hrow.get("Align"),
        "total_files": hrow.get("TotalFiles"),
        "content_offset": hrow.get("ContentOffset"),
        "content_size": hrow.get("ContentSize"),
        "toc_offset": hrow.get("TocOffset"),
        "toc_size": hrow.get("TocSize"),
        "entries": len(entries),
        "crilayla_entries": len(compressed),
        "crilayla_bytes": sum(e["size"] for e in compressed),
        "stored_bytes": sum(e["size"] for e in entries),
        "extract_bytes": sum(e["extract_size"] for e in entries),
        "by_ext": [{"ext": k, "count": v, "bytes": ext_bytes[k]}
                   for k, v in ext_count.most_common()],
        "by_dir": [{"dir": k, "count": v} for k, v in dirs.most_common()],
    }


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("cpk")
    ap.add_argument("--json")
    ap.add_argument("--markdown")
    ap.add_argument("--sample-magic", type=int, default=8,
                    help="bytes of each entry to read for magic sniffing (0 = off)")
    ap.add_argument("--limit", type=int, help="stop after N entries (sanity runs)")
    ap.add_argument("--schema", action="store_true", help="dump both UTF schemas")
    args = ap.parse_args(argv)

    data, header, hrow, toc, entries = build_manifest(args.cpk, args.sample_magic, args.limit)
    info = summarise(args.cpk, hrow, entries)
    info["entries_detail"] = entries

    if args.schema:
        print("== CPK header table ==")
        print(header.schema_report())
        print("== TOC table ==")
        print(toc.schema_report())

    print(f"{info['cpk']}: {info['bytes']:,} bytes, codec={info['codec']}, "
          f"version={info['version']} revision={info['revision']}, "
          f"mode={info['cpk_mode']}, tools={info['cpk_tools']!r}")
    print(f"  entries={info['entries']} stored={info['stored_bytes']:,} "
          f"extract={info['extract_bytes']:,} crilayla={info['crilayla_entries']} "
          f"({info['crilayla_bytes']:,} stored bytes)")
    print("  extensions:")
    for e in info["by_ext"][:40]:
        print(f"    .{e['ext']:<10} {e['count']:>7}  {e['bytes']:>14,}")
    print("  top dirs:")
    for d in info["by_dir"][:20]:
        print(f"    {d['dir']:<24} {d['count']:>7}")

    if args.json:
        os.makedirs(os.path.dirname(os.path.abspath(args.json)), exist_ok=True)
        with open(args.json, "w", encoding="utf-8") as f:
            json.dump(info, f, indent=1)
        print(f"  -> {args.json}")
    if args.markdown:
        os.makedirs(os.path.dirname(os.path.abspath(args.markdown)), exist_ok=True)
        with open(args.markdown, "w", encoding="utf-8") as f:
            f.write(f"# {info['cpk']} manifest summary\n\n")
            f.write(f"* {info['entries']} entries, {info['stored_bytes']:,} stored bytes, "
                    f"{info['extract_bytes']:,} after extraction\n")
            f.write(f"* codec: {info['codec']} · CPK {info['version']}.{info['revision']} · "
                    f"{info['cpk_tools']}\n\n")
            f.write("| ext | count | bytes (extracted) |\n|---|---:|---:|\n")
            for e in info["by_ext"]:
                f.write(f"| `.{e['ext']}` | {e['count']} | {e['bytes']:,} |\n")
        print(f"  -> {args.markdown}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
