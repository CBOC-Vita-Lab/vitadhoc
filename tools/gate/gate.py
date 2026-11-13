#!/usr/bin/env python3
"""gate.py -- the one test command for vitadhoc.

Run it from anywhere; every path is relative to the repository root.

  python3 tools/gate/gate.py                 default tier: quick steps plus the host app, cores and audio
  python3 tools/gate/gate.py --quick         the fast subset: unit tests, twin selftests, playerkit, widekit, save tests
  python3 tools/gate/gate.py --full          everything, incl. armhf under qemu and the Vita package build
  python3 tools/gate/gate.py --changed       only the steps whose tags match the paths changed since --base (HEAD)
  python3 tools/gate/gate.py --only a,b      these steps, with their prerequisites
  python3 tools/gate/gate.py --skip a,b      leave these steps out
  python3 tools/gate/gate.py --list          every step: tier, tags, prerequisites, whether it can run here
  python3 tools/gate/gate.py --dry-run       the plan: what would run, what is cached, what is skipped
  python3 tools/gate/gate.py --baseline F    compare with a saved summary.json; a check count that dropped or a
                                             failure count that rose is a regression (exit status 1)

Results per step:
  PASS     the command exited 0 and printed no failures (cached PASSes are marked "cached")
  FAIL     non-zero exit, a reported failure, or a timeout
  BLOCKED  a prerequisite FAILed or was BLOCKED, so the step did not run
  SKIP     a prerequisite is missing from this machine, or a private ROM folder is not set: never a failure
  INFO     informational (not gating): shown with its numbers, does not fail the run

Output (OUT = tools/gate/out, gitignored by the root **/out/ rule):
  OUT/logs/<step>.log   stdout and stderr of each step
  OUT/summary.json      results per step (also the format --baseline reads)
  OUT/summary.md        the table as markdown
  OUT/.gate-cache/      the PASS memory that makes repeated runs skip unchanged steps

Caching: a PASS is remembered under a key made of the step's command, the content of its input
directories (git-tracked and untracked, not ignored) and its prerequisites' keys, and of this file.
The next run skips the step while the key is unchanged and its declared outputs still exist.
Private ROM steps key on the folder path only, not on the ROM files; use --force after changing ROMs.

Steps run with `nice -n 10`, at most --jobs at a time (default: the number of cores). Steps that
share a resource (the host app's data roots and screenshot folder, make in gpsp/) form a group and
never overlap. Each step has a timeout; a step that exceeds it is killed and FAILs.
"""
from __future__ import annotations

import argparse
import datetime
import hashlib
import importlib.util
import json
import os
import re
import shutil
import signal
import subprocess
import sys
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO = Path(__file__).resolve().parents[2]
DEFAULT_OUT = HERE / "out"
CORES = os.cpu_count() or 2
TIERS = ("quick", "default", "full", "release")
FAILING = ("FAIL", "BLOCKED")

# ---- toolchains -------------------------------------------------------------
VITASDK = os.environ.get("VITASDK", "/opt/vitasdk")
_RGBDS_DIR = (os.environ.get("GATE_RGBDS") or os.environ.get("RGBDS")
              or ("/opt/rgbds/bin" if os.path.exists("/opt/rgbds/bin/rgbasm") else ""))
RGBDS = (_RGBDS_DIR.rstrip("/") + "/") if _RGBDS_DIR else ""

# ---- --changed: which tags a changed path selects ------------------------------
# The longest matching prefix decides. A step runs when one of its tags is selected; "*" selects all.
# A top-level file with no row is release material (SHA256SUMS lists every shipped file).
# Any other unmatched path selects everything, which is the safe answer.
CHANGE_MAP = [
    ("tools/gate/", {"*"}),
    ("tools/", {"release"}),
    ("tests/", {"release"}),
    ("docs/", {"release"}),
    ("licenses/", {"release"}),
    ("notices/", {"release"}),
    ("store/", {"release"}),
    ("SHA256SUMS", {"release"}),
    ("VitAdhoc.vpk", {"release", "vita"}),
    ("gpsp/", {"gpsp", "vita"}),                       # core and app sources: every gpsp step
    ("gpsp/vita/tests/savesync/", {"save"}),
    ("gpsp/vita/tests/localsave/", {"save"}),
    ("gpsp/vita/tests/artfetch/", {"cover"}),
    ("gpsp/vita/tests/upscale/", {"cover"}),
    ("gpsp/vita/tests/gamedb/", {"gamedb"}),
    ("gpsp/vita/tests/library/", {"library"}),
    ("gpsp/vita/tests/cheats/", {"cheats"}),
    ("gpsp/vita/tests/rewind/", {"app", "rewind"}),
    ("gpsp/vita/tests/test_release_package.py", {"release"}),
    ("gpsp/tools/run_audio_regressions.py", {"audio"}),
    ("gpsp/tools/run_host_tests.py", {"unit"}),
    ("nes/", {"nes", "vita"}),                         # the NES core is built into the app
    ("snes/", {"snes", "vita"}),
    ("snes/frontend/third_party/", {"snes", "playerkit", "widekit", "vita"}),
    ("wswan/", {"wswan", "vita"}),
    ("playerkit/", {"playerkit", "vita"}),
    ("widekit/", {"widekit", "vita"}),
]

# ---- the steps ---------------------------------------------------------------
# cmd placeholders: {out} {repo} {jobs} {rgbds} {vitasdk} {gitver}
# requires: ("exe", name) on PATH or absolute, ("path", p), ("py", module)
# parse: the key of PARSERS below; "exit" uses the exit status only
GROUP_MAKE = "gpsp-make"     # make inside gpsp/ (shared object and build directories)
GROUP_HOST = "hostcore-run"  # host app runs: shared data roots, screenshot folder, loopback ports
GROUP_TMP = "tmp-fixed"      # run_host_tests.py compiles into fixed /tmp names

APP_TAGS = {"gpsp", "app", "host", "nes"}   # the app and what it links; NES is built into it
ARMHF = [("exe", "arm-linux-gnueabihf-gcc"), ("exe", "qemu-arm")]
ARM_NONE = [("exe", "arm-none-eabi-gcc")]
RGBASM = [("exe", RGBDS + "rgbasm")]
VITA_GCC = [("path", VITASDK + "/bin/arm-vita-eabi-gcc")]
VITA_GPP = [("path", VITASDK + "/bin/arm-vita-eabi-g++")]
CC = [("exe", "cc")]
CXX = [("exe", "g++")]
PIL = [("py", "PIL")]


class Step:
    def __init__(self, name, desc, cmd, tier, tags, inputs=(), deps=(), cwd=".", outputs=(),
                 requires=(), env_req=None, parse="exit", timeout=1800, group=None, gating=True):
        assert tier in TIERS, (name, tier)
        self.name, self.desc, self.cmd, self.tier = name, desc, cmd, tier
        self.tags = frozenset(tags)
        self.inputs, self.deps, self.cwd = list(inputs), list(deps), cwd
        self.outputs, self.requires, self.env_req = list(outputs), list(requires), env_req
        self.parse, self.timeout, self.group, self.gating = parse, timeout, group, gating


