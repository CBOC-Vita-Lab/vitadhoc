#!/usr/bin/env python3
"""Run real-code audio regressions; no ROM, device, or VPK is required."""
from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import shlex
import shutil
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
SOURCES = {
    "mixer": "tools/tests/test_audio_mixer_regressions.c",
    "output": "vita/tests/audio/test_audio_output.c",
    "lifecycle": "vita/tests/audio/test_audio_lifecycle.c",
    "host_lifecycle": "vita/tests/audio/test_audio_lifecycle.c",
    "timer": "tools/test_sound_timer_step.c",
    "safety": "tools/tests/test_gba_sound_safety.c",
}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--only", nargs="+", choices=tuple(SOURCES))
    parser.add_argument("--cc", default=os.environ.get("CC", "cc"))
    parser.add_argument("--sanitize", action="store_true")
    parser.add_argument("--vita-headers", type=Path,
                        help="VitaSDK arm-vita-eabi/include directory")
    parser.add_argument("--out", type=Path, help="retain binaries and native include")
    parser.add_argument("--json", type=Path, help="write a verification receipt")
    args = parser.parse_args()
    cc = shlex.split(args.cc)
    if not cc or shutil.which(cc[0]) is None:
        parser.error("a host C compiler is required (--cc or CC)")
    headers = args.vita_headers
    if headers is None and os.environ.get("VITASDK"):
        headers = Path(os.environ["VITASDK"]) / "arm-vita-eabi/include"

    selected = args.only or list(SOURCES)
    temporary = tempfile.TemporaryDirectory(prefix="vitadhoc-audio-")
    out = (args.out or Path(temporary.name)).resolve()
    out.mkdir(parents=True, exist_ok=True)
    results = []
    env = dict(os.environ)
    env.setdefault("UBSAN_OPTIONS", "halt_on_error=1")
    try:
        for name in selected:
            flags = ["-std=gnu11", "-O1", "-g", "-DINLINE=inline",
                     "-Werror=implicit-function-declaration",
                     "-ffunction-sections", "-fdata-sections", "-I", str(ROOT),
                     "-I", str(ROOT / "libretro/libretro-common/include")]
            if args.sanitize:
                flags += ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"]
            try:
                if name == "lifecycle":
                    if headers is None or not (headers / "psp2/audioout.h").is_file():
                        raise RuntimeError("lifecycle needs --vita-headers or VITASDK; "
                                           "missing headers are not a pass")
                    source = (ROOT / "vita/plat_vita.c").read_text()
                    start = "static int g_port = -1;"
                    end = "/* ---- where the threads run"
                    if source.count(start) != 1 or source.count(end) != 1:
                        raise RuntimeError("native audio-section markers changed; "
                                           "inspect before updating the test")
                    region = source[source.index(start):source.index(end)]
                    (out / "audio_lifecycle_native.inc").write_text(region)
                    flags += ["-I", str(out), "-idirafter", str(headers.resolve())]
                if name == "host_lifecycle":
                    source = (ROOT / "vita/hostcore/plat_host.c").read_text()
                    start = "static FILE    *wav_fp;"
                    end = "/* ---- the command line"
                    if source.count(start) != 1 or source.count(end) != 1:
                        raise RuntimeError("host audio-section markers changed; "
                                           "inspect before updating the test")
                    region = source[source.index(start):source.index(end)]
                    (out / "audio_lifecycle_host.inc").write_text(region)
                    flags += ["-DAUDIO_TEST_HOST", "-I", str(out), "-pthread"]
                binary = out / ("test_audio_" + name)
                linker = "-Wl,-dead_strip" if sys.platform == "darwin" else "-Wl,--gc-sections"
                command = cc + flags + [str(ROOT / SOURCES[name]), linker,
                                        "-lm", "-o", str(binary)]
                print("COMPILE " + shlex.join(command), flush=True)
                subprocess.run(command, check=True, env=env)
                print("RUN " + str(binary), flush=True)
                subprocess.run([str(binary)], check=True, env=env)
                results.append({"suite": name, "status": "pass"})
            except (OSError, RuntimeError, subprocess.CalledProcessError) as error:
                print("FAIL %s: %s" % (name, error), file=sys.stderr, flush=True)
                results.append({"suite": name, "status": "fail", "reason": str(error)})
    finally:
        if args.json:
            args.json.parent.mkdir(parents=True, exist_ok=True)
            args.json.write_text(json.dumps({"sanitized": args.sanitize,
                                            "suites": results}, indent=2) + "\n")
        temporary.cleanup()
    passed = sum(item["status"] == "pass" for item in results)
    print("Audio regressions: %d passed, %d failed, 0 skipped" %
          (passed, len(results) - passed))
    return int(passed != len(selected))


if __name__ == "__main__":
    sys.exit(main())
