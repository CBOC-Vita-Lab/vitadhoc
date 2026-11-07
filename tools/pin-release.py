#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 CBOC-Vita contributors
"""Pin a release VPK: copy it to the repository root as VitAdhoc.vpk and write
its hashes into cboc-release.json, docs/RELEASE.json, docs/SOURCE.md and the
package test.  Run again whenever the VPK is rebuilt, then run
tools/update-checksums.py.

    python3 tools/pin-release.py gpsp/vita/VitAdhoc.vpk 1.3 2026-11-07
"""
import hashlib
import json
import re
import shutil
import sys
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]

# Runtimes (eboot.bin) of earlier public releases: the installer may update
# any of them in place.
EARLIER_RUNTIMES = [
    "9069471e980c5b0616b48048dcad7c0446b3070ee63033bbcb1b528dd19bb9e8",  # 1.0
    "29a82a3716664c1616e404702fe46ca0460d59e8242a5dfcbca63f3a87b2a6c8",  # 1.1
    "3e107b77447ef26961de846a32b0cf90e9cafe55dcc56329efaa75424e0b4074",  # 1.2
]


def sha(data):
    return hashlib.sha256(data).hexdigest()


def sub_once(text, pattern, repl, what):
    new, n = re.subn(pattern, repl, text, count=1, flags=re.MULTILINE)
    if n != 1:
        sys.exit("pin-release: could not update %s" % what)
    return new


def main():
    if len(sys.argv) != 4:
        sys.exit(__doc__)
    src, version, date = Path(sys.argv[1]), sys.argv[2], sys.argv[3]
    if not re.fullmatch(r"\d+\.\d+", version) or not re.fullmatch(r"\d{4}-\d\d-\d\d", date):
        sys.exit("pin-release: version is X.Y, date is YYYY-MM-DD")
    vpk = ROOT / "VitAdhoc.vpk"
    if src.resolve() != vpk.resolve():
        shutil.copyfile(src, vpk)
    data = vpk.read_bytes()
    with zipfile.ZipFile(vpk) as z:
        runtime = sha(z.read("eboot.bin"))
        snes = sha(z.read("snes.bin")) if "snes.bin" in z.namelist() else None
    package = sha(data)

    # Text edits, not a JSON rewrite: the file keeps its hand-made layout.
    path = ROOT / "cboc-release.json"
    text = path.read_text()
    text = sub_once(text, r'"releaseId": "[^"]*"', '"releaseId": "vitadhoc-v%s"' % version, "releaseId")
    text = sub_once(text, r'"version": "[^"]*"', '"version": "%s"' % version, "version")
    text = sub_once(text, r'"sha256": "[0-9a-f]{64}"', '"sha256": "%s"' % package, "package sha256")
    text = sub_once(text, r'"runtimeSha256": "[0-9a-f]{64}"', '"runtimeSha256": "%s"' % runtime, "runtime")
    earlier = [h for h in EARLIER_RUNTIMES if h != runtime]
    block = '"appUpdateFrom": [' + ",".join('\n    "%s"' % h for h in earlier) + "\n  ]"
    text = sub_once(text, r'"appUpdateFrom": \[[^\]]*\]', block.replace("\\", "\\\\"), "appUpdateFrom")
    json.loads(text)
    path.write_text(text)

    path = ROOT / "docs/RELEASE.json"
    rel = json.loads(path.read_text())
    rel.update(version=version, date=date, tag="v" + version, title="VitAdhoc " + version)
    rel["package"]["sha256"] = package
    rel["package"]["runtimeSha256"] = runtime
    path.write_text(json.dumps(rel, indent=2, ensure_ascii=False) + "\n")

    path = ROOT / "docs/SOURCE.md"
    text = path.read_text()
    text = sub_once(text, r"^\| Version \| .* \|$", "| Version | %s |" % version, "SOURCE.md version")
    text = sub_once(text, r"^\| Package \| .* \|$",
                    "| Package | `VitAdhoc.vpk`, SHA-256 `%s` |" % package, "SOURCE.md package")
    text = sub_once(text, r"^\| Runtime \(`eboot.bin`\) \| .* \|$",
                    "| Runtime (`eboot.bin`) | SHA-256 `%s` |" % runtime, "SOURCE.md runtime")
    if snes:
        line = "| SNES player (`snes.bin`) | SHA-256 `%s` |" % snes
        if "| SNES player (`snes.bin`) |" in text:
            text = sub_once(text, r"^\| SNES player \(`snes.bin`\) \| .* \|$", line, "SOURCE.md snes")
        else:
            text = sub_once(text, r"^(\| Runtime \(`eboot.bin`\) \| .* \|)$", r"\1\n" + line, "SOURCE.md snes")
    text = sub_once(text, r"GIT_VERSION=\d+\.\d+`", "GIT_VERSION=%s`" % version, "SOURCE.md build command")
    path.write_text(text)

    path = ROOT / "gpsp/vita/tests/test_release_package.py"
    text = path.read_text()
    text = sub_once(text, r'^RUNTIME_SHA256 = "[0-9a-f]{64}"  # version .*$',
                    'RUNTIME_SHA256 = "%s"  # version %s' % (runtime, version), "package test pin")
    if snes:
        line = 'SNES_SHA256 = "%s"  # version %s' % (snes, version)
        if "SNES_SHA256 = " in text:
            text = sub_once(text, r'^SNES_SHA256 = "[0-9a-f]{64}"  # version .*$', line, "snes pin")
        else:
            text = sub_once(text, r'^(RUNTIME_SHA256 = .*)$', r"\1\n" + line, "snes pin")
    path.write_text(text)

    path = ROOT / ("release-notes-%s.md" % version)
    if path.is_file():
        text = path.read_text()
        text = sub_once(text, r"(SHA-256 `VitAdhoc.vpk`:\n`)[0-9a-f]{64}`", r"\g<1>%s`" % package,
                        "release notes hash")
        path.write_text(text)

    print("VitAdhoc %s: package %s, runtime %s%s" % (version, package, runtime,
          ", snes " + snes if snes else ""))
    print("Now run: python3 tools/update-checksums.py")


if __name__ == "__main__":
    main()
