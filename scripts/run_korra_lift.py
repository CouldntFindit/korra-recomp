#!/usr/bin/env python3
"""Korra's lift driver: seeds -> function catalog -> generated C.

Two stages, both against a pinned pcrecomp checkout (``PCRECOMP`` env var):

  catalog <analysis.exe> <seeds.json> <catalog.json>
      Recursive-descent discovery (pcrecomp ``disasm32``) seeded with the entry
      point, exports, RTTI/vtable methods and any extra seeds. Writes every
      function's basic-block ranges so the lift stage can re-decode instead of
      running discovery twice (the first pass on a 5 MB image takes minutes).

  lift <analysis.exe> <catalog.json> <outdir> [--split N] [--names names.json]
      Rebuild the blocks from the catalog and lower each function with
      ``lift32.Lifter``. Writes ``recomp_NNNN.c``, ``recomp_funcs.h``,
      ``recomp_dispatch.c`` and ``lift_stats.json``.

``--names`` maps a VA (decimal or 0x-hex) to a name, e.g. the RTTI method names
from ``rtti.py``; unnamed functions stay ``sub_XXXXXXXX``.

Everything this writes is derived from the game binary: ``catalog.json`` lives
in ``work/`` and the C in ``src/recomp/gen/``, both gitignored.
"""
from __future__ import annotations

import json
import os
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
PCRECOMP = os.environ.get("PCRECOMP", os.path.normpath(os.path.join(ROOT, "..", "pcrecomp")))
for _sub in ("pe", "disasm", "lift"):
    _p = os.path.join(PCRECOMP, "tools", _sub)
    if _p not in sys.path:
        sys.path.insert(0, _p)

from pe_analyze import analyze_pe, build_iat_map          # noqa: E402
from disasm32 import Disassembler, Function, BasicBlock   # noqa: E402
from lift32 import Lifter                                 # noqa: E402


def load_seeds(path: str) -> list[int]:
    with open(path) as f:
        doc = json.load(f)
    out = []
    for e in doc:
        if isinstance(e, dict):
            e = e.get("address", e.get("address_hex"))
        out.append(int(e, 0) if isinstance(e, str) else int(e))
    return out


def open_image(exe: str):
    info = analyze_pe(exe)
    iat = build_iat_map(info)
    with open(exe, "rb") as f:
        data = f.read()
    return info, iat, Disassembler(data, info.image_base, info.sections)


def cmd_catalog(exe: str, seeds_path: str, out_path: str) -> int:
    info, iat, dis = open_image(exe)
    seeds = set(load_seeds(seeds_path)) if seeds_path and os.path.exists(seeds_path) else set()
    seeds.add(info.image_base + info.entry_point_rva)
    seeds |= {info.image_base + e.rva for e in info.exports
              if info.code_start <= info.image_base + e.rva < info.code_end}
    print("[*] %s: base=0x%08X code=0x%08X-0x%08X imports=%d seeds=%d"
          % (os.path.basename(exe), info.image_base, info.code_start, info.code_end, len(iat), len(seeds)),
          flush=True)

    t0 = time.time()
    functions = dis.find_functions(info.code_start, info.code_end, iat, seeds=seeds)
    print("[*] discovered %d functions in %.1fs" % (len(functions), time.time() - t0), flush=True)

    funcs = []
    for addr in sorted(functions):
        fn = functions[addr]
        funcs.append({
            "addr": fn.address,
            "end": fn.end,
            "name": fn.name,
            "kind": getattr(fn, "entry_kind", "start"),
            "thunk": bool(fn.is_thunk),
            "size": fn.size,
            "blocks": [[b.start, b.end] for b in sorted(fn.blocks.values(), key=lambda b: b.start)],
            "jump_targets": sorted(getattr(fn, "jump_targets", ()) or ()),
            "calls_to": sorted(fn.calls_to),
        })
    doc = {
        "exe": os.path.basename(exe),
        "image_base": info.image_base,
        "code_start": info.code_start,
        "code_end": info.code_end,
        "import_count": len(iat),
        "seed_count": len(seeds),
        "function_count": len(funcs),
        "functions": funcs,
    }
    os.makedirs(os.path.dirname(os.path.abspath(out_path)), exist_ok=True)
    with open(out_path, "w") as f:
        json.dump(doc, f)
    print("[*] wrote %s (%d functions, %.1f MB)" % (out_path, len(funcs), os.path.getsize(out_path) / 1e6))
    return 0


def rebuild_function(dis: Disassembler, rec: dict) -> Function:
    fn = Function(address=rec["addr"], end=rec["end"], name=rec["name"])
    fn.entry_kind = rec["kind"]
    fn.is_thunk = rec["thunk"]
    fn.size = rec["size"]
    fn.jump_targets = set(rec["jump_targets"])
    fn.calls_to = set(rec["calls_to"])
    for start, end in rec["blocks"]:
        insns = []
        for insn in dis.disassemble_at(start, max(end - start, 1)):
            if insn.end_address > end:
                break
            insns.append(insn)
        if not insns:
            continue
        block = BasicBlock(start=start, end=end, instructions=insns)
        block.is_exit = bool(insns[-1].is_ret)
        fn.blocks[start] = block
    return fn


