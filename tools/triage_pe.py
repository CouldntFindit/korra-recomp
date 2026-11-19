#!/usr/bin/env python3
"""PE triage report for a recompilation target.

Prints (and optionally writes JSON for) everything the lift needs to know
before any code is generated: header facts, section entropies, imports with
ordinals, debug/PDB reference, data directories, security flags, and the
middleware markers we grep for by hand anyway.

  python tools/triage_pe.py "The Legend of Korra\\LoK.exe" [--json work/triage.json]

The report contains no game bytes, only metadata, so it is safe to commit
(result lives in docs/PE_TRIAGE.md).
"""
from __future__ import annotations

import collections
import hashlib
import json
import math
import os
import sys

import pefile

KNOWN_STRINGS = [
    b"CRI File System", b"CRILAYLA", b"CPKMC", b"USM", b"HCA-MX", b"NcVoice",
    b"Wwise", b"AkSoundEngine", b"AkBankManager", b"Init.bnk", b"WwiseInfo",
    b"Direct3DCreate9", b"D3DXCreateTexture", b"XAudio2", b"CoCreateInstance",
    b"ProgressFrameRateType", b"DrawFrameRateType", b"MotionFrameRateType",
    b"framerate_d", b"framerate_n", b"Platinum", b"PRJ_024", b"steam_api",
    b"fopen_s", b"BeginScene", b"Present", b"ShaderModel",
]


def entropy(data: bytes) -> float:
    if not data:
        return 0.0
    counts = collections.Counter(data)
    n = len(data)
    return -sum((c / n) * math.log2(c / n) for c in counts.values())


def sha256_file(path: str) -> str:
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def gh(data: bytes, path: str) -> bytes:
    with open(path, "rb") as f:
        f.seek(data)
        return f.read()


