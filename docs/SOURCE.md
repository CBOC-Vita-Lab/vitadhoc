# Source Record

| Item | Value |
| --- | --- |
| Version | 1.2 |
| Package | `VitAdhoc.vpk`, SHA-256 `b844da404b1ed36ca776818d6870a19c3effc23a4c39ed6503854eeb16c940de` |
| Runtime (`eboot.bin`) | SHA-256 `3e107b77447ef26961de846a32b0cf90e9cafe55dcc56329efaa75424e0b4074` |
| Title ID | `GBAD00001` (existing data layout: `ux0:data/GBAdhoc/`) |
| Original 1.2 source commit | `dd6998f6a902f0f0a8deae0ae3e16c73b5c103f7`, based on 1.1 commit `91ce334f2e807e919ce7602e86bb9d44c13dde50` |
| Build command | `make -C gpsp/vita -j4 vpk BIOS= VITASDK=/opt/vitasdk GIT_VERSION=1.2` |
| Toolchain | VitaSDK `0.20260926.321` ([Building](BUILDING.md) has the pins) |

The release documentation may be updated after the original source commit;
the hashes above identify the unchanged 1.2 application package. `GIT_VERSION`
is the label written in the app log, not a substitute for a package hash.

The VPK's 24 entries include the runtime, original app artwork, databases and
readable license/attribution notices. Package checks verify the runtime hash,
title ID, safe archive paths and matching notices. No commercial game,
save file or proprietary BIOS is included; the open-source BIOS is built in.
The install ZIP includes the same VPK and the corresponding source.

## Automated Evidence

The historical 1.2 acceptance record reports the 54-gate release harness,
whole-app tests of the x86 host build, audio tests under sanitizers, the GBA
rewind suite and release-package checks. A local-play save-timing gate timed
out under load and passed on reruns. This is not an unqualified clean first
run of all 54 gates.

The broader host suite retained seven older failures also seen in 1.1. The
GB/GBC rewind legs were not run in that acceptance environment because the
`rgbds` assembler was unavailable. Keep those gaps separate from the passing
focused suites. [Building](BUILDING.md) lists runnable test commands.

For the publication audit, the original source/install checksum checker
verified 1,166 listed files. All 13 download-checker tests and five
release-package tests passed. The original install ZIP's 1,167 regular files
matched that source snapshot, including its checksum manifest.

These checks establish file integrity and specific tested behaviours. Host
builds, native compilation and package transfer do not by themselves prove
physical-Vita frame rate, audio quality, link reliability or game compatibility.

## Hardware Evidence

The historical 1.0 hardware result reported Pokemon Ruby at 59.9 emulated fps
with CPU xBRZ and wide view enabled. It does not establish the speed of 1.2,
Pokemon Unbound or other ROM hacks.

Installation/readback of the 1.2 runtime and a subjective first-play
impression are distinct from a controlled gameplay benchmark. Current
hardware checks, exact settings, scenes and capture provenance belong in the
[performance report](PERFORMANCE.md). Do not turn a smooth-looking recording
or a host test result into a universal full-speed claim.

[Known Issues](../KNOWN-ISSUES.md) tracks hardware acceptance gaps.
[Licensing Status](../LICENSING.md) records the maintainer's permission and
the components whose terms and provenance need separate consideration.