STEPS = [
    # ---- prerequisites: built once here, shared by every step that needs them ----
    Step("build-snes-twin", "SNES twin: headless runner (snes/twin)",
         "make -C snes/twin -j{jobs}", "quick", {"snes"}, inputs=["snes/"], requires=CXX,
         outputs=["snes/twin/build/snes_twin"], timeout=3600),
    Step("build-nes-twin", "NES twin: headless runner (nes/twin)",
         "make -C nes/twin -j{jobs}", "quick", {"nes"}, inputs=["nes/"], requires=CXX + CC,
         outputs=["nes/twin/build/nes_twin"], timeout=3600),
    Step("build-wswan-twin", "WonderSwan twin and blend_test (wswan/twin)",
         "make -C wswan/twin -j{jobs}", "quick", {"wswan"}, inputs=["wswan/"], requires=CXX + CC,
         outputs=["wswan/twin/build/wswan_twin", "wswan/twin/build/blend_test"], timeout=3600),
    Step("build-hostcore-x86", "the Vita app for Linux, x86 dynarec (hostcore ARCH=x86)",
         "make -C gpsp/vita/hostcore ARCH=x86 -j{jobs}", "default", APP_TAGS,
         inputs=["gpsp/", "nes/"], requires=CC, group=GROUP_MAKE,
         outputs=["gpsp/vita/hostcore/build/x86/gbadhoc_host"], timeout=7200),
    Step("build-hostcore-armhf", "the Vita app for armhf, run under qemu (ARM dynarec)",
         "make -C gpsp/vita/hostcore ARCH=armhf -j{jobs}", "full", APP_TAGS,
         inputs=["gpsp/", "nes/"], requires=ARMHF + [("exe", "arm-linux-gnueabihf-gcc")],
         group=GROUP_MAKE, outputs=["gpsp/vita/hostcore/build/armhf/gbadhoc_host"], timeout=7200),
    Step("build-testrom", "GBA test cartridge gbadhoc-test.gba (hostcore/testrom)",
         "make -C gpsp/vita/hostcore/testrom gbadhoc-test.gba", "default", APP_TAGS,
         inputs=["gpsp/"], requires=ARM_NONE, group=GROUP_MAKE,
         outputs=["gpsp/vita/hostcore/testrom/gbadhoc-test.gba"]),
    Step("build-testrom-gb", "GB and GBC link and save cartridges (needs RGBDS)",
         "make -C gpsp/vita/hostcore/testrom gb RGBDS={rgbds}", "default", APP_TAGS,
         inputs=["gpsp/"], requires=RGBASM, group=GROUP_MAKE,
         outputs=["gpsp/vita/hostcore/testrom/gblocal.gb", "gpsp/vita/hostcore/testrom/gblink.gb",
                  "gpsp/vita/hostcore/testrom/gbsave.gb"]),
    Step("build-cheatrom", "cheats cartridge cheatrom.gba (tests/cheats/rom)",
         "make -C gpsp/vita/tests/cheats/rom cheatrom.gba", "default", APP_TAGS | {"cheats"},
         inputs=["gpsp/"], requires=ARM_NONE, group=GROUP_MAKE,
         outputs=["gpsp/vita/tests/cheats/rom/cheatrom.gba"]),
    Step("build-rewind-rom", "rewind cartridges (tests/rewind/rom, needs RGBDS)",
         "make -C gpsp/vita/tests/rewind/rom all RGBDS={rgbds}", "default", APP_TAGS,
         inputs=["gpsp/"], requires=ARM_NONE + RGBASM, group=GROUP_MAKE,
         outputs=["gpsp/vita/tests/rewind/rom/rwbusy.gba"]),
    Step("build-linktest-rom", "link-cable test ROM linktest.gba (tools/linktest)",
         "make -C gpsp/tools/linktest", "default", APP_TAGS,
         inputs=["gpsp/"], requires=ARM_NONE, group=GROUP_MAKE,
         outputs=["gpsp/tools/linktest/linktest.gba"]),

    # ---- quick: unit tests, twin selftests, playerkit, widekit, save tests ----
    Step("release-unit", "download checker and release contracts (tests/, python unittest)",
         "python3 -m unittest discover -s tests", "quick", {"release"}, inputs=["."], parse="unittest"),
    # Development: the package test checks the VPK built here (gpsp/vita/VitAdhoc.vpk,
    # `make vpk`) when there is one, without the release hash pins; it skips the
    # package checks when nothing has been built.
    Step("release-package", "VitAdhoc.vpk (fresh build if any): no BIOS or games inside, notices",
         "python3 -m unittest discover -s gpsp/vita/tests -p test_release_package.py", "quick",
         {"release"}, inputs=["."], parse="unittest"),
    # Release time only (--release): the committed root VitAdhoc.vpk with its pinned
    # runtime hashes, and SHA256SUMS against the tree. Both are stale by design
    # between releases (tools/pin-release.py refreshes them).
    Step("release-package-pinned", "the committed VitAdhoc.vpk: notices, pinned eboot.bin / snes.bin hashes",
         "RELEASE_VPK=VitAdhoc.vpk python3 -m unittest discover -s gpsp/vita/tests -p test_release_package.py",
         "release", {"release"}, inputs=["."], parse="unittest"),
    Step("check-files", "SHA256SUMS: every listed file matches (tools/check-files.py)",
         "python3 tools/check-files.py --directory .", "release", {"release"}, inputs=["."],
         parse="check_files"),
    Step("gamedb", "title database: gamedb.c against assets/gamedb.txt and make_gamedb.py",
         "make -C gpsp/vita/tests/gamedb test", "quick", {"gpsp", "gamedb"}, inputs=["gpsp/"],
         requires=CC, group=GROUP_MAKE),
    Step("save-sync", "save sync: RetroArch edits, short sources, backups (test_save_sync.c)",
         "mkdir -p {out}/bin && gcc -Wall -Wextra -Werror -I vita -I frontend-common "
         "vita/tests/savesync/test_save_sync.c vita/save_sync.c frontend-common/fe_util.c "
         "-o {out}/bin/save-sync && {out}/bin/save-sync",
         "quick", {"gpsp", "save"}, inputs=["gpsp/"], cwd="gpsp", requires=CC),
    Step("save-backup-failure", "save sync: backups that cannot be made leave the save untouched",
         "mkdir -p {out}/bin && gcc -Wall -Wextra -Werror -I vita -I frontend-common "
         "vita/tests/savesync/test_backup_failure.c vita/save_sync.c frontend-common/fe_util.c "
         "-o {out}/bin/save-backup-failure && {out}/bin/save-backup-failure",
         "quick", {"gpsp", "save"}, inputs=["gpsp/"], cwd="gpsp", requires=CC),
    Step("save-legacy-heal", "save sync: every legacy save layout gets its plain RetroArch file, never over another save",
         "mkdir -p {out}/bin && gcc -Wall -Wextra -Werror -I vita -I frontend-common "
         "vita/tests/savesync/test_legacy_heal.c vita/save_sync.c frontend-common/fe_util.c "
         "-o {out}/bin/save-legacy-heal && {out}/bin/save-legacy-heal",
         "quick", {"gpsp", "save"}, inputs=["gpsp/"], cwd="gpsp", requires=CC),
    Step("local-save-fail", "local link: a second game's save is never lost (save_write)",
         "mkdir -p {out}/bin && gcc -Wall -Wextra -Werror -no-pie -DVITA -I vita -I psp "
         "-I frontend-common -I libretro/libretro-common/include -I . "
         "vita/tests/localsave/test_local_save_fail.c frontend-common/fe_util.c "
         "-Wl,--unresolved-symbols=ignore-all -o {out}/bin/local-save-fail && {out}/bin/local-save-fail",
         "quick", {"gpsp", "save"}, inputs=["gpsp/"], cwd="gpsp", requires=CC),
    Step("play-log", "play log: the ring of recent games and its file (test_play_log.c)",
         "mkdir -p {out}/bin && gcc -Wall -Wextra -I vita -I frontend-common "
         "vita/tests/playlog/test_play_log.c vita/play_log.c -o {out}/bin/play-log && {out}/bin/play-log",
         "quick", {"gpsp", "save"}, inputs=["gpsp/"], cwd="gpsp", requires=CC),
    Step("playerkit", "playerkit: unit tests and menu walk, ASan and UBSan",
         "make -C playerkit test", "quick", {"playerkit"},
         inputs=["playerkit/", "snes/frontend/third_party/"], requires=CXX, parse="playerkit",
         timeout=3600),
    Step("widekit", "widekit: tests, ASan and UBSan",
         "make -C widekit test", "quick", {"widekit"}, inputs=["widekit/"], requires=CXX,
         parse="widekit", timeout=3600),
    Step("snes-selftest", "SNES host glue: determinism, states, SRAM, copier header, pacing",
         "SNES_ROMS=\"$GATE_SNES_ROMS\" sh snes/twin/selftest.sh", "quick", {"snes"},
         inputs=["snes/"], deps=["build-snes-twin"], parse="pass_fail"),
    Step("nes-selftest", "NES host glue: determinism, states, battery RAM, headers, mappers",
         "sh nes/twin/selftest.sh", "quick", {"nes"}, inputs=["nes/"],
         deps=["build-nes-twin"], parse="pass_fail"),
    Step("wswan-selftest", "WonderSwan host glue: determinism, states, SRAM and EEPROM, orientation",
         "sh wswan/twin/selftest.sh", "quick", {"wswan"}, inputs=["wswan/"],
         deps=["build-wswan-twin"], parse="pass_fail"),

    # ---- default: the host app, cores, audio, cover art, cheats, library ----
    Step("unit-host", "host unit suites: ~40 C suites under ASan and UBSan (run_host_tests.py)",
         "python3 gpsp/tools/run_host_tests.py --json {out}/host-tests.json", "default",
         {"gpsp", "unit"}, inputs=["gpsp/", "nes/"], requires=CC, parse="host", timeout=7200,
         group=GROUP_TMP),
    Step("rewind-unit-x86", "rewind unit: a machine whose every moment is known, ASan, UBSan, TSan (x86)",
         "make -C gpsp/vita/tests/rewind build/unit_san build/unit_tsan && cd gpsp/vita/tests/rewind && "
         "for s in 1 2 3; do build/unit_san $s 200000 0 && build/unit_san $s 200000 1 && "
         "build/unit_tsan $s 60000 1 || exit 1; done",
         "default", {"gpsp", "app", "rewind"}, inputs=["gpsp/"], requires=CC, group=GROUP_MAKE),
    # the census: every default-on 1.4 feature proves it acted in a twin run
    # (steady pacing, NES wide view, jit counters, cover upload budget, save
    # backup); quick, so it runs in --quick too
    Step("census", "default-on features act (app_tests.py --only census)",
         "python3 app_tests.py x86 --only census", "quick", APP_TAGS | {"app"}, cwd="gpsp/vita/hostcore",
         inputs=["gpsp/", "nes/"], deps=["build-hostcore-x86", "build-testrom"],
         requires=PIL, group=GROUP_HOST, parse="app", timeout=1800),
    Step("app-x86", "the whole app on x86: game list, menus, states, fast forward (app_tests.py)",
         "python3 app_tests.py x86", "default", APP_TAGS | {"app"}, cwd="gpsp/vita/hostcore",
         inputs=["gpsp/", "nes/"], deps=["build-hostcore-x86", "build-testrom", "build-cheatrom"],
         requires=PIL, group=GROUP_HOST, parse="app", timeout=7200),
    Step("rewind-x86", "rewind in the app, quick pass (rewind_tests.py --quick)",
         "python3 rewind_tests.py x86 --quick", "default", APP_TAGS | {"app"},
         cwd="gpsp/vita/hostcore", inputs=["gpsp/", "nes/"],
         deps=["build-hostcore-x86", "build-testrom", "build-rewind-rom"],
         requires=PIL, group=GROUP_HOST, parse="app", timeout=7200),
    Step("local-x86", "local link: two players on one console (local_tests.py)",
         "python3 local_tests.py x86", "default", APP_TAGS | {"app"}, cwd="gpsp/vita/hostcore",
         inputs=["gpsp/", "nes/"],
         deps=["build-hostcore-x86", "build-testrom", "build-testrom-gb", "build-cheatrom"],
         requires=PIL, group=GROUP_HOST, parse="app", timeout=7200),
    Step("link-x86", "two app twins linked over loopback, menus to session (link_tests.py)",
         "python3 link_tests.py x86 --hz 20", "default", APP_TAGS | {"app"},
         cwd="gpsp/vita/hostcore", inputs=["gpsp/", "nes/"],
         deps=["build-hostcore-x86", "build-testrom", "build-testrom-gb", "build-linktest-rom"],
         requires=PIL, group=GROUP_HOST, parse="app", timeout=7200),
    # a busy console is not a gone one: 20 s peer-dead limit, keepalives sent by
    # the transport's I/O thread (netdrv test_lan stall12/peerdead, and the real
    # link session stalled for 12 s / its partner killed, sesslan)
    Step("link-liveness", "link liveness: 12 s stall kept, killed peer ends in ~20 s (test_lan, sesslan)",
         "make -C gpsp/netdrv test_netdrv test-lan LAN_TESTS=\"stall12 peerdead\" && ./gpsp/netdrv/test_netdrv >/dev/null && "
         "python3 gpsp/netdrv/tests/run_sesslan.py {out}/sesslan-live stall kill",
         "default", {"gpsp", "link"}, inputs=["gpsp/netdrv/", "gpsp/frontend-common/", "gpsp/gbcore/", "gpsp/tools/gblink/"],
         requires=CC, group=GROUP_MAKE, timeout=1800),
    Step("link-liveness-long", "link liveness: a 25 s stall rides on the I/O keepalive, ends without it (test_lan stall25)",
         "make -C gpsp/netdrv test-lan LAN_TESTS=stall25",
         "full", {"gpsp", "link"}, inputs=["gpsp/netdrv/"], requires=CC, group=GROUP_MAKE, timeout=1800),
    Step("audio-regressions", "audio: mixer, output, lifecycle, timer, safety, sim, GB audio",
         "python3 gpsp/tools/run_audio_regressions.py --vita-headers {vitasdk}/arm-vita-eabi/include",
         "default", {"gpsp", "audio"}, inputs=["gpsp/"], requires=CC, parse="audio"),
    Step("artfetch", "cover art fetch: worker, HTTP parser, paused batches (test_artfetch.py)",
         "python3 gpsp/vita/tests/artfetch/test_artfetch.py", "default", {"gpsp", "cover"},
         inputs=["gpsp/"], requires=CC, parse="unittest"),
    Step("upscale", "cover upscalers: xBRZ and the Vita path (upscale_test.c)",
         "make -C gpsp/vita/tests/upscale upscale_test && cd gpsp/vita/tests/upscale && ./upscale_test; "
         "rc=$?; make -C gpsp/vita/tests/upscale clean >/dev/null 2>&1; exit $rc",
         "default", {"gpsp", "cover"}, inputs=["gpsp/"], requires=CC + CXX, group=GROUP_MAKE),
    Step("cheats-unit-x86", "cheats: decoders, ciphers, mGBA suite, .cht database (x86)",
         "make -C gpsp/vita/tests/cheats ARCH=x86 RGBDS={rgbds} unit", "default",
         {"gpsp", "cheats"}, inputs=["gpsp/"], requires=CC, group=GROUP_MAKE),
    Step("library-x86", "library screen: layout and state (library_test.c, x86)",
         "make -C gpsp/vita/hostcore ARCH=x86 -f Makefile -f ../tests/library/library.mk library-test",
         "default", {"gpsp", "library", "app"}, inputs=["gpsp/"], deps=["build-hostcore-x86"],
         requires=CC, group=GROUP_MAKE),
    Step("wswan-private", "WonderSwan selftest plus a private ROM (GATE_WS_ROM)",
         "WSWAN_PRIVATE_ROM=\"$GATE_WS_ROM\" sh wswan/twin/selftest.sh", "default", {"wswan"},
         inputs=["wswan/"], deps=["build-wswan-twin"], env_req="GATE_WS_ROM", parse="pass_fail"),
    Step("snes-mt-compare", "SNES: every private ROM with threads off and on (GATE_SNES_ROMS)",
         "python3 snes/twin/mt_compare.py --roms \"$GATE_SNES_ROMS\"", "default", {"snes"},
         inputs=["snes/"], deps=["build-snes-twin"], env_req="GATE_SNES_ROMS", parse="snes_mt",
         timeout=7200),
    Step("snes-conformance", "SNES: private ROMs against their reference screenshots (GATE_SNES_ROMS)",
         "python3 snes/twin/conformance.py \"$GATE_SNES_ROMS\"", "default", {"snes"},
         inputs=["snes/"], deps=["build-snes-twin"], env_req="GATE_SNES_ROMS",
         parse="snes_conf", gating=False, timeout=7200),
    Step("nes-conformance", "NES: private blargg-style ROMs against conformance_expect.txt (GATE_NES_ROMS)",
         "NES_TESTROMS=\"$GATE_NES_ROMS\" python3 conformance.py --expect conformance_expect.txt",
         "default", {"nes"}, cwd="nes/twin", inputs=["nes/"], deps=["build-nes-twin"],
         env_req="GATE_NES_ROMS", parse="nes_conf", timeout=7200),

    # ---- full: armhf under qemu, the Vita build, private cores runs, long tests ----
    Step("rewind-x86-long", "rewind, full: 20 minutes of play a console (rewind_tests.py x86)",
         "python3 rewind_tests.py x86", "full", APP_TAGS | {"app"}, cwd="gpsp/vita/hostcore",
         inputs=["gpsp/", "nes/"], deps=["build-hostcore-x86", "build-testrom", "build-rewind-rom"],
         requires=PIL, group=GROUP_HOST, parse="app", timeout=14400),
    Step("cores-own-rom", "cores: own test ROM, x86 against armhf, and the link bench (run_tests.sh)",
         "sh gpsp/vita/hostcore/run_tests.sh", "full", APP_TAGS | {"app"}, inputs=["gpsp/", "nes/"],
         deps=["build-hostcore-x86", "build-hostcore-armhf", "build-testrom"], requires=ARMHF,
         group=GROUP_HOST, timeout=7200),
    # instance B's image bounds and boot copies under ASan/UBSan (run_tests.sh
    # calls it at its end too; here it runs on its own, in every tier but quick)
    Step("gbadual-image", "instance B image bounds and boot copies (run_gbadual_image_test.sh)",
         "bash gpsp/tools/tests/run_gbadual_image_test.sh", "default", {"gpsp", "app"},
         inputs=["gpsp/"], requires=CC, group=GROUP_MAKE),
    # the fast GBA picture kernels against the scalar reference build, on
    # the private games (frame, audio and save hashes; video_identity.py)
    Step("video-identity", "GBA renderer: vector kernels vs -DVIDEO_KERNELS_SCALAR on private games (GATE_GBA_ROMS)",
         "make -C gpsp/vita/hostcore ARCH=x86 -j{jobs} B=build/x86-scalar "
         "CORE_ARCH_FLAGS='-fomit-frame-pointer -ffast-math -DVIDEO_KERNELS_SCALAR' && "
         "python3 gpsp/tools/tests/video_identity.py --a gpsp/vita/hostcore/build/x86/gbadhoc_host "
         "--b gpsp/vita/hostcore/build/x86-scalar/gbadhoc_host --rom-dir \"$GATE_GBA_ROMS\"",
         "full", APP_TAGS | {"app"}, inputs=["gpsp/"], deps=["build-hostcore-x86"], requires=PIL,
         env_req="GATE_GBA_ROMS", group=GROUP_MAKE, timeout=14400),
    Step("cores-private-gba", "cores: every private GBA/GB/GBC ROM, x86 against armhf (GATE_GBA_ROMS)",
         "sh gpsp/vita/hostcore/run_tests.sh \"$GATE_GBA_ROMS\"", "full", APP_TAGS | {"app"},
         inputs=["gpsp/", "nes/"], deps=["build-hostcore-x86", "build-hostcore-armhf", "build-testrom"],
         requires=ARMHF, env_req="GATE_GBA_ROMS", group=GROUP_HOST, timeout=14400),
    Step("app-armhf", "the whole app on armhf under qemu, the ARM dynarec (app_tests.py)",
         "python3 app_tests.py armhf", "full", APP_TAGS | {"app"}, cwd="gpsp/vita/hostcore",
         inputs=["gpsp/", "nes/"], deps=["build-hostcore-armhf", "build-testrom", "build-cheatrom"],
         requires=PIL + ARMHF, group=GROUP_HOST, parse="app", timeout=14400),
    Step("rewind-armhf", "rewind on armhf under qemu (rewind_tests.py armhf --quick)",
         "python3 rewind_tests.py armhf --quick", "full", APP_TAGS | {"app"},
         cwd="gpsp/vita/hostcore", inputs=["gpsp/", "nes/"],
         deps=["build-hostcore-armhf", "build-testrom", "build-rewind-rom"],
         requires=PIL + ARMHF, group=GROUP_HOST, parse="app", timeout=14400),
    Step("local-armhf", "local link on armhf under qemu (local_tests.py armhf)",
         "python3 local_tests.py armhf", "full", APP_TAGS | {"app"}, cwd="gpsp/vita/hostcore",
         inputs=["gpsp/", "nes/"],
         deps=["build-hostcore-armhf", "build-testrom", "build-testrom-gb", "build-cheatrom"],
         requires=PIL + ARMHF, group=GROUP_HOST, parse="app", timeout=14400),
    Step("link-armhf", "two app twins linked over loopback, armhf under qemu (link_tests.py armhf)",
         "python3 link_tests.py armhf --hz 20", "full", APP_TAGS | {"app"},
         cwd="gpsp/vita/hostcore", inputs=["gpsp/", "nes/"],
         deps=["build-hostcore-armhf", "build-testrom", "build-testrom-gb", "build-linktest-rom"],
         requires=PIL + ARMHF, group=GROUP_HOST, parse="app", timeout=14400),
    Step("rewind-unit-armhf", "rewind unit on armhf under qemu (the ARM dynarec's rewind code)",
         "make -C gpsp/vita/tests/rewind build/unit_armhf && cd gpsp/vita/tests/rewind && "
         "for s in 1 2 3; do qemu-arm -L /usr/arm-linux-gnueabihf build/unit_armhf $s 120000 0 && "
         "qemu-arm -L /usr/arm-linux-gnueabihf build/unit_armhf $s 120000 1 || exit 1; done",
         "full", {"gpsp", "app", "rewind"}, inputs=["gpsp/"],
         requires=ARMHF + [("exe", "arm-linux-gnueabihf-gcc")], group=GROUP_MAKE),
    Step("cheats-unit-armhf", "cheats unit tests, armhf under qemu",
         "make -C gpsp/vita/tests/cheats ARCH=armhf RGBDS={rgbds} unit", "full",
         {"gpsp", "cheats"}, inputs=["gpsp/"], requires=ARMHF + [("exe", "arm-linux-gnueabihf-gcc")],
         group=GROUP_MAKE),
    Step("cheats-e2e-x86", "cheats end to end: both cores with codes on, x86 (builds both cores)",
         "make -C gpsp/vita/tests/cheats ARCH=x86 e2e", "full", {"gpsp", "cheats"},
         inputs=["gpsp/", "nes/"], deps=["build-cheatrom"], requires=CC, group=GROUP_MAKE,
         timeout=14400),
    Step("cheats-e2e-armhf", "cheats end to end, armhf under qemu",
         "make -C gpsp/vita/tests/cheats ARCH=armhf e2e", "full", {"gpsp", "cheats"},
         inputs=["gpsp/", "nes/"], deps=["build-cheatrom"],
         requires=ARMHF + [("exe", "arm-linux-gnueabihf-gcc")], group=GROUP_MAKE, timeout=14400),
    Step("nes-arm-check", "NES core: host and armhf give byte-identical results (arm_check.sh)",
         "sh nes/twin/arm_check.sh", "full", {"nes"}, inputs=["nes/"],
         requires=ARMHF + [("exe", "arm-linux-gnueabihf-gcc")], parse="nes_arm", timeout=7200),
    Step("widekit-arm", "widekit: NEON and scalar agree under qemu (make arm-check)",
         "make -C widekit arm-check", "full", {"widekit"}, inputs=["widekit/"],
         requires=[("exe", "arm-linux-gnueabihf-g++"), ("exe", "qemu-arm")], timeout=3600),
    Step("widekit-vita", "widekit: compiles for the Vita, warning free (compile only)",
         "make -C widekit vita", "full", {"widekit", "vita"}, inputs=["widekit/"],
         requires=VITA_GPP, timeout=3600),
    Step("playerkit-vita", "playerkit: compiles for the Vita (compile only)",
         "make -C playerkit vita", "full", {"playerkit", "vita"},
         inputs=["playerkit/", "snes/frontend/third_party/"], requires=VITA_GPP, timeout=3600),
    Step("vita-build", "the Vita package: make vpk (BIOS empty, GIT_VERSION from git describe)",
         "make -C gpsp/vita -j{jobs} vpk BIOS= VITASDK={vitasdk} GIT_VERSION={gitver}", "full",
         {"gpsp", "vita"},
         inputs=["gpsp/", "nes/", "snes/", "wswan/", "playerkit/", "widekit/", "licenses/", "notices/"],
         requires=VITA_GCC, group=GROUP_MAKE, timeout=14400),
]
BY_NAME = {s.name: s for s in STEPS}


