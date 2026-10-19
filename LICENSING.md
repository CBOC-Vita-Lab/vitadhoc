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
- The reviewed build-21 Vita build does not link either PSP component above.
  This is not a claim about every PSP build or the newer covers executable.
- CPU xBRZ, experimental GPU xBRZ, mGBA-derived files, fonts, databases and
  SDK/runtime components retain their own notices. See
  [Third-Party Notices](notices/THIRD-PARTY.md).

## Release Status

The newer covers executable still needs its matching source. The remaining
component provenance and combined-package review are also unfinished,
including the GPU shader's exact historical source revision. This repository
remains an earlier build-21 source preview, not a cleared binary release.
Neither the root license nor individual source headers have been replaced
with a blanket GPL-3.0 declaration.
