# Source Record

| Item | Value |
| --- | --- |
| Version | 1.1 |
| Package | `VitAdhoc.vpk`, SHA-256 `ba45181433f9d250f07e2316dea60e6acaa74010e53ae2c28b16c3fc6124baa4` |
| Runtime (`eboot.bin`) | SHA-256 `9069471e980c5b0616b48048dcad7c0446b3070ee63033bbcb1b528dd19bb9e8` |
| Title ID | `GBAD00001` (shares `ux0:data/GBAdhoc/` with GBAdhoc's data layout) |
| Built from | this commit's `gpsp/`, `notices/` and `licenses/` |
| Build command | `make -C gpsp/vita -j4 vpk BIOS= VITASDK=/opt/vitasdk GIT_VERSION=library-r7-20261022` |
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

[Licensing Status](../LICENSING.md) records the GBAdhoc maintainer's
permission and the components that keep their own terms.