def _validate():
    for s in STEPS:
        for d in s.deps:
            assert d in BY_NAME, (s.name, d)
    assert len(BY_NAME) == len(STEPS), "duplicate step name"


_validate()


# ---- parsers: each suite's own totals ------------------------------------------
def _sum_all(pattern, text):
    return re.findall(pattern, text, re.M)


def _last(pattern, text):
    found = list(re.finditer(pattern, text, re.M))
    return found[-1] if found else None


def parse_app(text):
    # "x86: 12 checks, 0 failed" (app_tests, rewind_tests, local_tests and link_tests)
    found = _sum_all(r"^[A-Za-z0-9_.-]+: (\d+) checks, (\d+) failed\s*$", text)
    if not found:
        return {}
    return {"checks": sum(int(c) for c, _ in found), "failed": sum(int(f) for _, f in found)}


def parse_host(text):
    m = _last(r"^(\d+) passed, (\d+) failed, (\d+) skipped", text)
    if not m:
        return {}
    p, f, s = (int(x) for x in m.groups())
    return {"checks": p + f, "failed": f, "skipped": s}


def parse_playerkit(text):
    m = _last(r"^(\d+) tests, (\d+) failed, (\d+) failed checks", text)
    if not m:
        return {}
    menu = _last(r"^menu_walk: \d+ screenshots in .*, (\d+) failed checks", text)
    tests, _, bad = (int(x) for x in m.groups())
    return {"checks": tests, "failed": bad + (int(menu.group(1)) if menu else 0)}


