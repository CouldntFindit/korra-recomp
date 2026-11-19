#!/usr/bin/env python3
"""usm_demux.py -- demux a CRI Sofdec2 (.usm) movie into its elementary streams.

Reads one .usm, walks the chunk chain, and writes one file per stream:
MPEG-2/H.264 video as a raw elementary stream (.m2v/.h264), ADX/HCA audio as
.adx/.hca. Also reports container metadata (resolution, fps, duration, streams).

    python tools/usm_demux.py <file.usm> --out <dir> [--json file.json]
                             [--max-chunks N] [--dry-run]

Container layout (verified on this game's files, see docs/assets.md):
  0x00  "CRID"            u32 size_field (real length = size_field + 8)
  0x20  "@UTF"            CRIUSF_DIR_STREAM directory table
  8+size_field            first chunk; chunks follow back to back, each
                          (real length) = u32@+0x04 + 8
  chunk header (0x20 B, big-endian):
    +0x00 magic "@SFV"/"@SFA"/"@ALP"/"@CUE"/"@SBT"/"@STA"/"@USR"/…
    +0x04 u32 size_field, +0x08 u16 payload_offset, +0x0A u16 pad
    +0x0C u8 channel, +0x0E u16 type (0=data,1=table,2=#HEADER,3=metadata)
    +0x10 u32 frame_time, +0x14 u32 frame_rate (x100, e.g. 3000 = 30.000)
  payload at chunk + 8 + payload_offset; for data chunks the last `pad` bytes
  are padding and are stripped before appending to the elementary stream.
"""
from __future__ import annotations

import argparse
import json
import os
import struct
import sys
from collections import defaultdict

CHUNK_MAGICS = {b"@SFV", b"@SFA", b"@ALP", b"@CUE", b"@SBT", b"@STA",
                b"@USR", b"@AHX", b"@ELM", b"@ATP", b"@PST"}
TYPE_DATA, TYPE_TABLE, TYPE_HEADER, TYPE_META = 0x0000, 0x0001, 0x0002, 0x0003

MPEG2_FPS = {1: 24000 / 1001, 2: 24.0, 3: 25.0, 4: 30000 / 1001,
             5: 30.0, 6: 50.0, 7: 60000 / 1001, 8: 60.0}


def parse_mpeg2_sequence_header(es: bytes) -> dict | None:
    """Read the first sequence header (00 00 01 B3) of an MPEG-1/2 video ES."""
    i = es.find(b"\x00\x00\x01\xb3")
    if i < 0:
        return None
    b = es[i + 4:i + 8]
    if len(b) < 4:
        return None
    bits = int.from_bytes(b, "big")
    width = (bits >> 20) & 0xFFF
    height = (bits >> 8) & 0xFFF
    aspect = (bits >> 4) & 0xF
    frc = bits & 0xF
    return {"codec": "mpeg2", "width": width, "height": height,
            "aspect_code": aspect, "frame_rate_code": frc,
            "fps": MPEG2_FPS.get(frc)}


def parse_adx_header(data: bytes) -> dict | None:
    if data[:2] != b"\x80\x00":
        return None
    if len(data) < 0x14:
        return None
    hdr_size = struct.unpack_from(">H", data, 2)[0]
    enc, block = data[4], data[5]
    bitdepth, channels = data[6], data[7]
    rate, samples = struct.unpack_from(">II", data, 8)
    highpass = struct.unpack_from(">H", data, 0x10)[0]
    return {"codec": "adx", "header_size": hdr_size, "encoding": enc,
            "block_size": block, "bitdepth": bitdepth, "channels": channels,
            "sample_rate": rate, "samples": samples, "highpass": highpass}


def parse_hca_header(data: bytes) -> dict | None:
    if data[:3] != b"HCA":
        return None
    if len(data) < 0x14:
        return None
    version = struct.unpack_from(">H", data, 4)[0]
    hdr_size = struct.unpack_from(">H", data, 6)[0]
    fmt, channels = data[8], data[9]
    rate = int.from_bytes(data[10:13], "big")
    blocks = struct.unpack_from(">I", data, 0x0D)[0]
    return {"codec": "hca", "version": version, "header_size": hdr_size,
            "bitdepth_code": fmt, "channels": channels,
            "sample_rate": rate, "block_count": blocks}


def detect_stream(data: bytes) -> dict:
    for fn in (parse_hca_header, parse_adx_header):
        info = fn(data)
        if info:
            return info
    if b"\x00\x00\x01\xb3" in data[:0x4000]:
        info = parse_mpeg2_sequence_header(data)
        if info:
            return info
    if b"\x00\x00\x00\x01" in data[:0x4000]:
        return {"codec": "h264"}
    return {"codec": "unknown", "head": data[:16].hex()}


def walk(data: bytes, max_chunks: int | None):
    """Yield parsed chunks starting after the CRID block."""
    first = 8 + struct.unpack_from(">I", data, 4)[0]
    pos = first
    n = 0
    while pos + 0x20 <= len(data):
        magic = data[pos:pos + 4]
        if magic not in CHUNK_MAGICS:
            yield {"error": f"unknown chunk magic {magic!r}", "offset": pos}
            return
        size_field = struct.unpack_from(">I", data, pos + 4)[0]
        poff, pad = struct.unpack_from(">HH", data, pos + 8)
        chn = data[pos + 0x0C]
        ctype = struct.unpack_from(">H", data, pos + 0x0E)[0]
        ftime, frate = struct.unpack_from(">II", data, pos + 0x10)
        payload_start = pos + 8 + poff
        payload_end = pos + 8 + size_field
        if payload_end > len(data):
            yield {"error": "chunk overruns file", "offset": pos}
            return
        yield {
            "offset": pos, "magic": magic.decode("ascii", "replace"),
            "size": size_field, "pad": pad, "channel": chn, "type": ctype,
            "frame_time": ftime, "frame_rate": frate,
            "payload": data[payload_start:payload_end],
            "payload_start": payload_start,
        }
        pos = payload_end
        n += 1
        if max_chunks and n >= max_chunks:
            return


