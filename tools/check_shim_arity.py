#!/usr/bin/env python3
"""Check every host shim pops exactly the number of stdcall arguments its caller
pushed.

A shim ends with RET(value, argc) / RETV(argc) / RETP(ptr, argc) and advances
g_esp by 4 + 4*argc. Get one wrong and the guest's stack pointer walks into the
caller's frame; the failure then shows up as an unrelated crash deep in the
engine. It already happened once: GetStringTypeW is a 4-argument function but
the Android host declared it with 5, so it popped the caller's local and read a
pointer as a character count.

The reference implementation for arity is pcrecomp's runtime/win32hle, which
runs this same binary on Windows: every symbol both trees implement must agree.
Names only the Android host implements are listed separately for review, since
there is nothing to compare them against.

    python tools/check_shim_arity.py [--reference PATH] [--host PATH]
"""

from __future__ import annotations

import argparse
import pathlib
import re
import sys

HERE = pathlib.Path(__file__).resolve().parent
DEFAULT_HOST = HERE.parent / "src" / "host_android"
DEFAULT_REFERENCE = HERE.parent.parent / "pcrecomp" / "runtime" / "win32hle"

# ^<ret> <name>(void) { ... } at column 0. Shims are always (void).
FUNC_RE = re.compile(r"^(?:static\s+)?[A-Za-z_][\w\s\*]*?\b(\w+)\s*\(void\)\s*\{", re.M)
RET_RE = re.compile(r"\b(RET|RETV|RETP)\s*\((.*?)\)\s*;", re.S)
# { "Symbol", fn } or { "Symbol", &fn }
ENTRY_RE = re.compile(r'\{\s*"([^"]+)"\s*,\s*&?(\w+)\s*\}')


def function_bodies(src: str):
    """Yield (name, body) for each top-level `x(void) { ... }`."""
    for m in FUNC_RE.finditer(src):
        body_start = m.end()
        depth, i = 1, body_start
        while i < len(src) and depth:
            if src[i] == "{":
                depth += 1
            elif src[i] == "}":
                depth -= 1
            i += 1
        yield m.group(1), src[body_start:i]


def split_args(text: str) -> list[str]:
    out, depth, cur = [], 0, ""
    for ch in text:
        if ch == "(":
            depth += 1
        elif ch == ")":
            depth -= 1
        if ch == "," and depth == 0:
            out.append(cur.strip())
            cur = ""
        else:
            cur += ch
    if cur.strip():
        out.append(cur.strip())
    return out


def arity_of(body: str):
    """The argument count this body pops, from its RET family calls."""
    seen = set()
    for kind, args in RET_RE.findall(body):
        parts = split_args(args)
        if kind == "RETV":
            if parts:
                seen.add(int(parts[0], 0))
        elif len(parts) >= 2:
            try:
                seen.add(int(parts[1], 0))
            except ValueError:
                pass
    if not seen:
        return None, set()
    # A tail that RETs from more than one branch may legitimately repeat the
    # same count; differing counts are reported so they can be looked at.
    return min(seen), seen


def scan(directory: pathlib.Path):
    """{symbol: (argc, [callsite descriptions])} for every registry entry."""
    arity_by_fn: dict[str, tuple[int | None, set[int]]] = {}
    symbols: dict[str, list[tuple[str, str]]] = {}
    sources = sorted(list(directory.glob("*.c")) + list(directory.glob("*.cpp")) +
                     list(directory.glob("*.h")))
    for path in sources:
        src = path.read_text(encoding="utf-8", errors="ignore")
        for name, body in function_bodies(src):
            arity_by_fn[name] = arity_of(body)
        for sym, fn in ENTRY_RE.findall(src):
            symbols.setdefault(sym, []).append((fn, path.name))
    result = {}
    for sym, targets in symbols.items():
        for fn, filename in targets:
            if fn not in arity_by_fn:
                continue
            argc, seen = arity_by_fn[fn]
            if argc is None:
                continue
            result[sym] = (argc, seen, fn, filename)
    return result


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--host", type=pathlib.Path, default=DEFAULT_HOST)
    ap.add_argument("--reference", type=pathlib.Path, default=DEFAULT_REFERENCE)
    args = ap.parse_args()

    if not args.host.is_dir():
        print("host directory not found: %s" % args.host, file=sys.stderr)
        return 2
    host = scan(args.host)
    print("host shims with a known arity: %d" % len(host))

    problems = 0
    for sym, (argc, seen, fn, filename) in sorted(host.items()):
        if len(seen) > 1:
            print("INCONSISTENT %-38s %s in %s pops %s" %
                  (sym, fn, filename, sorted(seen)))
            problems += 1

    if args.reference.is_dir():
        ref = scan(args.reference)
        print("reference shims with a known arity: %d" % len(ref))
        compared = 0
        for sym, (argc, seen, fn, filename) in sorted(host.items()):
            if sym not in ref:
                continue
            compared += 1
            ref_argc = ref[sym][0]
            if ref_argc != argc:
                print("ARITY MISMATCH %-34s host %s pops %d, reference %s pops %d" %
                      (sym, fn, argc, ref[sym][2], ref_argc))
                problems += 1
        print("compared against the reference: %d symbols" % compared)
        only = sorted(set(host) - set(ref))
        if only:
            print("host-only (no reference arity to compare): %d" % len(only))
            print("  " + " ".join(only))
    else:
        print("reference tree not found (%s): comparison skipped" % args.reference)

    print()
    print("PROBLEMS: %d" % problems)
    return 1 if problems else 0


if __name__ == "__main__":
    raise SystemExit(main())
