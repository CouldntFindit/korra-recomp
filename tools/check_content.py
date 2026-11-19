#!/usr/bin/env python3
"""Pre-commit guard: refuse to stage game content or generated recompiled code.

Run by the hook in tools/install_hooks.ps1 (or directly):
  python tools/check_content.py --staged

Rules: a tracked path is rejected when it is a game binary/asset, lives in a
generated-output tree, or is larger than MAX_BYTES (default 20 MiB) anywhere
outside docs/ images.
"""
from __future__ import annotations

import os
import subprocess
import sys

MAX_BYTES = 20 * 1024 * 1024

BAD_DIRS = (
    "work/",
    "src/recomp/gen/",
    "game_files/",
    "logs/",
    "dumps/",
    "build/",
    "out/",
)

BAD_EXT = {
    ".exe", ".dll", ".cpk", ".usm", ".hca", ".adx", ".acb", ".awb", ".bnk",
    ".wai", ".wem", ".pck", ".sgb", ".mfx", ".ico", ".texture",
    ".obj", ".pdb", ".ilk", ".apk", ".so", ".a", ".o",
}

BAD_NAMES = {"lok.exe", "steam_api.dll", "steam_appid.txt"}


def staged_files() -> list[str]:
    out = subprocess.run(
        ["git", "diff", "--cached", "--name-only", "--diff-filter=ACMR"],
        capture_output=True, text=True, check=True
    ).stdout
    return [p for p in out.splitlines() if p.strip()]


def why(path: str) -> str | None:
    p = path.replace("\\", "/")
    low = p.lower()
    base = os.path.basename(low)
    if base in BAD_NAMES:
        return "game executable/runtime file"
    for d in BAD_DIRS:
        if low == d.rstrip("/") or low.startswith(d) or ("/" + d) in low:
            return "generated/private tree (%s)" % d
    ext = os.path.splitext(base)[1]
    if ext in BAD_EXT:
        return "game content or build artifact (%s)" % ext
    if low.startswith("gamedata/") or "/gamedata/" in low:
        return "game data directory"
    try:
        size = os.path.getsize(path)
    except OSError:
        return None
    if size > MAX_BYTES and low.startswith("docs/images/") is False:
        return "file larger than %d MiB" % (MAX_BYTES // (1024 * 1024))
    return None


def main(argv: list[str]) -> int:
    files = staged_files() if "--staged" in argv or len(argv) == 1 else [a for a in argv[1:] if not a.startswith("-")]
    if not files:
        return 0
    bad = [(f, r) for f in files for r in [why(f)] if r]
    if not bad:
        print("check_content: %d file(s) clean" % len(files))
        return 0
    print("check_content: content guard rejected the commit:")
    for f, r in bad:
        print("  %-60s %s" % (f, r))
    return 1


if __name__ == "__main__":
    sys.exit(main(sys.argv))
