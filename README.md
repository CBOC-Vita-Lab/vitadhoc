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

This is the private build-21 publishing candidate, not a verified public
release. The BIOS-free package retains the original build-21 executable;
distribution notices have been added. Its package checks and cartridge
database tests pass. A separate clean-source build also succeeds; it is a
different candidate, not a substitute for hardware testing.

Earlier hardware testing measured build 20 in one GBA game at approximately
59.94 FPS at 500 MHz. That does not verify build 21, the full game library,
two-Vita networking or every display option. [Known Issues](KNOWN-ISSUES.md)
lists the remaining checks. No campaign-wide or universal compatibility claim
is made. Distribution also waits for the experimental GPU shader's license
compatibility to be resolved; it is distinct from the CPU xBRZ implementation.

## Install

You need a homebrew-enabled PS Vita, VitaShell and games you may lawfully use.
No BIOS download is required; an open replacement is built in.

1. Once a release is available, install its `VitAdhoc-build21.vpk` with VitaShell.
2. Start VitAdhoc once to create `ux0:data/GBAdhoc/`.
3. Put `.gba`, `.gb` and `.gbc` files in `ux0:data/GBAdhoc/roms/`, or use an
   [existing ROM folder](INSTALL.md#existing-rom-folders).

Back up saves before updating. Installing the same `GBAD00001` application ID
does not intentionally move your data, but the update path still needs final
hardware acceptance. See [Install](INSTALL.md) for exact paths, optional art,
updates and troubleshooting.

[Controls](CONTROLS.md) | [Technical Notes](TECHNICAL.md) |
[Building](BUILDING.md) | [Assets And Licenses](ASSETS.md) |
[Changes](CHANGELOG.md)

CBOC, Claude and ChatGPT contributed to this release preparation; full roles
and upstream acknowledgments are in [Credits](CREDITS.md). Code and databases
retain their separate licenses.
