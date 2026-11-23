"""Which lifted functions call a COM vtable slot, and on what base register.

Usage: python tools/icall_slot_scan.py 0x54 0x64 ...

Scans src/recomp/gen/*.c for `RECOMP_ICALL(MEM32(<base> + 0xNN))`, attributes
each hit to the enclosing `void sub_XXXXXXXX(void) {`, and prints one line per
(function, slot) with the base expression and a count. Read-only; no game
bytes are printed.
"""
import glob
import re
import sys

CALL = re.compile(r"RECOMP_ICALL\(MEM32\(([^)]+?)\)\);.*?/\* 0x([0-9A-Fa-f]+): call")
FUNC = re.compile(r"\nvoid (sub_[0-9A-Fa-f]{8})\(void\) \{")


def main() -> int:
    slots = {"0x" + a.lower().replace("0x", "") for a in sys.argv[1:]}
    if not slots:
        print(__doc__)
        return 2
    rows = {}
    for path in glob.glob("src/recomp/gen/recomp_*.c"):
        with open(path, encoding="utf-8", errors="replace") as f:
            text = f.read()
        funcs = [(m.start(), m.group(1)) for m in FUNC.finditer(text)]
        if not funcs:
            continue
        names = [f[0] for f in funcs]
        import bisect
        for m in CALL.finditer(text):
            base = m.group(1).strip()
            off = None
            for tok in base.split("+"):
                tok = tok.strip()
                if tok.startswith("0x"):
                    try:
                        off = hex(int(tok, 16))
                    except ValueError:
                        pass
                    break
            if off not in slots:
                continue
            i = bisect.bisect_right(names, m.start()) - 1
            fn = funcs[i][1] if i >= 0 else "?"
            key = (fn, off, base)
            rows[key] = rows.get(key, 0) + 1
    for (fn, off, base), n in sorted(rows.items()):
        print("%s  slot %-4s base `%s`  x%d" % (fn, off, base, n))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