def parse_widekit(text):
    m = _last(r"^(\d+) checks, (\d+) failed\s*$", text)
    return {"checks": int(m.group(1)), "failed": int(m.group(2))} if m else {}


def parse_unittest(text):
    ran = _last(r"^Ran (\d+) tests?", text)
    if not ran:
        return {}
    out = {"checks": int(ran.group(1)), "failed": 0, "skipped": 0}
    st = _last(r"^FAILED \(([^)]*)\)", text)
    if st:
        for key in ("failures", "errors"):
            m = re.search(key + r"=(\d+)", st.group(1))
            out["failed"] += int(m.group(1)) if m else 0
    sk = _last(r"skipped=(\d+)", text)
    if sk:
        out["skipped"] = int(sk.group(1))
    return out


def parse_pass_fail(text):
    p = len(re.findall(r"^PASS\s", text, re.M))
    f = len(re.findall(r"^FAIL\s", text, re.M))
    s = len(re.findall(r"^SKIP\s", text, re.M))
    if not (p or f or s):
        return {}
    return {"checks": p + f, "failed": f, "skipped": s}


def parse_check_files(text):
    m = _last(r"^PASS: (\d+) file", text)
    if m:
        return {"checks": int(m.group(1)), "failed": 0}
    return {"failed": len(re.findall(r"^- ", text, re.M)) or 1}


