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
The SDK's [build lock](../notices/VitaSDK-build-lock.json) records its complete
source pins. Runtime notice sources are the pinned
[VitaSDK headers](https://github.com/vitasdk/vita-headers/blob/e66ebe90b73d1fa4cce005a5b2072cec27322544/LICENSE.md),
[newlib](https://github.com/vitasdk/newlib/blob/2e428297c0b6aefd830c5a75a7daa7e774562a42/COPYING.NEWLIB)
and [GCC 15.2.0 exception](https://github.com/gcc-mirror/gcc/blob/releases/gcc-15.2.0/COPYING.RUNTIME).
The full dependency notices and pins are in [Third-Party Notices](../notices/THIRD-PARTY.md)
and each vendored component's provenance file. The seven libvita2d shader
objects are retained licensed upstream inputs with `.cg` source alongside them;
they are not a bundled proprietary shader compiler.

## Mbed TLS (Online play, 2.0)

Online play talks HTTPS with the project's own client (`gpsp/vita/nethttps/`),
which needs Mbed TLS 3.6.7. It is fetched and built once, by a script, into a
private prefix; it is never installed into VitaSDK and the build itself stays
offline. The pin (git commit, tag, the one upstream TLS 1.3 clean-up patch and
every hash) is `gpsp/vita/deps/lock.json`; the recipe is
`gpsp/vita/deps/Makefile.mbedtls` and the client profile (TLS client only, no
server, no SHA-1 or MD5) is `gpsp/vita/deps/mbedtls/np_user_config.h`.

```sh
python3 tools/online/build-mbedtls.py --target vita   --work "$TLS"   # Vita prefix:  $TLS/mbedtls/vita
python3 tools/online/build-mbedtls.py --target host64 --work "$TLS"   # twin prefix:  $TLS/mbedtls/host64
```

`$TLS` is any new folder. The first run needs the network once (it clones the
pinned source and checks the commit); after that `--fetch-only` and
`--rebuild` exist, and the script refuses to reuse a work folder built from
different inputs (use a new one). `--target all` builds `vita`, `host32` and
`host64`. The Vita target needs VitaSDK at `$VITASDK` (default `/opt/vitasdk`)
and `arm-vita-eabi-gcc`; the host targets need gcc.

Then pass the prefix to the builds:

```sh
make -C gpsp/vita -j4 vpk BIOS= VITASDK=/opt/vitasdk MBEDTLS="$TLS/mbedtls/vita"
make -C gpsp/vita/hostcore ARCH=x86 -j8 MBEDTLS="$TLS/mbedtls/host64"
```

Without `MBEDTLS=` the Vita build stops at `vpk` on purpose: a package without
TLS would be an Online that cannot connect. The twin builds without it and then
speaks plain `http://` only (the TLS test cases are skipped; `MBEDTLS_HOST`
points the unit tests at the host prefix). The licence of Mbed TLS (Apache-2.0
or GPL-2.0-or-later, used here under GPL-2.0-or-later) is in
[Licensing Status](../LICENSING.md).

## Vita Build

From the repository root, with the SDK at `/opt/vitasdk` and the Mbed TLS Vita
prefix built above:

```sh
make -C gpsp/vita -j4 vpk BIOS= VITASDK=/opt/vitasdk MBEDTLS="$TLS/mbedtls/vita"
```

Output: `gpsp/vita/VitAdhoc.vpk`; intermediate files are in
`gpsp/vita/build/vita/`. The default title is VitAdhoc, ID `GBAD00001`,
version 4.1.1 / SFO 04.11, ATTRIBUTE2 12. The shared package must use empty
`BIOS=`. Do not add a private BIOS to a public build.

The core reports itself as `4.1.1+4.5.0fixes` (`CORE_VERSION`): GBAdhoc 4.1.1
plus the fixes taken from 4.5.0, listed in the 1.3 notes of
[Technical Notes](TECHNICAL.md). `VERSION` stays 4.1.1 and sets the package's
SFO version.

`GIT_VERSION` defaults to the current source revision; it is the label the
app writes in its log. Version 1.0 was built with
`GIT_VERSION=library-r7-20261022` ([Source record](SOURCE.md) has its hashes).

The VPK target uses the ready-to-package images under `gpsp/vita/sce_sys/`.
For this working tree, the Neon Link masters are under `gpsp/vita/artwork/livearea/`.
To regenerate the bubble, LiveArea background, and Start gate, install
ImageMagick 7 and run `make -C gpsp/vita assets`. It uses the bundled Outfit
and Inter fonts and produces 8-bit indexed PNGs. A normal VPK build does not
need ImageMagick. Keep the masters with the working files when moving this tree.

## Tests

```sh
python3 -m unittest discover -s tests                     # download checker
python3 -m unittest discover -s gpsp/vita/tests -p test_release_package.py
make -C gpsp/vita/tests/gamedb test
make -C gpsp/vita/hostcore ARCH=x86 -j8                   # the app, built for Linux (add MBEDTLS=<host64 prefix> for https://)
(cd online/service && node --test "test/*.test.mjs")      # the online room service (Online, 2.0)
python3 gpsp/vita/hostcore/app_tests.py x86 --keep        # whole-app tests
```

The package test checks `VitAdhoc.vpk` at the repository root (or
`RELEASE_VPK=/path/to/VitAdhoc.vpk`): no BIOS or games inside, the notices
byte-identical to this tree, and the expected runtime hash.

The whole-app tests run the real app on Linux with a scripted pad, touch and
keyboard, and check its log and screenshots. They need the test cartridges:
`make -C gpsp/vita/hostcore/testrom` (arm-none-eabi tools). The RetroAchievements
test runs against a stand-in server on the loopback.

Unit tests for the 1.0 features, each with `-fsanitize=address,undefined`,
from `gpsp/`:

```sh
gcc -std=gnu99 -DINLINE=inline -ffunction-sections -fdata-sections \
    -Ilibretro/libretro-common/include -Ifrontend-common -o /tmp/t_cart \
    tools/tests/test_cart_sensors.c savestate.c -Wl,--gc-sections && /tmp/t_cart
gcc -std=gnu99 -Ifrontend-common -o /tmp/t_patch tools/tests/test_rom_patch.c \
    vita/rom_patch.c frontend-common/fe_util.c && /tmp/t_patch
gcc -std=gnu99 -Ivita -Ifrontend-common -o /tmp/t_log \
    vita/tests/playlog/test_play_log.c vita/play_log.c && /tmp/t_log
(cd gbcore && gcc -std=gnu11 -fgnu89-inline -fno-strict-aliasing -I. -I../frontend-common \
    -o /tmp/t_mbc7 tests/mbc7.c gbcore_tgbdual.c tgbdual/*.c && /tmp/t_mbc7)
```

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

## Package Review

Check safe/unique archive paths, `GBAD00001`, no original BIOS/ROM/save, the
expected executable hash, all readable notices, fonts/art/database provenance
and corresponding source. The release VPK sits at the repository root as
`VitAdhoc.vpk`, beside the source it was built from; `SHA256SUMS` lists every
file (`python3 tools/update-checksums.py`). Final acceptance must test the exact chosen package on physical Vita;
a build, archive scan or emulator run does not replace that check.
