# playerkit file formats

Everything playerkit writes lives under the app's data folder, `<root>`
(`ux0:data/GBAdhoc` on the console). The save-state side deliberately uses the
same folders, names and picture formats as the Game Boy Advance player, so
the home screen's shelf can read SNES and NES states with no special cases.
This file describes the formats as they are on disk. All numbers are
little-endian.

## Save states

Folder: `<root>/states/<console>/<xx>/`, with `<console>` = `snes`, `nes`,
`gba`, `gb`, `gbc`, `ws`, and `<xx>` the game's bucket: the first two hex
digits (lower case) of the CRC-32 (zlib's) of `<name>`'s bytes, e.g.
`Super Example World (USA)` -> crc32 `f3a54da3` -> `states/snes/f3/`. 256
small folders instead of one with thousands of files: a lookup in a big
folder on the Vita's card takes seconds. `<name>` is the game's file name
without its extension (no folder, no `.sfc`), exactly as written, spaces and
brackets included. The VitAdhoc app uses the same layout for its own consoles
(its Game Boy files carry `.gb`/`.gbc` before `.stN`).

Before 1.4 the files were in `<root>/states/<console>/` itself (and before
1.3 beside the ROM). A game's files are moved into its bucket the first time
the game is looked at (`migrate_to_bucket`; the app's
`sidecar_migrate_game`): never over a file already in the bucket (that one
wins, the old one stays where it was), and a file that cannot be moved stays
and is tried again next time. When everything moved, an empty marker
`<name><ext>.st0.mig` in the bucket says so, and later looks cost one stat.
A reader that finds no marker and no files in the bucket may look in the old
flat folder.

| file | what |
| --- | --- |
| `<name>.st0` ... `<name>.st9` | the ten slots. Slot 1 in the menu is `.st0`, slot 10 is `.st9`. The raw bytes the player's core produced; no wrapper. |
| `<name>.stN.thumb` | 64 x 42 picture for the slot list (see below) |
| `<name>.stN.shot` | picture up to 240 x 160 for the big preview |
| `<name>.stN.meta` | playerkit's own note (optional; a reader may ignore it) |
| `<name>.rsm` | "Continue": where the game was left (written on exit and about once a minute) |
| `<name>.rsm.pt` | which patch the resume point was made with (text) |
| `<name><ext>.st0.mig` | empty: this game's files are all in the bucket (see above) |
| `<root>/scenes/<name>.shot` | the home screen's picture of the game, same picture format as `.shot` |

A game with a ROM patch applied keeps the same `<name>`; the patch is told
apart by the key in `.rsm.pt` (the app's own note, copied from `patch_key` in
the launch file). A resume point made with a different patch is not loaded.

### Picture files (`.thumb`, `.shot`, scene)

A 16-byte header, then rows of 16-bit pixels.

```
offset  size  field
0       4     magic "STH1"
4       2     width in pixels
6       2     height in pixels
8       2     row stride, in pixels (not bytes)
10      6     zero
16      ...   height rows of `stride` pixels, 2 bytes each
```

- `.thumb`: width 64, height 42, stride 64. The game picture is shrunk to
  fit and centred on black (a 256x224 frame becomes 48x42 with black bars at
  the sides).
- `.shot` and scene: width up to 240, height up to 160, stride 256. The
  picture is shrunk to fit inside 240 x 160 keeping its shape; pictures that
  are already smaller are stored as they are. The columns between width and
  stride are unused (zero).
- Pixel format is **BGR565, blue in the high bits**: `(b5 << 11) | (g6 << 5) | r5`.
  This is the order the main app uploads straight into its textures; it is
  the reverse of libretro's RGB565 that the cores produce, so the writer
  swaps red and blue.
- Shrinking is a box filter: each destination pixel is the average of the
  source pixels it covers.

### `.stN.meta`

Plain `key=value` lines, one per line, written with the same safe write as
the state.

```
v=1
console=snes
name=Super Example World (USA)
size=70512
crc32=1a2b3c4d
saved=1760000000
play_s=754
frame_w=256
frame_h=224
end=1
```

`size` and `crc32` describe the state file; the player checks them when it
loads and refuses a damaged file with a message instead of feeding it to the
core. `end=1` is the last line: a note without it (cut short by a power loss)
or an empty note is ignored. The note is emptied before its state is replaced
and written after, so it never describes another state. `saved` is Unix seconds; the slot list shows it as `3 Oct 2026, 14:32`
(day, abbreviated month, year, 24-hour time, local time). If there is no
`.meta` (an older player wrote the state) the file's own modification time is
the save time and the check is skipped. `play_s` is the play time of the game
when the state was made, `-1` when unknown.

When the main app deletes a state it should remove `.stN`, `.stN.thumb`,
`.stN.shot` and `.stN.meta` (and any `.old` or `.tmp` leftovers beside them).

### `.rsm.pt`

The patch key as text, or `-` and a newline when no patch was in use.

## Safe writes

The state files themselves (`.stN`, `.rsm`), the launch and session files, the
controls and the battery save are written the same way:

1. write `<path>.tmp`;
2. read it back and compare its size and CRC32 with what was meant to be
   written;
3. rename the old file to `<path>.old` (the Vita's card refuses to rename
   over an existing file, so the old one must be moved first);
4. rename `<path>.tmp` to `<path>`;
5. remove `<path>.old`.

If any step fails the old file is put back, so a failed save never destroys
the previous one. Step 1 truncates a leftover `.tmp`; a writer that knows the
file is new skips step 3 (no lookup of a missing name is ever made: on the
Vita's card that walks the whole folder).

The companions of a state (`.thumb`, `.shot`, `.meta`, `.rsm.pt`) and the
scene picture are written in place, in one operation each, after the state:
they are never the only copy of anything, a cut picture fails its size check,
a cut note has no `end=1`, and a cut `.rsm.pt` reads as another patch (the
resume point is then not used). Readers must check a picture's size against
its header before using it. One level of undo for a load is kept in memory by
the player (no `.undo` file). If power is lost between steps 3 and 4 there is a
`<path>.old` and no `<path>`; readers call `recover_old()` (or simply look for
`.old`) and put it back.

## Controls

`<root>/controls/<console>.ini` is the global mapping for a console;
`<root>/controls/<console>/<game name>.ini` is the mapping for one game, used
instead of the global one when it contains `own=1`.

```
version=1
console=snes
own=0
stick=move          # left stick: off | move (acts as the d-pad) | custom (its four directions are bindable)
rstick=off
back_touch=0        # the rear panel's four corners as inputs
deadzone=72         # 40..120; a direction engages this far from centre (of 128)
bind_b=CROSS        # one physical input per game button
bind_a=CIRCLE
...
p2.bind_b=CROSS     # players 2 to 4 have their own p2./p3./p4. keys
bind_turbo_a=NONE
bind_turbo_b=NONE
bind_ff=RSTICK_RIGHT
bind_quicksave=SELECT+L          # shortcuts take a chord of up to four inputs
bind_quickload=SELECT+R
bind_screenshot=NONE
bind_rewind=RSTICK_LEFT
turbo_a_rate=15     # presses per second: 30, 15 or 10
turbo_a_mode=hold   # hold | toggle
```

Input names: `UP DOWN LEFT RIGHT CROSS CIRCLE SQUARE TRIANGLE L R START SELECT
LSTICK_UP LSTICK_DOWN LSTICK_LEFT LSTICK_RIGHT RSTICK_UP RSTICK_DOWN RSTICK_LEFT
RSTICK_RIGHT BACK_TL BACK_TR BACK_BL BACK_BR`, joined with `+`, any case, `NONE`
for unbound. A chord never contains both START and SELECT (that pair opens
the menu). Unknown keys and bad values are ignored and the default is kept.

## Launch and session files

See `INTEGRATION.md` for how the app and a player use them.

`<root>/launch/<console>.ini`, written by the app before it starts a player:

```
version=1
session_id=s-1760000000-1     # echoed back in session.txt
console=snes
game=ux0:data/GBAdhoc/roms/snes/Super Example World (USA).sfc
game_name=Super Example World
save=ux0:data/GBAdhoc/saves/snes/Super Example World (USA).srm
root=ux0:data/GBAdhoc
shape=original               # original | fit | zoom | full | pixel
quality=sharp                # sharp | smooth
cpu_mhz=0                    # 0 = the player's default, else 444 or 500
enter_button=cross           # cross | circle
language=en
start=new                    # new | continue | slot
slot=0                       # 1..10 when start=slot
patch=                       # IPS / BPS / UPS applied to the ROM in memory
patch_key=
retroarch_save=              # a RetroArch save to bring in when newer than `save`
wide_supported=0
ovr.scanlines=4              # per-game overrides, ovr.<key>=<value>
```

`<root>/launch/session.txt`, written by the player when it ends:

```
version=1
session_id=s-1760000000-1
console=snes
game=...
seconds_played=3725          # time the game actually ran (menu time excluded)
save_changed=1               # the battery save was written
save_error=0                 # a battery save failed (the old file was kept)
notice=Could not save the game: the memory card is full   # one line
states_touched=2,9          # slots saved or deleted
resume_present=1             # a Continue point exists
exit=normal                  # normal | game_list | error | suspend
ended_at=1760000000
```

Both files are written with the safe write, tolerate missing and unknown
keys, and carry a version; a reader that sees a newer version reads what it
knows and keeps the rest.

## Look memory

What the look governor (`pk_look`, `INTEGRATION.md` section 8) learned about one game. Only a game that needed a
lighter look than the best one has a file; a game that held 60 at the full look has none (and a file whose look
turned out not to be needed any more is deleted: `LOOK_MEM_DELETE`).

Path: `<root>/look/<console>/<xx>/<name>.look`, `<console>` as for states (`gba`, `gb`, `gbc`, `nes`, `ws`, `snes`),
`<xx>` and `<name>` as for states (the CRC-32 bucket and the game's file name without extension). Written with the
safe write (see "Safe writes") by the frontend's writer thread, at the end of the game or when the menu opens, never
on the game thread; the module only makes and reads the text (`LookGovernor::serialize`, `load_memory`).

```
pk-look 1
ladder=1a2b3c4d
mhz=444
mask=15
cool=30
peak=780
sav=120,120,80,200,100
```

| key | meaning |
| --- | --- |
| first line | `pk-look 1`: the format version. Anything else is rejected |
| `ladder` | FNV-1a of the rung names and rows (hex). A file made with another ladder (another build, another console) is rejected |
| `mhz` | the CPU clock it was learned at. At another clock the costs are used, the look is not (the game starts at the best look) |
| `mask` | bit i = rung i of the ladder is applied: the look that cured the last overload. Never has a bit beyond the ladder |
| `cool` | seconds of the cool-down before a climb is tried (doubled by failed climbs, 0..960) |
| `peak` | the worst 4 s load seen at that look, in thousandths of a refresh (0..4000) |
| `sav` | the learned cost of each rung, thousandths of a refresh, comma separated, one per rung (0..2000) |

A reader ignores unknown keys, rejects the whole file when a known key is out of range, and then starts from the
best look with the frontend's own guesses: a damaged file never makes the picture worse than having no file. Text,
at most 1 KiB.
