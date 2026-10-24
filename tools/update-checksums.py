#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 CBOC-Vita contributors
"""Write SHA256SUMS: one line for every file Git tracks, SHA256SUMS itself
excepted.  Run from anywhere in the repository after changing files."""
import hashlib
import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def main():
    names = subprocess.run(["git", "ls-files", "-z"], cwd=ROOT, check=True,
                           capture_output=True).stdout.decode().split("\0")
    lines = []
    for name in sorted(n for n in names if n and n != "SHA256SUMS"):
        digest = hashlib.sha256((ROOT / name).read_bytes()).hexdigest()
        lines.append("%s  %s\n" % (digest, name))
    (ROOT / "SHA256SUMS").write_text("".join(lines))
    print("SHA256SUMS: %d files" % len(lines))


if __name__ == "__main__":
    main()