def main(argv=None):
    ap = argparse.ArgumentParser(description="Demux a CRI .usm movie")
    ap.add_argument("usm")
    ap.add_argument("--out", required=True, help="output directory")
    ap.add_argument("--json", help="write a JSON report here")
    ap.add_argument("--max-chunks", type=int)
    ap.add_argument("--dry-run", action="store_true")
    args = ap.parse_args(argv)

    with open(args.usm, "rb") as f:
        data = f.read()
    if data[:4] != b"CRID":
        sys.exit(f"{args.usm}: not a USM (magic {data[:4]!r})")

    # Streams are identified by (chunk magic, channel number): every chunk in
    # this game's files carries channel 0, so the magic is what separates the
    # video stream from the audio stream.
    streams = defaultdict(lambda: {"magic": "", "chunks": 0, "es": bytearray(),
                                   "frames": 0, "frame_rate": 0,
                                   "last_frame_time": 0, "pad_bytes": 0,
                                   "bad_pad": 0, "channels": set()})
    counts = defaultdict(int)
    errors = []
    for ch in walk(data, args.max_chunks):
        if "error" in ch:
            errors.append(ch["error"] + f" @0x{ch.get('offset', 0):x}")
            break
        counts[ch["magic"]] += 1
        if ch["type"] != TYPE_DATA:
            continue
        st = streams[(ch["magic"], ch["channel"])]
        st["magic"] = ch["magic"]
        st["chunks"] += 1
        st["frame_rate"] = ch["frame_rate"] or st["frame_rate"]
        st["last_frame_time"] = max(st["last_frame_time"], ch["frame_time"])
        payload = ch["payload"]
        if ch["pad"]:
            pad = payload[len(payload) - ch["pad"]:]
            st["pad_bytes"] += ch["pad"]
            if any(b not in (0x00, 0xFF) for b in pad):
                st["bad_pad"] += 1
            payload = payload[:len(payload) - ch["pad"]]
        st["es"] += payload
        st["frames"] += 1
        st["channels"].add(ch["channel"])
    os.makedirs(args.out, exist_ok=True)
    base = os.path.splitext(os.path.basename(args.usm))[0]
    report = {"usm": os.path.basename(args.usm), "bytes": len(data),
              "chunk_counts": dict(counts), "errors": errors,
              "streams": []}
    for key, st in sorted(streams.items()):
        magic, chn = key
        info = detect_stream(bytes(st["es"]))
        ext = {"mpeg2": "m2v", "h264": "h264", "adx": "adx",
               "hca": "hca"}.get(info["codec"], "bin")
        name = f"{base}.{magic.strip('@').lower()}.ch{chn}.{ext}"
        path = os.path.join(args.out, name)
        if not args.dry_run:
            with open(path, "wb") as f:
                f.write(st["es"])
        fps = info.get("fps") or (st["frame_rate"] / 100.0 if st["frame_rate"] else 0)
        es = bytes(st["es"])
        # The timeline tick is 1/3000 s, not 1 ms: 125 ticks per 23.976 fps
        # frame (23.976 * 125 = 2997). Cross-check against the codec itself.
        timeline_s = st["last_frame_time"] / 3000.0 if st["last_frame_time"] else None
        codec_s = None
        if info["codec"] == "adx" and info.get("sample_rate"):
            codec_s = info["samples"] / info["sample_rate"]
        elif info["codec"] == "hca" and info.get("sample_rate"):
            codec_s = info["block_count"] * 1024 / info["sample_rate"]
        elif info["codec"] == "mpeg2":
            pictures = es.count(b"\x00\x00\x01\x00")
            codec_s = pictures / fps if fps else None
        entry = {
            "channel": chn, "magic": st["magic"], "data_chunks": st["chunks"],
            "bytes": len(es), "padding_bytes": st["pad_bytes"],
            "bad_padding_chunks": st["bad_pad"],
            "frame_rate_x100": st["frame_rate"],
            "last_frame_time_tick": st["last_frame_time"],
            "timeline_duration_s": round(timeline_s, 3) if timeline_s else None,
            "codec_duration_s": round(codec_s, 3) if codec_s else None,
            "fps": round(fps, 3) if fps else None,
            "file": None if args.dry_run else path,
            "codec_info": info,
        }
        if info["codec"] == "mpeg2":
            entry["pictures"] = es.count(b"\x00\x00\x01\x00")
            entry["sequence_headers"] = es.count(b"\x00\x00\x01\xb3")
            entry["gop_headers"] = es.count(b"\x00\x00\x01\xb8")
        report["streams"].append(entry)
        print(f"{st['magic']} ch{chn} {info['codec']:<8} "
              f"{len(es):>10,} B  {st['chunks']:>5} chunks  "
              f"{entry['fps'] or 0:>6.2f} fps  "
              f"{entry['codec_duration_s'] or 0:>7.2f} s (codec)  "
              f"{entry['timeline_duration_s'] or 0:>7.2f} s (timeline)  -> {name}")

    if report["streams"]:
        print("chunk histogram:", dict(counts))
    if errors:
        print("errors:", errors)
    if args.json:
        os.makedirs(os.path.dirname(os.path.abspath(args.json)), exist_ok=True)
        with open(args.json, "w", encoding="utf-8") as f:
            json.dump(report, f, indent=1)
        print("->", args.json)
    return 0


if __name__ == "__main__":
    sys.exit(main())
