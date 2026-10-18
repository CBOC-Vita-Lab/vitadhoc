# VitAdhoc

![VitAdhoc's included LiveArea artwork](docs/media/livearea-startup.png)

Included LiveArea artwork, not a gameplay screenshot.

Game Boy Advance, Game Boy and Game Boy Color on PS Vita, based on GBAdhoc
and gpSP. It uses a native Vita frontend, with touch menus, a game browser
and two-player link modes.

## Highlights

- Browse GBA, GB and GBC libraries without moving existing ROM folders.
  Build 21 adds external folders, including common RetroFlow locations.
- Use five save-state slots, rewind, remappable controls and optional turbo
  buttons. Battery saves stay beside their games.
- Choose integer scaling, fit or stretch, colour correction, frame blending,
  Scale3x or CPU xBRZ. GBA wide-view options are available for supported games.
- Open the included cheat database and offline game descriptions without
  downloading a separate pack. Games, covers and Nintendo's BIOS are not included.
- Link two Vitas or run two emulated handhelds on one Vita. These modes have
  different requirements and testing limits: see [Play Together](PLAY-TOGETHER.md).

## Current Status

This private build-21 candidate keeps Claude's original executable, removes
the separately packaged Nintendo BIOS and adds distribution notices. Package
and host tests pass, and the source builds cleanly. The unchanged runtime has
passed an in-place Vita update, launch and external-folder scan. Fresh installation, gameplay,
save/load and the experimental GPU shader's license review remain pending.
It is not a public release. See [Known Issues](KNOWN-ISSUES.md) for test limits.

## Install

New to the package? Open [Start Here](START-HERE.txt).

You need a homebrew-enabled PS Vita, VitaShell and games you may lawfully use.
No BIOS download is required; an open replacement is built in.

1. Once a release is available, install its `VitAdhoc-build21.vpk` with VitaShell.
2. Start VitAdhoc once to create `ux0:data/GBAdhoc/`.
3. Put `.gba`, `.gb` and `.gbc` files in `ux0:data/GBAdhoc/roms/`, or use an
   [existing ROM folder](INSTALL.md#existing-rom-folders).

Back up saves before updating, including saves beside games in external
folders. [Install](INSTALL.md) covers paths, optional art and troubleshooting.

[Controls](CONTROLS.md) | [Technical Notes](TECHNICAL.md) |
[Building](BUILDING.md) | [Assets And Licenses](ASSETS.md) |
[Changes](CHANGELOG.md)

CBOC, Claude and ChatGPT contributed to this release preparation; full roles
and upstream acknowledgments are in [Credits](CREDITS.md). Code and databases
retain their separate licenses.