def main(argv: list[str]) -> int:
    if len(argv) < 2:
        print(__doc__)
        return 2
    path = argv[1]
    out_json = None
    if "--json" in argv:
        out_json = argv[argv.index("--json") + 1]

    pe = pefile.PE(path, fast_load=True)
    oh = pe.OPTIONAL_HEADER
    fh = pe.FILE_HEADER
    rep: dict = {}

    rep["file"] = os.path.basename(path)
    rep["size"] = os.path.getsize(path)
    rep["sha256"] = sha256_file(path)
    rep["machine"] = hex(fh.Machine)
    rep["subsystem"] = oh.Subsystem
    rep["image_base"] = hex(oh.ImageBase)
    rep["entry_point"] = hex(oh.ImageBase + oh.AddressOfEntryPoint)
    rep["timestamp"] = fh.TimeDateStamp
    rep["sections"] = [
        {
            "name": s.Name.rstrip(b"\0").decode("latin-1"),
            "va": hex(s.VirtualAddress),
            "vsize": s.Misc_VirtualSize,
            "raw": s.SizeOfRawData,
            "chars": hex(s.Characteristics),
            "entropy": round(entropy(s.get_data()), 3),
        }
        for s in pe.sections
    ]
    rep["dll_characteristics"] = hex(oh.DllCharacteristics)
    rep["directories"] = [
        {"name": pefile.DIRECTORY_ENTRY[k], "va": hex(d.VirtualAddress), "size": d.Size}
        for k, d in enumerate(oh.DATA_DIRECTORY) if d.VirtualAddress or d.Size
    ]

    pe.parse_data_directories(directories=[
        pefile.DIRECTORY_ENTRY["IMAGE_DIRECTORY_ENTRY_IMPORT"],
        pefile.DIRECTORY_ENTRY["IMAGE_DIRECTORY_ENTRY_DELAY_IMPORT"],
        pefile.DIRECTORY_ENTRY["IMAGE_DIRECTORY_ENTRY_EXPORT"],
        pefile.DIRECTORY_ENTRY["IMAGE_DIRECTORY_ENTRY_DEBUG"],
        pefile.DIRECTORY_ENTRY["IMAGE_DIRECTORY_ENTRY_TLS"],
        pefile.DIRECTORY_ENTRY["IMAGE_DIRECTORY_ENTRY_RESOURCE"],
    ])

    imports = {}
    for entry in getattr(pe, "DIRECTORY_ENTRY_IMPORT", []) or []:
        names = []
        for imp in entry.imports:
            names.append(imp.name.decode("latin-1") if imp.name else "#%d" % imp.ordinal)
        imports[entry.dll.decode("latin-1")] = sorted(names)
    rep["imports"] = imports
    rep["import_count"] = sum(len(v) for v in imports.values())

    rep["delay_imports"] = {
        e.dll.decode("latin-1"): sorted(i.name.decode("latin-1") if i.name else "#%d" % i.ordinal
                                        for i in e.imports)
        for e in getattr(pe, "DIRECTORY_ENTRY_DELAY_IMPORT", []) or []
    }
    rep["reloc"] = {
        "rva": hex(oh.DATA_DIRECTORY[5].VirtualAddress),
        "size": oh.DATA_DIRECTORY[5].Size,
    }
    rep["tls"] = bool(getattr(pe, "DIRECTORY_ENTRY_TLS", None))
    rep["exports"] = sorted({e.name.decode("latin-1") for e in getattr(pe, "DIRECTORY_ENTRY_EXPORT", type("x", (), {"symbols": []})).symbols if e.name})

    debug = []
    for d in getattr(pe, "DIRECTORY_ENTRY_DEBUG", []) or []:
        entry = {"type": d.struct.Type}
        if d.entry:
            entry["pdb"] = d.entry.PdbFileName.rstrip(b"\0").decode("latin-1")
            entry["guid_age"] = "%08x%04x%04x" % (d.entry.Signature_Data1,
                                                  d.entry.Signature_Data2,
                                                  d.entry.Signature_Data3)
            entry["age"] = d.entry.Age
        debug.append(entry)
    rep["debug"] = debug

    # Middleware / anchor strings, counted across the whole file.
    raw = open(path, "rb").read()
    rep["strings"] = {s.decode("latin-1"): raw.count(s) for s in KNOWN_STRINGS}
    rep["rtti_names"] = raw.count(b".?AV")
    rep["rtti_hw"] = raw.count(b"Hw@@") + raw.count(b"Hw::")

    lines = []
    add = lines.append
    add("# PE triage — %s" % rep["file"])
    add("")
    add("| Property | Value |")
    add("|---|---|")
    add("| Size | %d bytes |" % rep["size"])
    add("| SHA-256 | `%s` |" % rep["sha256"])
    add("| Machine | %s |" % rep["machine"])
    add("| Subsystem | %s |" % rep["subsystem"])
    add("| Image base | %s |" % rep["image_base"])
    add("| Entry point | %s |" % rep["entry_point"])
    add("| Timestamp | %s |" % rep["timestamp"])
    add("| DllCharacteristics | %s |" % rep["dll_characteristics"])
    add("| Imports | %d functions in %d DLLs |" % (rep["import_count"], len(imports)))
    add("| TLS directory | %s |" % rep["tls"])
    add("| Base relocations | RVA %s, %#x bytes |" % (rep["reloc"]["rva"], rep["reloc"]["size"]))
    add("| RTTI names (`.?AV`) | %d |" % rep["rtti_names"])
    add("")
    add("## Sections")
    add("")
    add("| Name | VA | Virtual | Raw | Entropy | Characteristics |")
    add("|---|---|---|---|---|---|")
    for s in rep["sections"]:
        add("| `%s` | %s | %#x | %#x | %.3f | %s |" % (s["name"], s["va"], s["vsize"], s["raw"], s["entropy"], s["chars"]))
    add("")
    add("## Imports")
    add("")
    for dll, fns in sorted(imports.items()):
        add("### %s (%d)" % (dll, len(fns)))
        add("")
        add(", ".join("`%s`" % f for f in fns))
        add("")
    if rep["delay_imports"]:
        add("## Delay imports")
        add("")
        for dll, fns in sorted(rep["delay_imports"].items()):
            add("- **%s**: %s" % (dll, ", ".join("`%s`" % f for f in fns)))
        add("")
    add("## Debug / PDB")
    add("")
    for d in debug:
        add("- type %s%s" % (d["type"], (" — `%s` GUID %s age %s" % (d["pdb"], d["guid_age"], d["age"])) if "pdb" in d else ""))
    add("")
    add("## Strings of interest")
    add("")
    add("| String | Occurrences |")
    add("|---|---|")
    for s, n in rep["strings"].items():
        if n:
            add("| `%s` | %d |" % (s, n))
    add("")
    add("## Data directories")
    add("")
    for d in rep["directories"]:
        add("- %s: VA %s size %#x" % (d["name"], d["va"], d["size"]))

    text = "\n".join(lines) + "\n"
    print(text)
    if out_json:
        os.makedirs(os.path.dirname(os.path.abspath(out_json)), exist_ok=True)
        with open(out_json, "w", encoding="utf-8") as f:
            json.dump(rep, f, indent=1)
        print("wrote %s" % out_json)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
