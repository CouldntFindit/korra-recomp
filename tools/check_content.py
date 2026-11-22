#!/usr/bin/env python3
"""Pre-commit guard: refuse to stage game content, generated code, or
machine-identifying paths.

Run by the hook in tools/install_hooks.ps1 (or directly):
  python tools/check_content.py --staged
  python tools/check_content.py --all
  python tools/check_content.py <file> [<file>...]

Rules: a tracked path is rejected when it is a game binary/asset, lives in a
generated-output tree, or is larger than MAX_BYTES (default 20 MiB) anywhere
outside docs/ images. File *contents* are rejected when they carry an absolute
user-profile path, a cloud-sync folder name, or the localized Documents folder,
so machine-specific locations cannot reach the public repository.
"""
from __future__ import annotations

import os
import re
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


# Machine-identifying content. The pieces are assembled from fragments so this
# file does not itself publish the strings it protects against. The placeholder
# forms the repository uses instead are accepted: <workspace>, <Documents>,
# %LOCALAPPDATA%, $env:LOCALAPPDATA%.
LEAK_PATTERNS = (
    (re.compile("[A-Za-z]:" + r"[\\/]+" + "Users" + r"[\\/]"),
     "absolute Windows user-profile path"),
    (re.compile(r"/(?:home|Users)/[A-Za-z0-9._-]+/"),
     "absolute POSIX home path"),
    (re.compile("One" + "Drive", re.I),
     "cloud-sync folder name"),
    (re.compile("Bel" + "geler", re.I),
     "localized Documents folder name"),
)

# Files that carry the patterns above on purpose (this guard and its selftest).
SKIP_CONTENT = ("tools/check_content.py", "tests/check_content_selftest.ps1")

CONTENT_BYTES = 4 * 1024 * 1024


def content_reason(path: str) -> str | None:
    if path.replace("\\", "/") in SKIP_CONTENT:
        return None
    try:
        with open(path, "rb") as fh:
            head = fh.read(8192)
            if b"\0" in head:
                return None  # binary
            fh.seek(0)
            text = fh.read(CONTENT_BYTES).decode("utf-8", "replace")
    except OSError:
        return None
    for rx, label in LEAK_PATTERNS:
        m = rx.search(text)
        if m:
            line = text.count("\n", 0, m.start()) + 1
            return "%s (line %d: %r)" % (label, line, m.group(0)[:40])
    return None


def tracked_files() -> list[str]:
    out = subprocess.run(
        ["git", "ls-files"], capture_output=True, text=True, check=True
    ).stdout
    return [p for p in out.splitlines() if p.strip()]


def main(argv: list[str]) -> int:
    if "--staged" in argv or len(argv) == 1:
        files = staged_files()
    elif "--all" in argv:
        files = tracked_files()
    else:
        files = [a for a in argv[1:] if not a.startswith("-")]
    if not files:
        return 0
    bad = [(f, r) for f in files for r in [why(f) or content_reason(f)] if r]
    if not bad:
        print("check_content: %d file(s) clean" % len(files))
        return 0
    print("check_content: content guard rejected the commit:")
    for f, r in bad:
        print("  %-60s %s" % (f, r))
    return 1


if __name__ == "__main__":
    sys.exit(main(sys.argv))
