# Source Record

| Item | Value |
| --- | --- |
| Version | 1.0 |
| Package | `VitAdhoc.vpk`, SHA-256 `ba45181433f9d250f07e2316dea60e6acaa74010e53ae2c28b16c3fc6124baa4` |
| Runtime (`eboot.bin`) | SHA-256 `9069471e980c5b0616b48048dcad7c0446b3070ee63033bbcb1b528dd19bb9e8` |
| Title ID | `GBAD00001` (installs over earlier builds and keeps their data) |
| Built from | this commit's `gpsp/`, `notices/` and `licenses/` |
| Build command | `make -C gpsp/vita -j4 vpk BIOS= VITASDK=/opt/vitasdk GIT_VERSION=library-r7-20261022` |
| Toolchain | VitaSDK `0.20260926.321` ([Building](BUILDING.md) has the pins) |

`GIT_VERSION` is only the label the app writes in its log. The package's
`eboot.bin`, `notices/THIRD-PARTY.md` and the rest of its 24 entries were
checked against this source by the release acceptance: only `eboot.bin` and
the notices differ from the earlier build-21 package. The VPK contains no
Nintendo BIOS, games or saves; the open BIOS is built in.

Version 1.0 passed the full automated suite (54 checks, from unit tests under
sanitizers to whole-app runs of the computer build and the native Vita build).
The CPU test cartridge (`cpufix`, 12 checks) also passed on the ARM twin,
which runs the same dynarec as the Vita. On a Vita, the first 1.0 build ran
Pokémon Ruby at a steady 59.9 frames a second; this package adds the start-up
and quick-launch fixes found there. See [Known Issues](../KNOWN-ISSUES.md).

The first 1.0 build installed for testing had runtime
`a6f965aca4a6717a3a8d49c11c7ebdaae0ff279df93f7098ae8aab2efb888b20`; the
installer updates it in place.

Earlier: build 21's selected runtime was
`d7b06b6cbb6242c702c27961878fc7deb4a9add0beb4090fb0798f88747ca016`
(source revision `5f0fa23a1bed08f1eec0ae9751405ac9e15c0f16`).

[Licensing Status](../LICENSING.md) records the GBAdhoc maintainer's
permission and the components that keep their own terms.
