#!/usr/bin/env python3
"""Inventory The Legend of Korra's Wwise audio without any third-party packages.

Everything here is read-only over the *user's own* game data; nothing is written
except the report the caller asks for.  See docs/audio.md for the findings this
tool was written to produce.

Commands
--------
  scan   <sound_dir>            inventory every .wem/.bnk/.wai under a folder
  wem    <file.wem>...          one-line summary per .wem
  bank   <file.bnk>...          RIFF-style chunk table, DIDX chain check, HIRC census
  wai    <WwiseInfo.wai>        dump the WwiseInfo event table (CSV or JSON)
  names  <LoK.exe> --ids FILE   recover Wwise object names from string literals
  prefix <bank.bnk> --stream DIR
                                verify that bank media are byte-prefixes (prefetch)

Formats documented here were verified against the shipped 2014 retail data
(Wwise bank generator version 0x58 = Wwise 2013.1/2).  Wwise object IDs in this
title are FNV-1 (32-bit) of the *lowercase* object name, e.g. fnv1("bgm") ==
412724365 == BGM.bnk's bank ID.
"""
from __future__ import annotations

import argparse
import json
import os
import re
import struct
import sys

# --- Wwise HIRC object types (bank generator version 0x58) -------------------
HIRC_TYPES = {
    0: "State",
    1: "State",
    2: "Sound",
    3: "Action(any subtype)",
    4: "Event",
    5: "RandomSequenceContainer",
    6: "SwitchContainer",
    7: "ActorMixer",
    8: "Bus",
    9: "LayerContainer",
    10: "MusicSegment",
    11: "MusicTrack",
    12: "MusicSwitchContainer",
    13: "MusicRandomSequenceContainer",
    14: "Attenuation",
    18: "FxShareSet",
    19: "FxCustom",
    20: "AuxBus",
}

# Wwise .wem fmt tags seen in this title.  0xFFFF is *not* "extensible": Wwise
# reuses it as its own codec-tagged variant, the real codec is in the extension.
FMT_TAGS = {
    0x0001: "PCM",
    0x0002: "Wwise IMA ADPCM",
    0xFFFE: "PCM (extensible header)",
    0xFFFF: "Wwise Vorbis",
}


def fnv1(name: str) -> int:
    """Wwise AK::SoundEngine::GetIDFromString: FNV-1 32 over lowercase bytes."""
    h = 0x811C9DC5
    for c in name.lower().encode("latin-1", "ignore"):
        h = ((h * 0x01000193) & 0xFFFFFFFF) ^ c
    return h


# --- .wem -------------------------------------------------------------------
def riff_chunks(data: bytes):
    """Yield (id, offset_of_payload, size).  Wwise .wem is plain RIFF/RIFX-free."""
    off = 12
    n = len(data)
    while off + 8 <= n:
        cid = data[off:off + 4].decode("latin-1")
        size = struct.unpack_from("<I", data, off + 4)[0]
        yield cid, off + 8, size
        off += 8 + size + (size & 1)


