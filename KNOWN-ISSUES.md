# Known Issues And Test Coverage

## Before Public Release

- Resolve the experimental GPU shader license against the rest of the app.
  Its referenced upstream includes GPL-3.0, while the project advertises
  GPL-2.0. The CPU xBRZ copy has a different unversioned notice. The GPU
  strings are embedded even when its setting is off; do not treat that
  default as a distribution exemption. No blanket rights-clearance claim.
- The BIOS-free build-21 package still needs physical-Vita fresh-install,
  open-BIOS fallback, save/load, relaunch and upgrade acceptance.
- External ROM roots, large libraries and their adjacent saves passed the
  earlier host checks, not final build-21 hardware acceptance.
- The independent clean build succeeds but has a different version label and
  runtime hash. Select and test one exact final candidate; neither inherits
  the other's acceptance.

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
