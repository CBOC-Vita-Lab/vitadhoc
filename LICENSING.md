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
- **Open conflict (2.0, not resolved):** a GPL-2.0-only part and a GPL-3.0 part cannot be in one program. The
  experimental GPU xBRZ shader text (`gpsp/vita/shaders/xbrz_*.inc`, compiled into the main program as strings) is
  adapted from a shader that carries Zenju's GPL-3.0 notice (`licenses/GPU-xBRZ-notices.txt`). Before Gambatte every
  other part allowed "or later", so the program could be taken as a whole under GPL-3.0; with Gambatte linked in it
  cannot. One of the two has to leave the main program (or the shader has to be rewritten from the MIT xBR source)
  before a build with both is distributed. The CPU xBRZ copy (`gpsp/vita/third_party/xbrz`) names no GPL version and
  is used under version 2; its author's later releases name version 3, so that reading is a second, smaller risk.
- CPU xBRZ, experimental GPU xBRZ, mGBA-derived files, fonts, databases and
  SDK/runtime components retain their own notices. See
  [Third-Party Notices](notices/THIRD-PARTY.md).

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
beside it. Still open: the experimental GPU xBRZ shader's exact historical
upstream revision is unrecorded, and its combined-binary compatibility is
under review (the feature is off by default; its strings are in the binary);
with the GPL-2.0-only Gambatte core in the same program this is a conflict,
not only a review item (Separate Components above).
Neither the root license nor individual source headers have been replaced with
a blanket GPL-3.0 declaration.