def parse_wem(path: str) -> dict:
    with open(path, "rb") as f:
        data = f.read()
    info = {
        "path": path,
        "size": len(data),
        "chunks": [],
        "fmt_tag": None,
        "codec": None,
        "channels": None,
        "rate": None,
        "samples": None,
        "duration": None,
        "data_size": None,
        "ripple": None,       # RIFF size vs file size
    }
    if data[:4] != b"RIFF":
        info["codec"] = "not RIFF"
        return info
    info["ripple"] = struct.unpack_from("<I", data, 4)[0] + 8 == len(data)
    fmt_off = fmt_size = None
    for cid, off, size in riff_chunks(data):
        info["chunks"].append(cid)
        if cid == "fmt ":
            fmt_off = off
            fmt_size = size
        elif cid == "data":
            info["data_size"] = size
    if fmt_off is not None:
        tag, ch, rate, _avg, align, _bits = struct.unpack_from("<HHIIHH", data, fmt_off)
        info["fmt_tag"] = tag
        info["codec"] = FMT_TAGS.get(tag, "tag 0x%04X" % tag)
        info["channels"] = ch
        info["rate"] = rate
        # Wwise codec extension: u16 cbSize at fmt+0x10, then codec params.
        # For Vorbis the total sample count sits at fmt+0x18 (verified against
        # vgmstream on all 779 shipped files).  IMA ADPCM stores its *block*
        # count there instead, so derive frames from the block geometry.
        if tag == 0x0002 and align and info["data_size"]:
            # 4-bit IMA: every block carries a 4-byte header per channel.
            per_block = (align - 4 * ch) * 2 // max(ch, 1)
            info["samples"] = (info["data_size"] // align) * per_block
            info["blocks"] = info["data_size"] // align
        elif fmt_size >= 0x1C:
            ext = struct.unpack_from("<I", data, fmt_off + 0x18)[0]
            if tag in (0xFFFF, 0xFFFE, 0x0001):
                info["samples"] = ext
        if info["samples"] is None and info["data_size"] and align:
            info["samples"] = info["data_size"] // align
        if info["samples"] and rate:
            info["duration"] = info["samples"] / float(rate)
    return info


# --- .bnk -------------------------------------------------------------------
def parse_bnk(path: str) -> dict:
    with open(path, "rb") as f:
        data = f.read()
    out = {"path": path, "size": len(data), "chunks": {}, "order": []}
    off = 0
    while off + 8 <= len(data):
        cid = data[off:off + 4].decode("latin-1")
        size = struct.unpack_from("<I", data, off + 4)[0]
        out["chunks"][cid] = {"offset": off + 8, "size": size}
        out["order"].append(cid)
        off += 8 + size
    out["trailing"] = len(data) - off

    b = out["chunks"].get("BKHD")
    if b:
        p = b["offset"]
        ver, bank_id = struct.unpack_from("<II", data, p)
        out["bank_version"] = ver
        out["bank_id"] = bank_id
        out["language_id"] = struct.unpack_from("<I", data, p + 8)[0]

    d = out["chunks"].get("DIDX")
    if d:
        entries = []
        for i in range(d["size"] // 12):
            mid, moff, msize = struct.unpack_from("<III", data, d["offset"] + i * 12)
            entries.append({"id": mid, "offset": moff, "size": msize})
        data_chunk = out["chunks"].get("DATA")
        entries.sort(key=lambda e: e["offset"])
        contiguous = True
        cursor = 0
        for e in entries:
            if e["offset"] < cursor:
                contiguous = False
            cursor = max(cursor, e["offset"] + e["size"])
        out["media"] = entries
        out["media_contiguous"] = contiguous
        out["media_bytes"] = sum(e["size"] for e in entries)
        if data_chunk:
            out["data_size"] = data_chunk["size"]
            out["media_fill_ratio"] = out["media_bytes"] / float(data_chunk["size"] or 1)

    h = out["chunks"].get("HIRC")
    if h:
        p = h["offset"]
        count = struct.unpack_from("<I", data, p)[0]
        p += 4
        census = {}
        walk_ok = True
        while p + 5 <= h["offset"] + h["size"]:
            htype = data[p]
            sec = struct.unpack_from("<I", data, p + 1)[0]
            p += 5
            census[htype] = census.get(htype, 0) + 1
            p += sec
            if p > h["offset"] + h["size"]:
                walk_ok = False
                break
        out["hirc_count"] = count
        out["hirc_census"] = {HIRC_TYPES.get(k, "type %d" % k): v
                              for k, v in sorted(census.items())}
        out["hirc_walk_exact"] = walk_ok and p == h["offset"] + h["size"]

    s = out["chunks"].get("STID")
    if s:
        # u32 unknown, u32 count, then {u32 bankID, u8 length, char name[]}
        p = s["offset"]
        count = struct.unpack_from("<I", data, p + 4)[0]
        names = []
        q = p + 8
        for _ in range(count):
            if q + 5 > p + s["size"]:
                break
            _bank, length = struct.unpack_from("<IB", data, q)
            q += 5
            names.append(data[q:q + length].decode("latin-1", "replace"))
            q += length
        out["bank_name"] = ", ".join(names)
    return out


# --- WwiseInfo.wai ----------------------------------------------------------
def parse_wai(path: str) -> dict:
    """16-byte header + N x 20-byte records.

    0x00 u32 magic   0x20140808 (build date 2014-08-08)
    0x04 u32 unused
    0x08 u32 record size (20)
    0x0C u32 record count
    record: u32 id, f32 value, u32 flag_a, u32 flag_b, u32 flag_c
    """
    with open(path, "rb") as f:
        data = f.read()
    magic, _unk, recsize, count = struct.unpack_from("<4I", data, 0)
    recs = []
    for i in range(count):
        o = 16 + i * recsize
        rid, val, fa, fb, fc = struct.unpack_from("<IfIII", data, o)
        recs.append({"id": rid, "value": val, "flag_a": fa, "flag_b": fb, "flag_c": fc})
    return {
        "path": path,
        "size": len(data),
        "magic": magic,
        "record_size": recsize,
        "count": count,
        "sorted": [r["id"] for r in recs] == sorted(r["id"] for r in recs),
        "records": recs,
    }


# --- name recovery ----------------------------------------------------------
def exe_names(exe_path: str) -> dict:
    """Map fnv1(name) -> [original spellings] for every printable string in an exe."""
    with open(exe_path, "rb") as f:
        blob = f.read()
    h2s: dict[int, set] = {}
    for m in re.finditer(rb"[\x20-\x7e]{4,120}", blob):
        s = m.group().decode("latin-1")
        h2s.setdefault(fnv1(s), set()).add(s)
    return h2s


# --- commands ---------------------------------------------------------------
def cmd_scan(args):
    root = args.sound_dir
    wems, banks, wais, other = [], [], [], []
    for dirpath, _dirnames, filenames in os.walk(root):
        for name in sorted(filenames):
            p = os.path.join(dirpath, name)
            low = name.lower()
            if low.endswith(".wem"):
                w = parse_wem(p)
                w["path"] = os.path.relpath(p, root)
                wems.append(w)
            elif low.endswith(".bnk"):
                b = parse_bnk(p)
                b["path"] = os.path.relpath(p, root)
                banks.append(b)
            elif low.endswith(".wai"):
                wais.append(os.path.relpath(p, root))
            else:
                other.append((os.path.relpath(p, root), os.path.getsize(p)))

    by_dir: dict[str, list] = {}
    for w in wems:
        by_dir.setdefault(os.path.dirname(w["path"]) or ".", []).append(w)

    print("folder: %s" % root)
    print("%-28s %6s %14s %8s %8s" % ("directory", "files", "bytes", "seconds", "codecs"))
    for d in sorted(by_dir):
        rows = by_dir[d]
        total = sum(r["size"] for r in rows)
        dur = sum(r["duration"] or 0 for r in rows)
        codecs = sorted({r["codec"] for r in rows})
        print("%-28s %6d %14d %8.1f %s" % (d, len(rows), total, dur, ", ".join(codecs)))
    print()
    for b in banks:
        print("bank %s: %d bytes  version=0x%X (Wwise 2013.1/2)  id=%d  name=%s"
              % (b["path"], b["size"], b.get("bank_version", 0), b.get("bank_id", 0),
                 b.get("bank_name", "-")))
        print("   chunks: %s" % " ".join("%s(%d)" % (c, b["chunks"][c]["size"]) for c in b["order"]))
        if "media" in b:
            print("   DIDX: %d media, %d bytes, DATA fill %.4f"
                  % (len(b["media"]), b["media_bytes"], b.get("media_fill_ratio", 0)))
        if "hirc_census" in b:
            print("   HIRC: %d objects, walk exact=%s" % (b["hirc_count"], b["hirc_walk_exact"]))
            for k, v in b["hirc_census"].items():
                print("      %-28s %d" % (k, v))
    for w in wais:
        print("wai  %s" % w)
    for name, size in other:
        print("other %-40s %d bytes" % (name, size))

    if args.json:
        payload = {"wems": wems, "banks": banks, "wai": wais,
                   "other": [{"path": n, "size": s} for n, s in other]}
        with open(args.json, "w", encoding="utf8") as f:
            json.dump(payload, f, indent=1)
        print("\njson -> %s" % args.json)


def cmd_wem(args):
    for p in args.files:
        w = parse_wem(p)
        print("%-44s %-26s %dch %5s Hz %s %8d B"
              % (os.path.basename(p), w["codec"], w["channels"] or 0, w["rate"],
                 ("%7.2fs" % w["duration"]) if w["duration"] else "      ?",
                 w["data_size"] or 0))


def cmd_bank(args):
    for p in args.files:
        b = parse_bnk(p)
        print("%s: %d bytes" % (p, b["size"]))
        for c in b["order"]:
            print("   %-4s payload=%d at 0x%X" % (c, b["chunks"][c]["size"], b["chunks"][c]["offset"]))
        print("   version=0x%X id=%d name=%s lang=%s trailing=%d"
              % (b.get("bank_version", 0), b.get("bank_id", 0), b.get("bank_name", "-"),
                 b.get("language_id", 0), b["trailing"]))
        if "media" in b:
            print("   media=%d bytes=%d contiguous=%s fill=%.4f"
                  % (len(b["media"]), b["media_bytes"], b["media_contiguous"],
                     b.get("media_fill_ratio", 0)))
            for m in b["media"][:args.limit]:
                print("      %10d  0x%08X  off=%-8d size=%d" % (m["id"], m["id"], m["offset"], m["size"]))
        if "hirc_census" in b:
            print("   HIRC %d objects (walk exact=%s)" % (b["hirc_count"], b["hirc_walk_exact"]))
            for k, v in b["hirc_census"].items():
                print("      %-28s %d" % (k, v))


def cmd_wai(args):
    w = parse_wai(args.wai)
    print("record_size=%d count=%d sorted=%s magic=0x%08X"
          % (w["record_size"], w["count"], w["sorted"], w["magic"]))
    names = exe_names(args.names) if args.names else {}
    rows = []
    for r in w["records"]:
        nm = sorted(names.get(r["id"], ()))
        rows.append((r["id"], nm, r["value"], r["flag_a"], r["flag_b"], r["flag_c"]))
    if args.names:
        print("named: %d of %d" % (sum(1 for r in rows if r[1]), len(rows)))
    if args.csv:
        with open(args.csv, "w", encoding="utf8") as f:
            f.write("event_id,name,value,flag_a,flag_b,flag_c\n")
            for rid, nm, v, fa, fb, fc in rows:
                f.write("%d,%s,%g,%d,%d,%d\n" % (rid, (nm[0] if nm else ""), v, fa, fb, fc))
        print("csv -> %s" % args.csv)
    for rid, nm, v, fa, fb, fc in rows[:args.limit]:
        print("  %10d 0x%08X  %-34s %8.3f %d %d %d"
              % (rid, rid, (nm[0] if nm else ""), v, fa, fb, fc))


def cmd_names(args):
    h2s = exe_names(args.exe)
    with open(args.ids, "r", encoding="utf8") as f:
        ids = [int(x.strip(), 0) for x in f if x.strip() and not x.startswith("#")]
    hits = 0
    for i in ids:
        if i in h2s:
            hits += 1
            print("%11d 0x%08X  %s" % (i, i, sorted(h2s[i])[0]))
    print("# named %d of %d" % (hits, len(ids)), file=sys.stderr)


def cmd_prefix(args):
    b = parse_bnk(args.bank)
    data_path = args.bank
    with open(data_path, "rb") as f:
        blob = f.read()
    data_off = b["chunks"]["DATA"]["offset"]
    total = same = missing = 0
    truncated = 0
    for m in b.get("media", []):
        payload = blob[data_off + m["offset"]:data_off + m["offset"] + m["size"]]
        claimed = struct.unpack_from("<I", payload, 4)[0] + 8 if payload[:4] == b"RIFF" else m["size"]
        if claimed > m["size"]:
            truncated += 1
        total += 1
        stream = os.path.join(args.stream, "%d.wem" % m["id"])
        if not os.path.exists(stream):
            missing += 1
            continue
        with open(stream, "rb") as f:
            head = f.read(m["size"])
        if head == payload:
            same += 1
    print("media=%d  riff-claims-more=%d (prefix-shaped)  stream file present=%d  byte-prefix=%d"
          % (total, truncated, total - missing, same))


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)

    s = sub.add_parser("scan", help="inventory a sound folder")
    s.add_argument("sound_dir")
    s.add_argument("--json")
    s.set_defaults(func=cmd_scan)

    s = sub.add_parser("wem", help="summarise .wem files")
    s.add_argument("files", nargs="+")
    s.set_defaults(func=cmd_wem)

    s = sub.add_parser("bank", help="dump a .bnk")
    s.add_argument("files", nargs="+")
    s.add_argument("--limit", type=int, default=10)
    s.set_defaults(func=cmd_bank)

    s = sub.add_parser("wai", help="dump WwiseInfo.wai")
    s.add_argument("wai")
    s.add_argument("--names", help="exe to recover names from")
    s.add_argument("--csv")
    s.add_argument("--limit", type=int, default=20)
    s.set_defaults(func=cmd_wai)

    s = sub.add_parser("names", help="resolve object names from an exe's strings")
    s.add_argument("exe")
    s.add_argument("--ids", required=True, help="file with one decimal/0x id per line")
    s.set_defaults(func=cmd_names)

    s = sub.add_parser("prefix", help="check whether bank media are streamed-file prefixes")
    s.add_argument("bank")
    s.add_argument("--stream", required=True)
    s.set_defaults(func=cmd_prefix)

    args = ap.parse_args(argv)
    args.func(args)


if __name__ == "__main__":
    main()