def parse_audio(text):
    m = _last(r"Audio regressions: (\d+) passed, (\d+) failed, (\d+) skipped", text)
    if not m:
        return {}
    p, f, s = (int(x) for x in m.groups())
    return {"checks": p + f, "failed": f, "skipped": s}


def parse_snes_mt(text):
    m = _last(r"^(\d+) identical, (\d+) different, (\d+) skipped", text)
    if not m:
        return {}
    same, diff, skip = (int(x) for x in m.groups())
    return {"checks": same + diff, "failed": diff, "skipped": skip}


def parse_snes_conf(text):
    m = _last(r"^(\d+) of (\d+) match the reference", text)
    if not m:
        return {}
    same, total = int(m.group(1)), int(m.group(2))
    return {"checks": total, "failed": total - same}


def parse_nes_conf(text):
    m = _last(r"^expected passes: (\d+), regressions: (\d+)", text)
    return {"checks": int(m.group(1)), "failed": int(m.group(2))} if m else {}


def parse_nes_arm(text):
    m = _last(r"identical (\d+), different (\d+)", text)
    if not m:
        return {}
    same, diff = int(m.group(1)), int(m.group(2))
    return {"checks": same + diff, "failed": diff}


PARSERS = {
    "exit": lambda text: {}, "app": parse_app, "host": parse_host, "playerkit": parse_playerkit,
    "widekit": parse_widekit, "unittest": parse_unittest, "pass_fail": parse_pass_fail,
    "check_files": parse_check_files, "audio": parse_audio, "snes_mt": parse_snes_mt,
    "snes_conf": parse_snes_conf, "nes_conf": parse_nes_conf, "nes_arm": parse_nes_arm,
}


# ---- repository inputs ----------------------------------------------------------
def _in_scope(rel, prefixes):
    for p in prefixes:
        p = p.rstrip("/")
        if p in (".", "") or rel == p or rel.startswith(p + "/"):
            return True
    return False


