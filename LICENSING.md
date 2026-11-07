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

## Release Status

Version 1.0's VPK is built from the source in this repository, which sits
beside it. Still open: the experimental GPU xBRZ shader's exact historical
upstream revision is unrecorded, and its combined-binary compatibility is
under review (the feature is off by default; its strings are in the binary).
Neither the root license nor individual source headers have been replaced with
a blanket GPL-3.0 declaration.
