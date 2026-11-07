# Source Record

| Item | Value |
| --- | --- |
| Version | 1.3 |
| Package | `VitAdhoc.vpk`, SHA-256 `935d938cd0fa9293ed6fa5847bb83c72e0a779814e4083f9b6d64c0f6e3586b2` |
| Runtime (`eboot.bin`) | SHA-256 `c17d060a27af5df57ad9e32e67e49cb7f530229d72b97b1b185fafb8876946f6` |
| SNES player (`snes.bin`) | SHA-256 `ee8de2831f7f148e38d582181753ec6ce037190dfd5241c4a5b711f2b54dc56c` |
| Title ID | `GBAD00001` (existing data layout: `ux0:data/GBAdhoc/`) |
| Source | The `v1.3` tag (application code as built; later commits before the tag change documentation, screenshots and release records only), based on 1.2 commit `fb41c1d3e171` |
| Build command | `make -C gpsp/vita -j2 vpk BIOS= VITASDK=/opt/vitasdk GIT_VERSION=1.3` (also builds `snes/vita`, single job) |
| Toolchain | VitaSDK `0.20260926.321` ([Building](BUILDING.md) has the pins) |

`GIT_VERSION` supplies the version label written in the app log.

The VPK's 28 entries include the runtime, the SNES player, original app
artwork, databases and readable license/attribution notices. `snes.bin` is a
separate program built only from `snes/` (Snes9x 1.63 and our MIT frontend);
it contains no code from `gpsp/`, and its licence travels as
`licenses/Snes9x-LICENSE.txt`. Package checks verify both program hashes,
the title ID, safe archive paths and matching notices. No commercial game,
save file or proprietary BIOS is included; the open-source BIOS is built in.
The install ZIP includes the same VPK and the corresponding source.

## Automated Evidence

The 1.3 candidate passed the full gate on the x86 host build of the app:

| Suite | Result |
| --- | --- |
| Whole-app tests (`app_tests.py`) | 1,996 checks, 0 failed |
| Rewind (`rewind_tests.py`) | 373 checks, 0 failed |
| Two players on one console (`local_tests.py`) | 471 checks, 0 failed |
| Link between consoles (`link_tests.py --hz 20`, lossy network included) | 649 checks, 0 failed |
| Audio regressions | 8 passed, 0 failed |
| SNES self tests (`snes/twin/selftest.sh`) | all passed |
| Release package (`gpsp/vita/tests`) | 5 tests passed |

The SNES threads were checked bit for bit against a single-threaded run
(frame hashes and audio) on 289 test programs and 15 commercial games the
tester owns (`snes/twin/mt_compare.py`); the games are not included.
[Building](BUILDING.md) lists runnable test commands.

## Hardware Evidence

The [performance report](PERFORMANCE.md) records the physical-Vita samples,
exact settings, counter definitions and display captures, alongside the
runtime readback and preservation checks.

[Known Issues](../KNOWN-ISSUES.md) tracks remaining hardware checks.
[Licensing Status](../LICENSING.md) records the maintainer's permission and
component terms and provenance.