class Inputs:
    """Content hashes of repository files, memoized by size and mtime across runs."""

    def __init__(self, memo_path, exclude):
        self.memo_path, self.exclude = memo_path, exclude
        try:
            self.memo = json.loads(memo_path.read_text())
        except (OSError, ValueError):
            self.memo = {}
        self.dirty = False
        self.run_memo = {}
        self._files = None

    def files(self):
        if self._files is None:
            names = []
            try:
                raw = subprocess.run(["git", "-C", str(REPO), "ls-files", "-z", "--cached", "--others",
                                      "--exclude-standard"], capture_output=True, check=True).stdout
                names = [n.decode("utf-8", "replace") for n in raw.split(b"\0") if n]
            except (OSError, subprocess.CalledProcessError):
                for root, dirs, fs in os.walk(REPO):
                    dirs[:] = [d for d in dirs if d not in (".git", "build", "out", "__pycache__")]
                    for f in fs:
                        names.append(os.path.relpath(os.path.join(root, f), REPO))
            self._files = sorted(n for n in set(names) if not n.startswith(self.exclude + "/"))
        return self._files

    def digest(self, rel):
        if rel in self.run_memo:
            return self.run_memo[rel]
        path = REPO / rel
        try:
            st = path.stat()
        except OSError:
            self.run_memo[rel] = "missing"
            return "missing"
        ent = self.memo.get(rel)
        if ent and ent[0] == st.st_size and ent[1] == st.st_mtime_ns:
            d = ent[2]
        else:
            h = hashlib.sha256()
            with open(path, "rb") as fh:
                for chunk in iter(lambda: fh.read(1 << 20), b""):
                    h.update(chunk)
            d = h.hexdigest()
            self.memo[rel] = [st.st_size, st.st_mtime_ns, d]
            self.dirty = True
        self.run_memo[rel] = d
        return d

    def scope(self, prefixes):
        h = hashlib.sha256()
        count = 0
        for rel in self.files():
            if _in_scope(rel, prefixes):
                h.update(rel.encode() + b"\0" + self.digest(rel).encode() + b"\n")
                count += 1
        return h.hexdigest(), count

    def save(self):
        if not self.dirty:
            return
        self.memo_path.parent.mkdir(parents=True, exist_ok=True)
        tmp = self.memo_path.with_suffix(".tmp")
        tmp.write_text(json.dumps(self.memo))
        tmp.replace(self.memo_path)


# ---- requirements and private ROM folders --------------------------------------
def missing_requirements(step):
    missing = []
    for kind, val in step.requires:
        if kind == "exe" and shutil.which(val) is None:
            missing.append("needs " + val)
        elif kind == "path" and not os.path.exists(val):
            missing.append("needs " + val)
        elif kind == "py" and importlib.util.find_spec(val) is None:
            missing.append("needs python module " + val)
    if step.env_req:
        val = os.environ.get(step.env_req, "")
        if not val or not os.path.exists(val):
            missing.append("set %s to %s" % (step.env_req, "a folder of ROMs"
                                             if step.env_req != "GATE_WS_ROM" else "a WonderSwan ROM file"))
    return missing


def fmt(text, ctx):
    for k, v in ctx.items():
        text = text.replace("{%s}" % k, v)
    return text


def git_describe():
    try:
        r = subprocess.run(["git", "-C", str(REPO), "describe", "--always", "--dirty"],
                           capture_output=True, text=True, timeout=30)
        return r.stdout.strip() or "unknown"
    except (OSError, subprocess.SubprocessError):
        return "unknown"


# ---- selection --------------------------------------------------------------------
def changed_tags(base):
    def lines(args):
        try:
            r = subprocess.run(["git", "-C", str(REPO)] + args, capture_output=True, text=True, check=True)
            return [x for x in r.stdout.splitlines() if x.strip()]
        except (OSError, subprocess.CalledProcessError) as e:
            sys.exit("gate: cannot read git changes against %s: %s" % (base, e))
    paths = set(lines(["diff", "--name-only", base])) | set(lines(["ls-files", "--others",
                                                                    "--exclude-standard"]))
    tags, per_path = set(), {}
    for p in sorted(paths):
        per_path[p] = path_tags(p)
        tags |= per_path[p]
    return tags, per_path


def path_tags(p):
    """The tags a changed path selects: the longest CHANGE_MAP prefix, else release or everything."""
    best = None
    for prefix, t in CHANGE_MAP:
        if p == prefix.rstrip("/") or p.startswith(prefix):
            if best is None or len(prefix) > len(best[0]):
                best = (prefix, t)
    if best:
        return set(best[1])
    return {"release"} if "/" not in p else {"*"}


def tiers_allowed(a):
    if a.quick:
        return {"quick"}
    if getattr(a, "release", False):
        return set(TIERS)
    if a.full:
        return {"quick", "default", "full"}
    return {"quick", "default"}


def build_plan(a):
    """Return (ordered step names to run, skipped set, description of how it was chosen)."""
    skip = set(n for x in a.skip for n in x.split(",") if n)
    unknown = sorted(skip - set(BY_NAME))
    if a.only:
        wanted = set(n for x in a.only for n in x.split(",") if n)
        bad = sorted(wanted - set(BY_NAME))
        if bad:
            sys.exit("gate: unknown step(s): %s (see --list)" % ", ".join(bad))
        how = "--only"
    elif a.changed:
        tags, _ = changed_tags(a.base)
        if not tags:
            return [], skip, "no changed files against %s: nothing to run" % a.base
        wanted = {s.name for s in STEPS if s.tier in tiers_allowed(a)
                  and ("*" in tags or s.tags & tags)}
        how = "--changed against %s (tags: %s)" % (a.base, ", ".join(sorted(tags)))
    else:
        wanted = {s.name for s in STEPS if s.tier in tiers_allowed(a)}
        how = "tier " + ("quick" if a.quick else "release" if a.release else "full" if a.full else "default")
    if unknown:
        sys.exit("gate: unknown step(s) in --skip: %s" % ", ".join(unknown))
    order, seen = [], set()

    def visit(name):
        if name in seen or name in skip:
            return
        seen.add(name)
        for d in BY_NAME[name].deps:
            visit(d)
        order.append(name)

    for name in sorted(wanted, key=lambda n: [s.name for s in STEPS].index(n)):
        visit(name)
    return order, skip, how


# ---- keys and cache -------------------------------------------------------------------
def step_key(step, keys, inputs, self_digest, out_dir):
    h = hashlib.sha256()
    h.update(self_digest.encode())
    ctx = _context(out_dir)
    h.update(fmt(step.cmd, ctx).encode())
    h.update(step.cwd.encode())
    if step.env_req:
        h.update((os.environ.get(step.env_req, "") or "").encode())
    scope_digest, _ = inputs.scope(step.inputs)
    h.update(scope_digest.encode())
    for d in step.deps:
        h.update(keys.get(d, "absent").encode())
    return h.hexdigest()


def _context(out_dir):
    return {"out": str(out_dir), "repo": str(REPO), "jobs": str(JOBS[0]), "rgbds": RGBDS,
            "vitasdk": VITASDK, "gitver": GITVER[0]}


JOBS = [CORES]
GITVER = ["unknown"]


def cache_path(cache_dir, name):
    return cache_dir / (name + ".json")


def cached_result(step, key, cache_dir):
    try:
        data = json.loads(cache_path(cache_dir, step.name).read_text())
    except (OSError, ValueError):
        return None
    if data.get("key") != key or data.get("result") != "PASS":
        return None
    for o in step.outputs:
        if not (REPO / o).exists():
            return None
    return data


def write_cache(step, key, res, cache_dir):
    cache_dir.mkdir(parents=True, exist_ok=True)
    data = {"key": key, "result": "PASS", "checks": res["checks"], "failed": res["failed"],
            "skipped": res["skipped"], "seconds": res["seconds"],
            "finished": datetime.datetime.now().isoformat(timespec="seconds")}
    cache_path(cache_dir, step.name).write_text(json.dumps(data, indent=1))


# ---- running ----------------------------------------------------------------------------
class Running:
    def __init__(self, step, proc, log_path, logf, t0):
        self.step, self.proc, self.log_path, self.logf, self.t0 = step, proc, log_path, logf, t0
        self.timed_out = False


def rel_to_repo(p):
    try:
        return str(Path(p).resolve().relative_to(REPO))
    except ValueError:
        return str(p)


