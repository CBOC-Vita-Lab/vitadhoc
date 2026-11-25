# Licensing Status

## GBAdhoc Permission

On October 19, 2026, GBAdhoc maintainer ShoshinFauteux explicitly permitted
their own contributions under **GPL-2.0-or-later**, including use under GPL-3.0.
The permission covers their work in `gpsp/psp`, `gpsp/frontend-common`,
`gpsp/netdrv`, `gpsp/gbalink`, the `gbcore` glue and their gpSP core changes.
See the [maintainer's permission](https://github.com/ShoshinFauteux/GBAdhoc/issues/14#issuecomment-6267226116).

This resolves the question about that contributor's code. It does not
relicense other authors' work or clear the complete Vita package. Thanks to
[GBAdhoc and ShoshinFauteux](https://github.com/ShoshinFauteux/GBAdhoc) for the
original project and the clarification. Original notices remain intact.

## Separate Components

- `gpsp/psp/font_8x16.c` is a Linux VGA font with GPL-2.0-only terms, not part
  of this permission. Do not describe it as GPL-2.0-or-later.
- The maintainer flagged unclear provenance for the PSP Media Engine code
  under `gpsp/psp/me`. The permission does not settle that code's origin.
- The Vita build does not link either PSP component above. This is not a
  claim about every PSP build.
- Gambatte (the Accurate Game Boy core, 2.0) is **GPL-2.0 only**, not "or later". It is linked into the main
  program, so the main program as distributed is under GPL-2.0 for that part: the "including use under GPL-3.0"
  line above does not extend to a build that contains it. See
  [Game Boy: Gambatte](notices/GB-Gambatte.md).
- CPU xBRZ, mGBA-derived files, fonts, databases and
  SDK/runtime components retain their own notices. See
  [Third-Party Notices](notices/THIRD-PARTY.md).

## Which Licence The Main App Is Under

The main app (the `eboot.bin`: GBA, Game Boy, NES and WonderSwan) is one program,
so what it may be distributed under is what every part linked into it allows.

- **Today (without Gambatte): GPL-2.0-or-later.** Every part of the main app
  allows version 2 (or later), so the program can be taken as a whole under
  GPL-2.0 or GPL-3.0, as the maintainer's permission above describes.
- **With Gambatte linked in (the Accurate Game Boy core, 2.0): GPL-2.0 only.**
  Gambatte is GPL-2.0-only. The combined work is then distributed under
  **GPL-2.0**, and the "including use under GPL-3.0" line above does not extend
  to a build that contains it.
- The experimental GPU xBRZ shader (adapted from a shader carrying Zenju's
  GPL-3.0 notice) was the one GPL-3.0 part and could not sit in a GPL-2.0-only
  combination. It was **removed from the main app in 2.0** (code, shader text,
  its notice file and the shader-compiler link), so that conflict no longer
  exists. The tree and the package contain no GPL-3-only code in the main app.

Every component of the main app and how it fits GPL-2.0 (the combined work):

| Component | Licence | Fits GPL-2.0 |
| --- | --- | --- |
| gpSP core, GBAdhoc-derived code (`gpsp/psp`, `frontend-common`, `netdrv`, `gbalink`, `gbcore` glue) | GPL-2.0-or-later (maintainer's permission) | yes |
| TGB Dual (`gpsp/gbcore`) | GPL-2.0 | yes |
| Gambatte (2.0) | GPL-2.0 only | yes: it is why the whole is GPL-2.0 |
| FCEUmm (NES) | GPL-2.0-or-later | yes |
| Beetle WonderSwan | GPL-2.0-or-later | yes |
| CPU xBRZ (`third_party/xbrz`) | GPL, no version named in the files; used under version 2 | yes, as version 2 |
| Mbed TLS | Apache-2.0 OR GPL-2.0-or-later; used under GPL-2.0-or-later | yes, by that choice |
| mGBA cheat engine | MPL-2.0 | yes (MPL-2.0 allows combining with GPL) |
| Blip_Buffer (in Beetle) | LGPL-2.1-or-later | yes |
| libvita2d, stb, rcheevos, libretro-common, SameBoy palette data, xBR | MIT / Expat | yes |
| Newlib, GCC runtime (libgcc, GPL-3.0-or-later with the Runtime Library Exception) | BSD-style / GPL-3 with exception | yes: the exception covers the runtime linked into a GPL-2.0 program |
| Fonts (Inter, Outfit: OFL), databases, art | their own licences, shipped as data | not linked code |
| `gpsp/psp/font_8x16.c`, `gpsp/psp/me` | GPL-2.0-only / unclear | not linked into the Vita build |

`snes.bin` and `md.bin` are separate programs and keep their own licences
(sections below). This table describes the main app only; the CPU xBRZ row is
a reading of a header that names no version (its author's later releases name
version 3), so that reading stays a small, recorded risk.

## The Two-Player Link of the Players

`snes.bin` and `md.bin` play two-player games with playerkit's `pk_link` (MIT, written
for the players) on the project's own UDP transport and NAT layer
(`gpsp/netdrv/transport_lan.c`, `nat_*.c`, owner's code, `MIT OR GPL-2.0-or-later`
in their SPDX headers). They do not link `netdrv.c`, `fe_gblink.c`, `fe_evt.c`,
`fe_util.c` or `gb_link.c` (the GBAdhoc-derived Game Boy link): each player's
Makefile fails the build when one of them is in the object list, the linker map or
the symbol table (`check-licence`).

The players do not use the app driver's headers either. A player build defines
`PK_PLAYER`: `transport_lan.[ch]` then take the few types they need (clock, thread,
mutex, counters, a datagram ring, the transport vtable) from
`playerkit/src/pk_lan_compat.h`, written from scratch for playerkit (MIT), and the
app-only rewrite of a netdrv roster frame is compiled out (no `nd_crc16`, no shim).
The app build (no `PK_PLAYER`) is unchanged. `check-licence` in `snes/vita` and
`md/vita` also fails the build when the dependency file of any object of the player
names `netdrv.h`, `netdrv_wire.h`, `nd_thread.h`, `gb_link.h` or another transport
header of the app driver.

## The SNES Player (Snes9x)

Super NES games run in `snes.bin`, a separate program in the same VPK. It is
built only from `snes/` and links no code from `gpsp/`: VitAdhoc starts it with
the game's path and it starts VitAdhoc again when you quit.

- The Snes9x core in `snes/core` keeps the Snes9x licence (`snes/core/LICENSE`,
  shipped as `licenses/Snes9x-LICENSE.txt`). It allows non-commercial use and
  changes only and is not GPL-compatible, which is why it is its own program.
- Our frontend in `snes/frontend`, `snes/vita` and `snes/twin` is MIT.
- Because the package contains Snes9x, VitAdhoc must stay free of charge and
  must not be sold, bundled in a commercial product or used to promote one.
- [SNES notice](notices/SNES-Snes9x.md) and `snes/core/PROVENANCE.md` (the
  tag, commit and the few local edits) have the details. No legal opinion has
  been obtained on the separate-program design.

## The Sega Player (Genesis Plus GX)

Genesis / Mega Drive, Master System, Game Gear and SG-1000 games run in
`md.bin`, a second separate program in the same VPK, built only from `md/`,
`playerkit/` and `widekit/` (plus the dual-licensed files named in this
document) and started and left the same way as `snes.bin`.

- The Genesis Plus GX core in `md/core` keeps its own licence
  (`md/core/LICENSE.txt`, shipped as `licenses/GenesisPlusGX-LICENSE.txt`). It
  allows non-commercial use and changes only and is not GPL-compatible, which
  is why it is its own program. The same file carries the notices of the parts
  it includes (Nuked OPN2 and OPLL and blip_buf under LGPL-2.1, the MAME-derived
  sound and Z80 cores, Musashi).
- Our frontend in `md/frontend`, `md/vita` and `md/twin` is MIT.
- Because the package contains Genesis Plus GX, the same condition as for
  Snes9x applies a second time: VitAdhoc must stay free of charge and must not
  be sold, bundled in a commercial product or used to promote one.
- [Sega notice](notices/MD-GenesisPlusGX.md) and `md/core/PROVENANCE.md` (the
  pinned commit and every local edit) have the details. No legal opinion has
  been obtained on the separate-program design.

## Online Play: Mbed TLS And The Service

Version 2.0 links Mbed TLS 3.6.8 for the HTTPS client of online play. Mbed TLS
is offered under **Apache-2.0 OR GPL-2.0-or-later**; VitAdhoc takes the
GPL-2.0-or-later option, which matches the rest of the program, and both texts
stay in `licenses/MbedTLS-LICENSE.txt`. It is built from a pinned upstream
checkout, unpatched (pin in
`gpsp/vita/deps/lock.json`) and is not stored in the repository. Apache-2.0 is
not compatible with GPL-2.0-only, so this relies on the "or later" and on the
choice Mbed TLS offers: if the Snes9x player or any other separate program
ever linked it, that would need its own check (the SNES player does not).
The HTTPS client, the NAT layer and the room service are the project owner's
own code. Cloudflare's STUN and TURN are services, not code. No legal opinion
has been obtained.

## RetroAchievements Client: MIT Or GPL-2.0-or-later

`gpsp/vita/ra_vita.c`, `ra_vita.h`, `ra_http.c`, `ra_http.h` and
`ui_vita_ra.h` are the project's own code (written for VitAdhoc, none of it
from GBAdhoc or another project) and carry `SPDX-License-Identifier: MIT OR
GPL-2.0-or-later`, so the SNES and Genesis players can link them. They build
on rcheevos (MIT, `gpsp/vita/third_party/rcheevos`). `ra_roots.h` is generated
by our own script (`gpsp/vita/tools/ra_roots.py`, dual-licensed like the rest of
these files, so the file's wrapper and its SPDX line are covered by the grant); the
PEM certificates inside it are the CAs' public root certificates, taken from the
Mozilla root store as packaged by Debian, and are not ours to license (see
`notices/THIRD-PARTY.md`). Dependencies on GPL-only
gpsp files that remain, and that a separate player must replace with its own
small shims: `frontend-common/fe_evt.h` (logging), `frontend-common/fe_host.h`
(`fe_host_ra_read`, memory reads), `frontend-common/fe_console.h`,
`psp/osd_psp.h` (toasts and the indicator overlay), `vita/cheats_vita.h`
and `vita/plat.h` (threads, time).

## Release Status

Version 1.0's VPK is built from the source in this repository, which sits
beside it. The experimental GPU xBRZ shader, whose historical upstream
revision was never recorded and whose GPL-3.0 notice could not be combined with
Gambatte, was removed from the main app (see above).
Neither the root license nor individual source headers have been replaced with
a blanket GPL-3.0 declaration.
