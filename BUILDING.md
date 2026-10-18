# Build From Source

## Inputs

The source snapshot contains the Vita frontend, both emulator cores, open GBA
BIOS source/binary, fonts, databases and vendored runtime dependencies. It
contains no commercial BIOS or games. Old PSP releases, game artwork and
private test saves are deliberately omitted.

Use GNU make and VitaSDK. The checked local toolchain is VitaSDK
`0.20260926.321`, GCC 15.2.0, buildscripts revision
`3c0b9c7b2c8ecb7bf68a674451a295f724b7fafb`. The x86_64 Linux archive
`vitasdk-x86_64-linux-gnu-2026-09-26_08-31-53.tar.bz2` has SHA-256:

```text
87b111ff350c1f36bde1c2923e57467c1f86b044d54bc616631260486f2a512c
```

Obtain SDK tools from [VitaSDK](https://vitasdk.org/), not this repository.
The full dependency notices and pins are in [Third-Party Notices](notices/THIRD-PARTY.md)
and each vendored component's provenance file. The seven libvita2d shader
objects are retained licensed upstream inputs with `.cg` source alongside them;
they are not a bundled proprietary shader compiler.

## Vita Build

From the repository root, with the SDK at `/opt/vitasdk`:

```sh
make -C gpsp/vita -j4 vpk BIOS= VITASDK=/opt/vitasdk
```

Output: `gpsp/vita/VitAdhoc.vpk`; intermediate files are in
`gpsp/vita/build/vita/`. The default title is VitAdhoc, ID `GBAD00001`,
version 4.1.1 / SFO 04.11, ATTRIBUTE2 12. The shared package must use empty
`BIOS=`. Do not add a private BIOS to a public build.

`GIT_VERSION` defaults to the current source revision. A receipt must record
an explicit override if one is used; do not represent a custom label as an
upstream commit. Two full, network-isolated Linux x86_64 builds succeeded
with the pinned SDK: one uses `GIT_VERSION=build21-clean1` to distinguish it,
and the other uses the original `GIT_VERSION=5f0fa23` label for comparison.
Neither run accessed a Vita. The BIOS-free repack is separately identified,
not called a clean build.

| Candidate | Executable SHA-256 |
| --- | --- |
| Original build-21 runtime, BIOS-free repack | `d7b06b6cbb6242c702c27961878fc7deb4a9add0beb4090fb0798f88747ca016` |
| Independent `build21-clean1` build | `03f8cc382e634edd47d75f98a4efa8b7bfa90403fbddf565f4e47090f1ab808a` |
| Independent build with original `5f0fa23` label | `0b21c419ae0611c93578a9edff65bed0e06a905a86469e663f1f55620ac9bf57` |

The original-label rebuild matches all 5,463,744 decoded runtime bytes except
four bytes in module-info metadata. The SELF headers also differ, so this is
not a byte-identical reproduction. The other build has a different version
label. Archive/runtime privacy checks pass, but no candidate has final
publishing hardware acceptance; do not transfer tests between them.

Committed LiveArea images allow a normal build without Python graphics tools.
To regenerate them, install Pillow, CairoSVG and their normal dependencies,
then run `make -C gpsp/vita assets`. This now calls `tools/make_livearea.py`,
not the obsolete `make_assets.py`. pngquant is optional and changes PNG bytes;
record tool versions before claiming byte-for-byte artwork reproduction.

## Tests

```sh
make -C gpsp/vita/tests/gamedb test
python3 -m unittest discover -s gpsp/vita/tests -p test_release_package.py -v
```

The package tests use the separately assembled build-21 repack. Set
`RELEASE_VPK=/absolute/path/to/VitAdhoc-build21.vpk` when it is not in the
private publishing workspace. They deliberately fail if no package exists;
they do not silently skip binary checks. Its expected original-runtime hash
is recorded in the test.

The existing cheat suite's full supported command is:

```sh
make -C gpsp/vita/tests/cheats test RGBDS=/path/to/rgbds/
make -C gpsp/vita/tests/cheats vita VITASDK=/opt/vitasdk
```

It needs a Linux x86 toolchain, ARM hard-float cross compiler, qemu-arm,
arm-none-eabi tools for its synthetic GBA cartridge and RGBDS for the GB
cartridge. RGBDS 0.8.0 was built from its official archive, SHA-256
`7097e713384376c324bb001707b4d3924dc7051358a54069eb0bfd424ebe3c4e`.
It is not a native Apple Silicon test command.

Both architectures passed fresh publishing-workspace runs: 33,202 engine,
36,418 list/database and 202 end-to-end checks per architecture, with no
failed checks. The final runs were serialized to avoid shared fixture writes.
The ARM binaries were run by native ARM64-hosted qemu-arm; nested x86
translation incorrectly lost arguments in the ARM list test, so that failed
environment run is not counted as passed. RGBDS was built without LTO after
the host compiler's LTO jobserver crashed; this affects the test assembler,
not the Vita app.

The Vita-only cheat compilation also passed, with an existing unused-variable
warning in `gba_memory.c`. These are source tests, not physical-Vita gameplay
acceptance. Generated test ROMs are not included in Git or the player VPK.

## Package Review

Check safe/unique archive paths, `GBAD00001`, no original BIOS/ROM/save, the
expected executable hash, all readable notices, fonts/art/database provenance
and corresponding source. Keep VPKs/install ZIPs in Releases, not source
history. Final acceptance must test the exact chosen package on physical Vita;
a build, archive scan or emulator run does not replace that check.
