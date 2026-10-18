# Known Issues And Test Coverage

## Before Public Release

- Resolve the experimental GPU shader license against the rest of the app.
  Its referenced upstream includes GPL-3.0, while the project advertises
  GPL-2.0. The CPU xBRZ copy has a different unversioned notice. The GPU
  strings are embedded even when its setting is off; do not treat that
  default as a distribution exemption. No blanket rights-clearance claim.
- The earlier BIOS-free build-21 package passed an in-place Vita installation,
  file readback, launch and game-browser check. The current repack only adds
  license notices; the executable and app resources are unchanged. This exact
  package still needs installation, open-BIOS fallback, gameplay, save/load
  and gameplay-relaunch acceptance.
- Default external-folder scanning found an existing GBA library on hardware.
  Custom roots, the list limit and adjacent save/load have host coverage only.
- The selected package retains the original executable. The independent
  original-label build succeeds but differs by four decoded module-metadata
  bytes, with a different runtime hash. It is not a byte-identical rebuild
  and does not inherit the selected package's hardware evidence.

## Feature Limits

- Two-Vita Wi-Fi/internet/ad hoc, save sharing, the system connection screen,
  second-controller ports and the hardware keyboard lack final acceptance.
- Wireless Adapter mode was brought up/torn down in tests, not accepted in
  gameplay. No three/four-player or single-cartridge multiboot mode.
- Full Mystery Gift reception from a real phone through the game's save flow
  remains untested. A prior accepted computer parcel is narrower evidence.
- Browser preview recording/playback is not yet accepted on physical Vita.
- Only Emerald has prior commercial-game hardware measurements. Other
  wide-view profiles, generic scouting and real Mode 7 racers remain unverified.
- Wide-view edge sprites/weather can be incomplete; menus may use fallback
  margins. Experimental GPU xBRZ is too slow for the earlier tested workload.
- Unsupported cheat types are marked in the UI; GB Xploder codes are absent.
  Some cheats need their enable/master code. Back up saves before using cheats.
- Same-basename games share artwork/cheat/preview identifiers and some settings.
  The full path distinguishes favourites, not every per-game resource.
- The 2,048-entry limit applies per console list. External roots are cut first.

## Report A Problem

Give the build, console model, settings, game region/revision and shortest
reproduction steps. Say whether it occurs on hardware or an emulator. Include
only a reviewed log excerpt or permitted screenshot, not ROMs, BIOS files,
private saves or entire unsanitized logs. State what you expected and what
actually happened; an FPS counter without a duration/scenario is not enough.