def cmd_lift(exe: str, catalog_path: str, out_dir: str, split: int, names_path: str | None) -> int:
    info, iat, dis = open_image(exe)
    with open(catalog_path) as f:
        doc = json.load(f)
    names: dict[int, str] = {}
    if names_path and os.path.exists(names_path):
        for k, v in json.load(open(names_path)).items():
            names[int(k, 0) if isinstance(k, str) else int(k)] = v
    lifted_addrs = {rec["addr"] for rec in doc["functions"]}

    lifter = Lifter(iat_map=iat, func_names=names, lifted=lifted_addrs)
    os.makedirs(out_dir, exist_ok=True)

    emitted, errors, lines_total = 0, 0, 0
    chunk: list[str] = []
    file_idx = 0
    t0 = time.time()
    failures: list[dict] = []

    def flush() -> None:
        nonlocal file_idx, chunk
        path = os.path.join(out_dir, "recomp_%04d.c" % file_idx)
        with open(path, "w") as f:
            f.write("/* AUTO-GENERATED by scripts/run_korra_lift.py. DO NOT EDIT, DO NOT COMMIT. */\n")
            f.write('#include "recomp_types.h"\n#include "recomp_funcs.h"\n\n')
            f.write("\n".join(chunk))
        file_idx += 1
        chunk = []

    for i, rec in enumerate(doc["functions"]):
        try:
            fn = rebuild_function(dis, rec)
            if not fn.blocks:
                raise RuntimeError("no decodable blocks")
            code = lifter.lift_function(fn)
            chunk.append(code)
            emitted += 1
            lines_total += code.count("\n") + 1
        except Exception as e:  # noqa: BLE001 - every failure gets reported
            errors += 1
            failures.append({"addr": rec["addr"], "name": rec["name"], "error": repr(e)})
            chunk.append("void %s(void) { /* LIFT FAILED: %s */ }\n" % (rec["name"], str(e).replace("*/", "")))
        if len(chunk) >= split:
            flush()
            print("[*] %d/%d functions, %d files, %d errors"
                  % (emitted + errors, len(doc["functions"]), file_idx, errors), flush=True)
    if chunk:
        flush()

    with open(os.path.join(out_dir, "recomp_funcs.h"), "w") as f:
        f.write("#pragma once\n#include <stdint.h>\n\n")
        for rec in doc["functions"]:
            f.write("void %s(void);  /* 0x%08X */\n" % (rec["name"], rec["addr"]))

    with open(os.path.join(out_dir, "recomp_dispatch.c"), "w") as f:
        f.write("/* AUTO-GENERATED. DO NOT EDIT, DO NOT COMMIT. */\n")
        f.write('#include "recomp_types.h"\n#include "recomp_funcs.h"\n\n')
        f.write("const recomp_dispatch_entry_t recomp_dispatch_table[] = {\n")
        for rec in doc["functions"]:
            f.write("    { 0x%08Xu, %s },\n" % (rec["addr"], rec["name"]))
        f.write("};\n\nconst uint32_t recomp_dispatch_count = %du;\n" % len(doc["functions"]))

    stats = {
        "exe": doc["exe"],
        "functions_catalogued": len(doc["functions"]),
        "functions_lifted": emitted,
        "lift_failures": errors,
        "failures": failures,
        "source_files": file_idx,
        "lines": lines_total,
        "seconds": round(time.time() - t0, 1),
    }
    with open(os.path.join(out_dir, "lift_stats.json"), "w") as f:
        json.dump(stats, f, indent=1)
    print("\n[*] === COMPLETE ===")
    print("[*] %d/%d functions lifted, %d failures" % (emitted, len(doc["functions"]), errors))
    print("[*] %d files, %d lines, %.1fs" % (file_idx, lines_total, stats["seconds"]))
    if failures:
        print("[*] first failures: " + json.dumps(failures[:5], indent=1))
    return 0 if errors == 0 else 1


def main(argv: list[str]) -> int:
    if len(argv) < 2:
        print(__doc__)
        return 2
    cmd = argv[1]
    if cmd == "catalog":
        return cmd_catalog(argv[2], argv[3] if len(argv) > 3 else "", argv[4])
    if cmd == "lift":
        split = 500
        names = None
        rest = argv[2:]
        if "--split" in rest:
            i = rest.index("--split")
            split = int(rest[i + 1])
            del rest[i:i + 2]
        if "--names" in rest:
            i = rest.index("--names")
            names = rest[i + 1]
            del rest[i:i + 2]
        return cmd_lift(rest[0], rest[1], rest[2], split, names)
    print(__doc__)
    return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv))
