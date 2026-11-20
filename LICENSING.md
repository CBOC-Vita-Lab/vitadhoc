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
the symbol table (`check-licence`). Open point: `transport_lan.c` still includes the
headers `netdrv.h`, `netdrv_wire.h` and `nd_thread.h`, whose provenance has not been
confirmed; a header-free player mode of the transport would remove the question.

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
certificate data and is not part of this grant. Dependencies on GPL-only
gpsp files that remain, and that a separate player must replace with its own
small shims: `frontend-common/fe_evt.h` (logging), `frontend-common/fe_host.h`
(`fe_host_ra_read`, memory reads), `frontend-common/fe_console.h`,
`psp/osd_psp.h` (toasts and the indicator overlay), `vita/cheats_vita.h`
and `vita/plat.h` (threads, time).

## Release Status

Version 1.0's VPK is built from the source in this repository, which sits
beside it. Still open: the experimental GPU xBRZ shader's exact historical
upstream revision is unrecorded, and its combined-binary compatibility is
under review (the feature is off by default; its strings are in the binary).
Neither the root license nor individual source headers have been replaced with
a blanket GPL-3.0 declaration.
