# Source Record

| Item | Value |
| --- | --- |
| Version | 1.2 |
| Package | `VitAdhoc.vpk`, SHA-256 `b844da404b1ed36ca776818d6870a19c3effc23a4c39ed6503854eeb16c940de` |
| Runtime (`eboot.bin`) | SHA-256 `3e107b77447ef26961de846a32b0cf90e9cafe55dcc56329efaa75424e0b4074` |
| Title ID | `GBAD00001` (shares `ux0:data/GBAdhoc/` with GBAdhoc's data layout) |
| Built from | this commit's `gpsp/`, `notices/` and `licenses/` |
| Build command | `make -C gpsp/vita -j4 vpk BIOS= VITASDK=/opt/vitasdk GIT_VERSION=1.2` |
| Toolchain | VitaSDK `0.20260926.321` ([Building](BUILDING.md) has the pins) |

`GIT_VERSION` is only the label the app writes in its log. The package's
`eboot.bin`, `notices/THIRD-PARTY.md` and the rest of its 24 entries were
checked against this source by the release acceptance. The VPK contains no
Nintendo BIOS, games or saves; the open BIOS is built in.

Version 1.0 passed the full automated suite (54 checks, from unit tests under
sanitizers to whole-app runs of the computer build and the native Vita build).
The CPU test cartridge (`cpufix`, 12 checks) also passed on the ARM twin,
which runs the same dynarec as the Vita, and the app was played on a Vita:
Pokémon Ruby at a steady 59.9 frames a second. See
[Known Issues](../KNOWN-ISSUES.md).

Version 1.2 passed, on the computer build: the release-acceptance harness
used for 1.0 (54 gates, from sanitized unit tests to whole-app runs of the
computer build and the native Vita build; three of its tests were updated for
1.2's intended changes: ten state slots, the zip reader and recovery from a
graphics error), the app suite (x86 twin), the audio suites (6 of 6, under
sanitizers), the GBA rewind suite and the release-package checks. The broad
host suite has the same seven older failures as 1.1 and no new ones. The
GB/GBC rewind legs need the `rgbds` assembler and were not run. 1.2 has not
yet been timed or played on a Vita.

[Licensing Status](../LICENSING.md) records the GBAdhoc maintainer's
permission and the components that keep their own terms.