def start_step(step, out_dir, logs_dir):
    log_path = logs_dir / (step.name + ".log")
    cmd = fmt(step.cmd, _context(out_dir))
    cwd = REPO / step.cwd
    logf = open(log_path, "w")
    logf.write("== gate step %s: %s\n== cwd: %s\n== cmd: %s\n== started: %s\n\n" % (
        step.name, step.desc, cwd, cmd, datetime.datetime.now().isoformat(timespec="seconds")))
    logf.flush()
    nice = shutil.which("nice")
    # bash with pipefail: a step can never pass through a pipe into `tail`
    # (the status of a pipeline is its first failing command's)
    bash = shutil.which("bash")
    shell = [bash, "-o", "pipefail", "-c", cmd] if bash else ["/bin/sh", "-c", cmd]
    argv = ([nice, "-n", "10"] if nice else []) + shell
    proc = subprocess.Popen(argv, cwd=str(cwd), stdout=logf, stderr=subprocess.STDOUT,
                            stdin=subprocess.DEVNULL, start_new_session=True)
    return Running(step, proc, log_path, logf, time.time())


def kill_group(run):
    try:
        os.killpg(run.proc.pid, signal.SIGKILL)
    except (OSError, ProcessLookupError):
        pass
    run.proc.wait()


def finish_run(run, rc):
    step = run.step
    secs = time.time() - run.t0
    run.logf.write("\n== exit %s, %.1f s\n" % ("timeout" if run.timed_out else rc, secs))
    run.logf.close()
    text = run.log_path.read_text(errors="replace")
    try:
        totals = PARSERS[step.parse](text)
    except Exception as e:  # a parser bug must not hide a result
        totals = {}
        text += "\n(gate: parser error %r)\n" % (e,)
    checks, failed, skipped = totals.get("checks"), totals.get("failed"), totals.get("skipped")
    reason = ""
    if run.timed_out:
        result, reason = "FAIL", "timeout after %d s" % step.timeout
    elif rc != 0:
        result, reason = "FAIL", "exit %s" % rc
    elif step.gating and failed:
        result, reason = "FAIL", "%d failed" % failed
    else:
        result = "PASS"
    if result == "FAIL" and not failed:
        failed = 1  # a failing step always counts at least one failure
    if result == "PASS" and failed is None:
        failed = 0
    if not step.gating and result in ("PASS", "FAIL"):
        result = "INFO" if result == "PASS" else "FAIL"
    return {"result": result, "reason": reason, "checks": checks, "failed": failed,
            "skipped": skipped, "seconds": round(secs, 2), "cached": False,
            "log": rel_to_repo(run.log_path), "cmd": fmt(step.cmd, _context(OUT[0]))}


OUT = [DEFAULT_OUT]
RUNNING = {}  # name -> Running, for the steps in flight (killed on interrupt)


def execute(order, steps, a, out_dir, inputs, keys):
    cache_dir = out_dir / ".gate-cache"
    logs_dir = out_dir / "logs"
    logs_dir.mkdir(parents=True, exist_ok=True)
    results = {}
    plan = set(order)
    pending = list(order)
    running = RUNNING
    busy = set()
    total = len(order)
    finished = [0]

    def record(name, res):
        results[name] = res
        finished[0] += 1
        step = steps[name]
        detail = []
        if res.get("checks") is not None:
            detail.append("checks=%s" % res["checks"])
        if res.get("failed") is not None and res["result"] in ("PASS", "FAIL", "INFO"):
            detail.append("failed=%s" % res["failed"])
        if res.get("cached"):
            detail.append("cached")
        if res.get("reason"):
            detail.append(res["reason"])
        print("[%2d/%d] %-8s %-22s %7.1fs  %s" % (finished[0], total, res["result"], name,
                                                 res.get("seconds") or 0.0, "  ".join(detail)),
              flush=True)

    while pending or running:
        progressed = False
        for name in list(pending):
            step = steps[name]
            if any(d in plan and d not in results for d in step.deps):
                continue
            bad = [d for d in step.deps if d in plan and results[d]["result"] in FAILING]
            if bad:
                pending.remove(name)
                progressed = True
                record(name, {"result": "BLOCKED", "reason": "needs %s (%s)" % (
                    bad[0], results[bad[0]]["result"]), "checks": None, "failed": None,
                    "skipped": None, "seconds": 0.0, "cached": False, "log": "", "cmd": ""})
                continue
            notrun = [d for d in step.deps if d not in plan or results[d]["result"] == "SKIP"]
            if notrun:
                pending.remove(name)
                progressed = True
                record(name, skip_result("needs %s (not run)" % notrun[0]))
                continue
            miss = missing_requirements(step)
            if miss:
                pending.remove(name)
                progressed = True
                record(name, skip_result("; ".join(miss)))
                continue
            hit = None if a.force else cached_result(step, keys[name], cache_dir)
            if hit:
                pending.remove(name)
                progressed = True
                prev_log = logs_dir / (name + ".log")
                record(name, {"result": "PASS", "reason": "", "checks": hit.get("checks"),
                              "failed": hit.get("failed"), "skipped": hit.get("skipped"),
                              "seconds": 0.0, "cached": True,
                              "log": rel_to_repo(prev_log) if prev_log.exists() else "",
                              "cmd": ""})
                continue
            if len(running) >= JOBS[0] or (step.group and step.group in busy):
                continue  # waits in place for a free slot; the poll below sleeps
            pending.remove(name)
            progressed = True
            running[name] = start_step(step, out_dir, logs_dir)
            RUNNING[name] = running[name]
            if step.group:
                busy.add(step.group)
        # poll
        for name, run in list(running.items()):
            rc = run.proc.poll()
            if rc is None and time.time() - run.t0 > run.step.timeout:
                run.timed_out = True
                kill_group(run)
                rc = -9
            if rc is None:
                continue
            del running[name]
            RUNNING.pop(name, None)
            if run.step.group:
                busy.discard(run.step.group)
            res = finish_run(run, rc)
            if res["result"] == "PASS":
                write_cache(run.step, keys[name], res, cache_dir)
            record(name, res)
            progressed = True
        if not progressed and running:
            time.sleep(0.25)
        elif not progressed and not running and pending:
            for name in pending:
                record(name, skip_result("could not schedule"))
            pending = []
    return results


def skip_result(reason):
    return {"result": "SKIP", "reason": reason, "checks": None, "failed": None, "skipped": None,
            "seconds": 0.0, "cached": False, "log": "", "cmd": ""}


# ---- baseline ------------------------------------------------------------------------------
def compare_baseline(results, baseline):
    flags = {}
    for name, res in results.items():
        base = baseline.get(name)
        if not base or res["result"] not in ("PASS", "FAIL", "INFO"):
            continue
        msgs = []
        bc, cc = base.get("checks"), res.get("checks")
        if bc is not None and cc is not None and cc < bc:
            msgs.append("checks %s -> %s" % (bc, cc))
        bf, cf = base.get("failed"), res.get("failed")
        if bf is not None and cf is not None and cf > bf:
            msgs.append("failed %s -> %s" % (bf, cf))
        if msgs:
            flags[name] = msgs
    return flags


# ---- output --------------------------------------------------------------------------------
COLUMNS = ("step", "result", "checks", "failed", "skipped", "seconds", "log", "note")


def _cell(v):
    return "-" if v is None else str(v)


def table_rows(results, flags, order):
    rows = []
    for name in order:
        r = results[name]
        result = r["result"] + (" (cached)" if r.get("cached") else "")
        note = r.get("reason", "")
        if name in flags:
            note = ("REGRESSION: " + "; ".join(flags[name]) + ("; " + note if note else ""))
        rows.append([name, result, _cell(r.get("checks")), _cell(r.get("failed")),
                     _cell(r.get("skipped")), "%.1f" % (r.get("seconds") or 0.0),
                     r.get("log") or "-", note or "-"])
    return rows


