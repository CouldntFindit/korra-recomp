#!/usr/bin/env python3
"""Re-verify the audio findings in docs/audio.md against the user's own data.

Run after any tool change, and on a fresh game copy, to prove the document still
describes reality.  Exits non-zero if an assertion fails.

    python tools\\verify_audio.py --game "..\\The Legend of Korra"
    python tools\\verify_audio.py --game "..\\The Legend of Korra" --dump work\\bgm.xml

`--dump` is optional: it is the wwiser XML of BGM.bnk, needed for the event-graph
assertions (event -> action -> track -> media).  Without it those checks report
SKIP instead of failing.
"""
from __future__ import annotations

import argparse
import os
import re
import struct
import sys
import xml.etree.ElementTree as ET

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import audio_inventory as inv  # noqa: E402


def fnv1(s: str) -> int:
    h = 0x811C9DC5
    for c in s.lower().encode("latin-1"):
        h = ((h * 0x01000193) & 0xFFFFFFFF) ^ c
    return h


def graph_from_xml(path: str):
    """Pull event -> targets -> media out of a wwiser dump (same walk as docs/audio.md 3.3)."""
    root = ET.parse(path).getroot()

    def fields(el):
        return {f.get("name"): f.get("value") for f in el.findall("field")}

    def U(v):
        return int(v) if v not in (None, "") else None

    def sub(el, tag):
        for c in el.iter("object"):
            if c.get("name") == tag:
                return c

    events, actions, tracks, nodes = {}, {}, {}, {}
    for el in root.iter("object"):
        kind = el.get("name") or ""
        if not (kind in ("CAkEvent", "CAkMusicTrack", "CAkMusicSegment",
                         "CAkMusicRanSeqCntr", "CAkMusicSwitchCntr")
                or kind.startswith("CAkAction")):
            continue
        f = fields(el)
        oid = U(f.get("ulID"))
        if kind == "CAkEvent":
            events[oid] = [int(fields(a)["ulActionID"])
                           for lst in el.iter("list") if lst.get("name") == "actions"
                           for a in lst.findall("object")]
        elif kind.startswith("CAkAction"):
            allf = {x.get("name"): x.get("value") for x in el.iter("field")}
            actions[oid] = {"kind": kind.replace("CAkAction", ""),
                            "target": U(allf.get("idExt"))}
        elif kind == "CAkMusicTrack":
            src = []
            for bs in el.iter("object"):
                if bs.get("name") == "AkBankSourceData":
                    mi = sub(bs, "AkMediaInformation")
                    if mi is not None:
                        src.append(U(fields(mi).get("sourceID")))
            tracks[oid] = src
        else:
            ch = [int(fields(c)["ulChildID"]) for c in el.iter("object")
                  if c.get("name") == "Children" and "ulChildID" in fields(c)]
            nodes[oid] = ch

    def media_of(nid, seen=None):
        seen = seen or set()
        if nid in seen:
            return set()
        seen.add(nid)
        out = set(m for m in tracks.get(nid, []) if m)
        for c in nodes.get(nid, []):
            out |= media_of(c, seen)
        return out

    reachable = {}
    for eid, acts in events.items():
        tg = {actions[a]["target"] for a in acts
              if actions.get(a, {}).get("kind") == "Play" and actions[a]["target"]}
        ms = set()
        for t in tg:
            ms |= media_of(t)
        reachable[eid] = (tg, ms)
    return reachable, tracks


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--game", required=True, help="game root containing GameData\\")
    ap.add_argument("--dump", help="wwiser XML dump of BGM.bnk (optional)")
    args = ap.parse_args(argv)

    sound = os.path.join(args.game, "GameData", "sound")
    exe = os.path.join(args.game, "LoK.exe")
    results = []

    def check(name, ok, detail=""):
        results.append(ok)
        print("%-4s %-54s %s" % ("PASS" if ok else "FAIL", name, detail))

    def skip(name):
        print("%-4s %-54s %s" % ("SKIP", name, "no --dump given"))

    # --- streams ------------------------------------------------------------
    wems, banks = [], {}
    for dirpath, _dn, files in os.walk(sound):
        for f in files:
            p = os.path.join(dirpath, f)
            if f.endswith(".wem"):
                wems.append(inv.parse_wem(p))
            elif f.endswith(".bnk"):
                banks[os.path.relpath(p, sound)] = inv.parse_bnk(p)

    check("779 .wem files", len(wems) == 779, "found %d" % len(wems))
    check("2 banks (Init.bnk, bgm\\BGM.bnk)", len(banks) == 2, ", ".join(sorted(banks)))
    dur = sum(w["duration"] or 0 for w in wems)
    check("total audio 6,257 s (104.3 min)", abs(dur - 6257) < 3, "%.1f s" % dur)
    check("total .wem bytes 166,350,801",
          sum(w["size"] for w in wems) == 166350801, "")
    check("every RIFF size matches its file size",
          all(w["ripple"] for w in wems), "%d/%d" % (sum(1 for w in wems if w["ripple"]), len(wems)))
    codecs = {}
    for w in wems:
        codecs[w["codec"]] = codecs.get(w["codec"], 0) + 1
    check("777 Vorbis + 1 IMA ADPCM + 1 PCM16",
          codecs.get("Wwise Vorbis") == 777 and codecs.get("Wwise IMA ADPCM") == 1
          and codecs.get("PCM (extensible header)") == 1, str(codecs))
    chans = {}
    for w in wems:
        chans[w["channels"]] = chans.get(w["channels"], 0) + 1
    check("surround music present (50x4ch, 79x5ch, 3x6ch)",
          chans.get(4) == 50 and chans.get(5) == 79 and chans.get(6) == 3,
          str(sorted(chans.items())))
    hca = [f for dp, _dn, fs in os.walk(sound) for f in fs
           if f.lower().endswith((".hca", ".adx", ".acb", ".awb"))]
    check("no HCA/ADX/ACB/AWB content", not hca, "%d files" % len(hca))

    # --- banks --------------------------------------------------------------
    init, bgm = banks["Init.bnk"], banks[os.path.join("bgm", "BGM.bnk")]
    check("bank generator version 0x58 (Wwise 2013.1/2)",
          init["bank_version"] == 0x58 and bgm["bank_version"] == 0x58,
          "init=0x%X bgm=0x%X" % (init["bank_version"], bgm["bank_version"]))
    check("bank IDs = FNV-1 of the lowercase name",
          init["bank_id"] == fnv1("init") and bgm["bank_id"] == fnv1("bgm"),
          "init=%d bgm=%d" % (init["bank_id"], bgm["bank_id"]))
    ic = init["hirc_census"]
    check("Init.bnk: 47 Bus / 8 AuxBus / 18 FX / 7 State, 0 events",
          ic.get("Bus") == 47 and ic.get("AuxBus") == 8 and ic.get("State") == 7
          and ic.get("FxShareSet", 0) + ic.get("FxCustom", 0) == 18
          and ic.get("Event", 0) == 0 and "media" not in init, "")
    bc = bgm["hirc_census"]
    check("BGM.bnk: 295 Event / 192 Track / 187 Segment / 69 RanSeq / 18 Switch",
          bc.get("Event") == 295 and bc.get("MusicTrack") == 192
          and bc.get("MusicSegment") == 187
          and bc.get("MusicRandomSequenceContainer") == 69
          and bc.get("MusicSwitchContainer") == 18, "")
    check("both HIRC walks end exactly at the chunk end",
          init["hirc_walk_exact"] and bgm["hirc_walk_exact"], "")
    check("BGM.bnk has 120 DIDX media", len(bgm["media"]) == 120, "")

    # --- prefetch claim -----------------------------------------------------
    blob = open(os.path.join(sound, "bgm", "BGM.bnk"), "rb").read()
    doff = bgm["chunks"]["DATA"]["offset"]
    shaped = same = present = 0
    for m in bgm["media"]:
        payload = blob[doff + m["offset"]:doff + m["offset"] + m["size"]]
        if struct.unpack_from("<I", payload, 4)[0] + 8 > m["size"]:
            shaped += 1
        fp = os.path.join(sound, "stream", "%d.wem" % m["id"])
        if os.path.exists(fp):
            present += 1
            with open(fp, "rb") as f:
                same += f.read(m["size"]) == payload
    check("119/120 bank media claim more than stored (prefix-shaped)", shaped == 119, "%d" % shaped)
    check("95 stream counterparts, all byte-identical prefixes",
          present == 95 and same == 95, "present=%d identical=%d" % (present, same))

    # --- WwiseInfo.wai ------------------------------------------------------
    wai = inv.parse_wai(os.path.join(sound, "WwiseInfo.wai"))
    wai_ids = {r["id"] for r in wai["records"]}
    check(".wai: 2,581 sorted 20-byte records",
          wai["count"] == 2581 and wai["record_size"] == 20 and wai["sorted"],
          "%d records" % wai["count"])

    # --- names --------------------------------------------------------------
    names = {}
    if os.path.exists(exe):
        for m in re.finditer(rb"[\x20-\x7e]{4,120}", open(exe, "rb").read()):
            names.setdefault(fnv1(m.group().decode("latin-1")), set()).add(
                m.group().decode("latin-1"))
        check("977/2581 .wai ids named from exe literals",
              sum(1 for i in wai_ids if i in names) == 977,
              "%d" % sum(1 for i in wai_ids if i in names))
    else:
        print("%-4s %-54s %s" % ("SKIP", "name recovery", "LoK.exe not found"))

    # --- music graph --------------------------------------------------------
    if args.dump:
        reachable, tracks = graph_from_xml(args.dump)
        track_media = {m for src in tracks.values() for m in src}
        ev_media = set().union(*[ms for _tg, ms in reachable.values()])
        check("295 events in the HIRC graph", len(reachable) == 295, "%d" % len(reachable))
        check("all music events appear in .wai",
              all(e in wai_ids for e in reachable), "")
        withplay = sum(1 for tg, _ in reachable.values() if tg)
        check("183 events carry a Play action", withplay == 183, "%d" % withplay)
        check("150 track media / 74 event-reachable",
              len(track_media) == 150 and len(ev_media) == 74,
              "track=%d reachable=%d" % (len(track_media), len(ev_media)))
        if names:
            named = sum(1 for e in reachable if e in names)
            check("97/295 music events named", named == 97, "%d" % named)
    else:
        skip("295 events in the HIRC graph")
        skip("183 events carry a Play action")
        skip("150 track media / 74 event-reachable")

    # --- SeFootstep.bin -----------------------------------------------------
    sfs = open(os.path.join(sound, "SeFootstep.bin"), "rb").read()
    magic, t1o, t1n, t2o, t2n = struct.unpack_from("<4s4I", sfs, 0)
    ids = {struct.unpack_from("<I", sfs, t2o + i * 24 + 20)[0] for i in range(t2n)}
    check("SeFootstep.bin: sfs magic, 2+44 records, 11 media ids",
          magic == b"sfs\x00" and t1n == 2 and t2n == 44 and len(ids) == 11
          and t2o + t2n * 24 == len(sfs), "ids=%d" % len(ids))
    check("footstep media are not in stream\\ (they live in core.bnk)",
          not any(os.path.exists(os.path.join(sound, "stream", "%d.wem" % i)) for i in ids), "")

    failed = results.count(False)
    print("\n%d assertions, %d failed" % (len(results), failed))
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