def render_text(rows):
    widths = [max([len(COLUMNS[i])] + [len(r[i]) for r in rows]) for i in range(len(COLUMNS))]
    lines = ["  ".join(COLUMNS[i].ljust(widths[i]) if i < 7 else COLUMNS[i] for i in range(len(COLUMNS)))]
    lines.append("  ".join("-" * widths[i] for i in range(len(COLUMNS))))
    for r in rows:
        lines.append("  ".join(r[i].ljust(widths[i]) if i < 7 else r[i] for i in range(len(COLUMNS))))
    return "\n".join(lines)


def render_markdown(rows):
    lines = ["| " + " | ".join(COLUMNS) + " |", "|" + "---|" * len(COLUMNS)]
    for r in rows:
        lines.append("| " + " | ".join(c.replace("|", "/") for c in r) + " |")
    return "\n".join(lines)


def write_summary(out_dir, meta, results, order, flags, steps):
    rows = table_rows(results, flags, order)
    data = dict(meta)
    data["steps"] = {name: {k: results[name].get(k) for k in
                            ("result", "reason", "cached", "checks", "failed", "skipped", "seconds",
                             "log", "cmd")} for name in order}
    data["regressions"] = flags
    (out_dir / "summary.json").write_text(json.dumps(data, indent=1))
    counts = {}
    for name in order:
        counts[results[name]["result"]] = counts.get(results[name]["result"], 0) + 1
    md = ["# Gate summary", "",
          "- run: %s" % meta["started"],
          "- tier: %s  |  selection: %s" % (meta["tier"], meta["selection"]),
          "- git: %s  |  jobs: %d  |  wall: %.0f s" % (meta["git"], meta["jobs"], meta["wall_seconds"]),
          "- results: " + ", ".join("%s %d" % (k, counts[k]) for k in sorted(counts)), "",
          render_markdown(rows), ""]
    if flags:
        md += ["## Regressions against the baseline", ""]
        md += ["- %s: %s" % (n, "; ".join(m)) for n, m in flags.items()] + [""]
    skipped = [(n, results[n]["reason"]) for n in order if results[n]["result"] == "SKIP"]
    if skipped:
        md += ["## Skipped (not failures)", ""]
        md += ["- %s: %s" % (n, r) for n, r in skipped] + [""]
    (out_dir / "summary.md").write_text("\n".join(md))


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    tier = ap.add_mutually_exclusive_group()
    tier.add_argument("--quick", action="store_true", help="the fast subset")
    tier.add_argument("--full", action="store_true", help="everything, incl. armhf and the Vita build")
    tier.add_argument("--release", action="store_true",
                      help="--full plus the release pins (committed VPK hashes, SHA256SUMS)")
    ap.add_argument("--changed", action="store_true", help="only steps selected by changed paths")
    ap.add_argument("--base", default="HEAD", help="what --changed compares with (default HEAD)")
    ap.add_argument("--only", action="append", default=[], metavar="A,B", help="these steps")
    ap.add_argument("--skip", action="append", default=[], metavar="A,B", help="leave these out")
    ap.add_argument("--list", action="store_true", help="list every step and whether it can run here")
    ap.add_argument("--dry-run", action="store_true", help="print the plan without running it")
    ap.add_argument("--jobs", "-j", type=int, default=CORES, help="steps at a time (default %d)" % CORES)
    ap.add_argument("--force", action="store_true", help="ignore the cache and run every step")
    ap.add_argument("--out", type=Path, default=DEFAULT_OUT, help="output directory (default tools/gate/out)")
    ap.add_argument("--baseline", type=Path, metavar="FILE", help="a saved summary.json to compare with")
    a = ap.parse_args(argv)
    if a.only and a.changed:
        ap.error("use --only or --changed, not both")
    JOBS[0] = max(1, a.jobs)
    out_dir = a.out.resolve()
    OUT[0] = out_dir
    GITVER[0] = git_describe()

    if a.list:
        print("%-22s %-8s %-4s %-30s %s" % ("step", "tier", "gate", "prerequisites", "status here"))
        for s in STEPS:
            miss = missing_requirements(s)
            status = "ready" if not miss else "SKIP: " + "; ".join(miss)
            deps = ",".join(s.deps) or "-"
            print("%-22s %-8s %-4s %-30s %s" % (s.name, s.tier, "yes" if s.gating else "info",
                                                 deps[:30], status))
            print("    %s  [%s]" % (s.desc, ",".join(sorted(s.tags))))
        return 0

    order, skip, how = build_plan(a)
    if not order:
        print("gate: " + how if how.startswith("no changed") else "gate: no steps selected")
        return 0

    out_dir.mkdir(parents=True, exist_ok=True)
    cache_dir = out_dir / ".gate-cache"
    exclude = os.path.relpath(out_dir, REPO) if str(out_dir).startswith(str(REPO) + os.sep) else "\0"
    inputs = Inputs(cache_dir / "inputs.json", exclude)
    self_digest = hashlib.sha256(Path(__file__).read_bytes()).hexdigest()
    keys = {}
    for name in order:
        keys[name] = step_key(BY_NAME[name], keys, inputs, self_digest, out_dir)
    if a.dry_run:
        print("gate plan (%s): %d steps" % (how, len(order)))
        for name in order:
            s = BY_NAME[name]
            if cached_result(s, keys[name], cache_dir) and not a.force:
                what = "cached PASS"
            elif missing_requirements(s):
                what = "SKIP: " + "; ".join(missing_requirements(s))
            else:
                what = "run"
            print("  %-22s %-12s %s" % (name, what, fmt(s.cmd, _context(out_dir)) if what == "run" else ""))
        return 0

    started = datetime.datetime.now().isoformat(timespec="seconds")
    t0 = time.time()
    print("gate: %d steps (%s); jobs %d; out %s" % (len(order), how, JOBS[0], rel_to_repo(out_dir)),
          flush=True)
    steps = BY_NAME
    try:
        results = execute(order, steps, a, out_dir, inputs, keys)
    except KeyboardInterrupt:
        for run in list(RUNNING.values()):
            kill_group(run)
        print("gate: interrupted; the steps in flight were killed", file=sys.stderr)
        return 130
    finally:
        inputs.save()
    wall = time.time() - t0

    flags = {}
    if a.baseline:
        try:
            base = json.loads(Path(a.baseline).read_text())
            flags = compare_baseline(results, base.get("steps", base))
        except (OSError, ValueError) as e:
            print("gate: cannot read baseline %s: %s" % (a.baseline, e), file=sys.stderr)
            return 2

    meta = {"started": started, "tier": "quick" if a.quick else "release" if a.release else "full" if a.full else "default",
            "selection": how, "git": GITVER[0], "jobs": JOBS[0], "wall_seconds": round(wall, 1),
            "baseline": str(a.baseline) if a.baseline else None}
    write_summary(out_dir, meta, results, order, flags, steps)

    print()
    print(render_text(table_rows(results, flags, order)))
    print()
    counts = {}
    for name in order:
        counts[results[name]["result"]] = counts.get(results[name]["result"], 0) + 1
    print("gate: " + ", ".join("%s %d" % (k, counts[k]) for k in sorted(counts))
          + "  (%.0f s)" % wall)
    print("summary: %s and %s" % (rel_to_repo(out_dir / "summary.md"), rel_to_repo(out_dir / "summary.json")))
    bad = [n for n in order if results[n]["result"] in FAILING]
    if bad:
        print("gate: FAILED: " + ", ".join(bad))
    if flags:
        print("gate: REGRESSIONS against the baseline: " + ", ".join(sorted(flags)))
    return 1 if (bad or flags) else 0


if __name__ == "__main__":
    sys.exit(main())
