/* config_psp.h — persisted frontend settings (plan §8), ini-backed
 * (<appdir>/config.ini).  Loaded at boot, saved on change.  Distinct from
 * .gpsp-harness.ini (harness channel, ADR-0036) and variant.ini
 * (per-game builds). */
#ifndef CONFIG_PSP_H
#define CONFIG_PSP_H

#include "fe_console.h"
#include "ctl_map.h"

/* REWIND LENGTH (Settings > "Rewind length"): Short = 1 MB, Medium = 2 MB,
 * Long = 4 MB of history.  Medium is the default, as 2 MB always was (the
 * size every test record is at).  Short is the one size that lives entirely
 * in the volatile pool and takes nothing from the heap; Long takes 2.56 MiB
 * of it (docs/REWIND.md 26).  The key stays `rewind_mb`, an integer of
 * megabytes, so harness scripts and old configs keep working. */
#define PCFG_REWIND_MB_SHORT   1
#define PCFG_REWIND_MB_MEDIUM  2
#define PCFG_REWIND_MB_LONG    4
#ifndef VITA   /* the Vita's own rewind sets its default below */
#define PCFG_REWIND_MB_DEFAULT PCFG_REWIND_MB_MEDIUM
#endif
/* Any integer -> the nearest allowed size that is not above it (3 -> 2,
 * 5..8 -> 4), and never below Short (0, negatives -> 1). */
static inline int pcfg_rewind_mb_snap(long mb)
{
   return mb >= PCFG_REWIND_MB_LONG ? PCFG_REWIND_MB_LONG :
          mb >= PCFG_REWIND_MB_MEDIUM ? PCFG_REWIND_MB_MEDIUM
                                      : PCFG_REWIND_MB_SHORT;
}
/* Short -> Medium -> Long -> Short (dir < 0: the other way). */
static inline int pcfg_rewind_mb_step(int mb, int dir)
{
   mb = pcfg_rewind_mb_snap(mb);
   if (dir < 0)
      return mb == PCFG_REWIND_MB_SHORT ? PCFG_REWIND_MB_LONG :
             mb == PCFG_REWIND_MB_MEDIUM ? PCFG_REWIND_MB_SHORT
                                         : PCFG_REWIND_MB_MEDIUM;
   return mb == PCFG_REWIND_MB_SHORT ? PCFG_REWIND_MB_MEDIUM :
          mb == PCFG_REWIND_MB_MEDIUM ? PCFG_REWIND_MB_LONG
                                      : PCFG_REWIND_MB_SHORT;
}
static inline const char *pcfg_rewind_mb_name(int mb)
{
   mb = pcfg_rewind_mb_snap(mb);
   return mb == PCFG_REWIND_MB_LONG ? "Long" :
          mb == PCFG_REWIND_MB_MEDIUM ? "Medium" : "Short";
}
#include "fe_util.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ADR-0071 build-scoped defaults.  A PLAYABLE build ships an upscaled picture
 * and the speed profile; the HARNESS keeps 1x so its frame-time numbers stay
 * comparable with every run recorded before this existed. */
#define PCFG_PROFILE_SPEED   0
#define PCFG_PROFILE_COMPAT  1
/* ADR-0073: THE SPEED PROFILE IS 57.00, NOT FULL SPEED, AND THAT IS THE POINT.
 *
 * Emerald stops reading the CLIENT's buttons whenever its RFU receive queue is
 * more than 4 deep (pokeemerald src/overworld.c:2520, KeyInterCB_SelfIdle) and
 * that queue drains exactly once per frame.  It exists only on the child, so
 * the host structurally cannot suffer it.  A host that produces packets faster
 * than the client consumes them therefore fills it and never empties it.
 *
 * At 59.73 the host reaches ~59.4 while the client manages ~56.6 -- and
 * net_pace_match is a CEILING, so nothing reconciles them.  Measured over 100+
 * scripted runs on two consoles:
 *
 *   target   trades        client fails to reach its seat
 *   59.73    21/45 = 0.47  17/45
 *   55.00    15/25 = 0.60   5/25
 *   57.00    13/13 = 1.00   0/13
 *
 * 57.00 is the highest tested rate the client still tracks closely (-0.7 fps
 * vs -2.0 at full speed), which keeps frames short -- delivery clumping rises
 * as the rate falls, so lower is not automatically safer and 53.00 was worse
 * than 55.00 on every measure.
 *
 * THIS ONLY APPLIES WHILE A WIRELESS SESSION IS LIVE.  Single-player is
 * unpaced and still runs as fast as the console can manage. */
#define PCFG_PROFILE_FPS_X100(p)  ((p) == PCFG_PROFILE_COMPAT ? 2997 : 5700)
#ifdef VITA
/* THE PS VITA. Fit fills the height without stretching. Explicit legacy
 * scale keys retain their old meaning. There is no Media Engine: me_mode stays 0 and
 * its key (like the other PSP-hardware keys) is read if present and never
 * written (config_psp.c). */
#define PCFG_SCALE_DEF       1
#define PCFG_ME_MODE_DEF     0
#elif defined(GPSP_PLAYABLE)
/* VID_SCALE_FIT (1): fills the 272-px height at the correct 3:2 aspect,
 * 408x272.  NOT stretch — stretch is 480x272, which is 18 % more GE fill AND
 * distorts, and it is one press of the video preset in the menu if wanted. */
#define PCFG_SCALE_DEF       1
/* Media Engine mode ON by default for players (2026-08-10): the second-core
 * renderer ran five consecutive flawless harness runs (~40k frames, 0 drops),
 * fps floor above the CPU path, user-validated visuals — and it self-disables
 * to CPU rendering if the ME fails its boot handshake or wedges, so a model
 * where it misbehaves degrades instead of breaking.  The settings toggle
 * ("Media Engine mode", restart-to-apply) turns it off. */
#define PCFG_ME_MODE_DEF     1
#else
#define PCFG_SCALE_DEF       0
#define PCFG_ME_MODE_DEF     0   /* harness: arms opt in explicitly */
#endif

#ifdef VITA
#define PCFG_NET_ADDRS 4
#endif

typedef struct
{
   int  scale;        /* VID_SCALE_* (default 1x) */
   int  filter;       /* VID_FILTER_* (default nearest; 2 = sharp bilinear) */
   /* ADR-0071: TRADING PROFILE.  0 = speed (DEFAULT), 1 = compatibility.
    *
    * Only the playable/variant builds consult this; the harness sets its rate
    * explicitly and ignores it.  Changing it requires a RELAUNCH, because the
    * session rate is latched into the pacer and the netdrv timeout scaling at
    * startup and half-changing it mid-session would leave the two consoles
    * disagreeing about how long a frame is — which is precisely the failure
    * gen-3's RFU cannot survive (it counts link timeouts in FRAMES).
    *
    *   0 speed          59.73 fps.  Full speed, and 1.00x the adapter timeout
    *                    budget a real GBA gets — exactly faithful.
    *   1 compatibility  29.97 fps.  Half rate, and therefore 1.99x that
    *                    budget.  The extra wall-clock per emulated timeout is
    *                    the actual mechanism, not "less work per second". */
   int  profile;
   /* Media Engine mode (user opt-in, default 0): run the GBA scanline
    * renderer on the PSP's second CPU core.  Applied at boot (the ME PRX,
    * capture mode and staging buffers are boot-time constructions), so the
    * settings UI relaunches the app when it changes — the same ADR-0071
    * restart flow the trading profile used. */
   int  me_mode;
   /* Persist the legacy keys for compatibility: 30/1 = 3x (default),
    * 0/0 = Unlimited, 0/1 = Unlimited Smooth. Use pcfg_ff_set_mode(). */
   int  ff_mult_x10;
   int  ff_hold;      /* 1 = hold (default), 0 = toggle */
   /* Sparse frameskip for NORMAL play: draw N frames, skip 1.  0 = off.
    * For a game sitting just under 60, halving the frame rate (ordinary
    * frameskip) reads as a hitch, while dropping 1 frame in 4 or 8 recovers
    * headroom and still looks smooth.  Suggested: 7 (87.5% drawn) for a small
    * nudge, 3 (75%) for a real one. */
   int  frameskip_sparse;
   int  ff_smooth;
   /* Keep audio running during fast-forward (default 0 = mute, the shipped
    * behaviour).  Exists for benchmark parity: an emulator that mutes during
    * FF skips the sample copy and the output path, and comparing it against
    * one that does not is an uncontrolled variable.  Note the core generates
    * the samples either way — only delivery is skipped — so the difference
    * is expected to be small, but "expected" is not "measured". */
   int  ff_audio;
   int  theme;        /* UI palette: 0 = dark (default), 1 = light */
   /* Game-list shell: 0 = Shelf (default, list + art panel),
    * 1 = Marquee (art fills the screen).  The browser reads it once at
    * startup, so like me_mode it applies on the next launch. */
   int  ui_shell;
#ifdef VITA
   /* The Vita's home screen (vita/ui_vita_home.h) in place of ui_shell's
    * list: 1 = home (default), 0 = the shelf or marquee ui_shell says. */
   int  ui_home;
   /* The home screen's previews (vita/clip_vita.h): 1 = the last ten
    * seconds of a game are kept when it is left, and play when the row
    * rests on it (default); 0 = nothing is recorded, the still shows. */
   int  previews;
   /* Keep full speed (main_vita.c, catching up): 1 = when the console
    * falls behind the display (a slow frame, the recompiler meeting new
    * code), the frames it is behind by are emulated without being drawn,
    * so the game's time and its sound stay at full speed (default);
    * 0 = the game simply runs late. */
   int  full_speed;
   /* Order of the game list (Settings > LIBRARY > Game order, CONFIG.INI
    * lib_sort): 0 by name, 1 most recently played first, 2 most played
    * first (psp/play_log.h). */
   int  lib_sort;
#endif
   /* Media Engine dirty-page VRAM copy: send only the 1 KiB pages the
    * frame actually touched instead of all 96 KiB.  Default 1.  Exposed
    * here so a graphics complaint can be bisected on the shipping build --
    * it was harness-only, which meant a player could not turn it off and
    * a bug report could not separate it from same-frame. */
   int  me_dirty;
   /* CONFIG.INI `rom_resident` (read, never written): hold a 32 MiB cart
    * whole when the heap allows it (main_psp.c rom_cache_select).  Default
    * ON since 3.0.0 (the Go A/B rig, 21/21); `rom_resident = 0` pages. */
   int  rom_resident;
   int  show_fps;     /* 1 = OSD chip with emulated-frame rate (default 0) */
#ifdef REWIND
   /* Settings > Gameplay > Rewind (docs/REWIND.md).  rewind: 1 = record
    * (GBA games, outside link sessions); 0 = no snapshots, no memory, and
    * the store paths carry no page marks.  Read when a game is started.
    * rewind_mb: megabytes of history -- one of the three named lengths
    * (pcfg_rewind_mb_snap): 1 Short, 2 Medium (the default), 4 Long.
    * CONFIG.INI `rewind`, `rewind_mb`. */
   int  rewind;
   int  rewind_mb;
/* The toggle's default.  OFF until the owner decides (docs/REWIND.md). */
#ifndef PCFG_REWIND_DEFAULT
#define PCFG_REWIND_DEFAULT 0
#endif
#endif
   /* 1 (default): a GBA cartridge clock shows the console's real time
    * (rtc_clock.h).  0 = the frame-counting clock of 3.5.0 and earlier, the
    * kill switch.  CONFIG.INI `rtc_realtime`, written only if present. */
   int  rtc_realtime;
   /* The thumbstick (ctl_map.h CTL_STICK_*): 0 off (default, 3.5.0's
    * behaviour), 1 a second d-pad, 2 four bindable directions.  CONFIG.INI
    * `stick = off | move | custom`, written only once it has been changed. */
   int  stick;
   /* 1 once the player has seen the Controls page's one-time note about
    * putting Quick save / Quick load on the stick alone (`stick_warned`). */
   int  stick_warned;
   /* GB/GBC DMG palette, a GBCORE_PALETTE_* id (gbcore.h): 0 = Auto (Super
    * Game Boy colours where the game has them, grey otherwise, default),
    * 1 Grey, 2 DMG green, 3 Pocket.  Unknown ids fall back to Auto. */
   int  gb_palette;
   /* Ambient bars (PCFG_AMB_*): what fills the bars beside a picture that
    * does not cover the screen.  Like scale, filter and gb_palette this is
    * the LIVE MIRROR of the running (or browsed) console's display profile
    * -- see "PER-CONSOLE DISPLAY PROFILES" at the end of this header. */
   int  ambient;
   /* CONFIG.INI `loading_art` (read, never written): 1 (default) puts the
    * game's hero art behind the ROM loading screen and bakes the ambient
    * texture from it; 0 skips both, so the launch path is the 3.1 one.  The
    * A/B switch for the startup-heap proof in docs/DISPLAY-FEATURES.md. */
   int  loading_art;
   /* CONFIG.INI `gbc_color_correction` (no UI row): 1 (default) shows Game
    * Boy Color games through the GBC LCD model, 0 shows their colours raw
    * (gbcore_set_color_correction; docs/GB-PALETTE-FIXES.md). */
   int  gbc_color_correction;
   /* Benchmark mode (config.ini only, no UI row).  Makes the FF button mean
    * "engine throughput test": uncapped pacing, frameskip DISABLED (every
    * frame is rendered), and the ME kept on its normal ASYNC path instead of
    * the synchronous FF path.  Without this, an FF measurement flatters us
    * badly — the shipping FF path renders on a 33 ms cadence (~30 fps of
    * render work) and blocks on the ME, so it measures neither the parallel
    * pipeline nor a comparable render load. */
   int  bench_mode;
   /* Wireless-session frameskip policy (ADR-0019).  0 = off (default, the
    * smooth one), 1 = adaptive (auto_threshold with hysteresis, engaged
    * only after sustained real-time slippage), 2 = auto (ADR-0018's
    * blanket behaviour, kept for hardware A/B). */
   int  net_frameskip;
   /* gpsp_frameskip_threshold used when the adaptive policy engages, as the
    * core's option string (audio-buffer occupancy %, core default 33). */
   char net_skip_threshold_str[8];
   /* TX offload thread (ADR-0021).  0 = inline (pre-ADR-0021 behaviour),
    * 1 = prompt (default, TX thread above main), 2 = deferred (TX thread
    * below main, sends drain in the vblank slack).  All three kept so the
    * user can A/B them on hardware in one sitting without a rebuild. */
   int  net_tx_thread;
   /* EVT/LOG writer thread (ADR-0024).  0 = inline fprintf+fflush on the
    * emulation thread (pre-ADR-0024 behaviour), 1 = default: lines go to a
    * ring and a low-priority writer thread does the memory-stick I/O in the
    * slack the emulation thread already donates at the vblank wait.  The
    * field measured 12.0-12.5 ms inside one flush, on both consoles, inside
    * a 16.7 ms frame.  Accepts either `log_thread` or `net_log_thread`. */
   int  log_thread;
   /* .sav block writer (ADR-0025).  0 = ADR-0020's budgeted drain on the
    * emulation thread, 1 = default: the same writer thread that owns the
    * EVT log does every open/seek/write/flush/close.  The field measured
    * `sram_flush ... ms=29.296` in the one window whose `frame` maximum
    * jumped 37264 -> 63772 us, which is the largest emu-thread stall left
    * after ADR-0024. */
   int  sram_thread;
   /* Session pacing mode.  **THE MEANING OF `1` CHANGED IN ADR-0033** — it
    * used to select ADR-0027/0028's adaptive peer matcher and now selects
    * fixed-rate clamping.  An existing config.ini carrying `net_pace_match=1`
    * therefore gets the NEW behaviour silently, which is deliberate: the
    * fixed clamp is what we want shipped, and the value that used to mean
    * "on" still means "on".
    *   0 = off      — both consoles free-run (unchanged meaning).
    *   1 = fixed    — DEFAULT.  While a session is live both consoles clamp
    *                  to `net_session_fps` (below).  Identical on both sides,
    *                  no negotiation, no control loop, no hunting.
    *   2 = adaptive — ADR-0027/0028's peer-capability matcher, kept verbatim
    *                  so the two policies can be A/B'd on hardware.
    * Gen-3's RFU counts link timeouts in FRAMES, so two emulators 20 % apart
    * (the field's 58 fps host vs 46 fps join) are mutually inconsistent in a
    * way two real GBAs never are.  Equality is what the games require; ADR-0033
    * buys it with a constant instead of a control loop. */
   int  net_pace_match;
   /* Requested fixed session frame rate in hundredths (ADR-0033),
    * `net_session_fps`.  Default 2997 = 29.97 fps = exactly two PSP vblanks
    * (ADR-0035 — 40.00 was unreachable in the field, see PCFG_SESSION_FPS_DEF).
    * This is what the USER ASKED FOR; what is applied is this snapped to an
    * achievable rate.  Only consulted when net_pace_match == 1. */
   int  net_session_fps_x100;
   /* `net_session_fps_snap` — 1 = default: snap the request to the nearest
    * whole-vblank rate (59.94/N), the only rates a console holds without
    * needing spare headroom inside a vblank.  0 = apply the request verbatim
    * via the fractional accumulator, which averages correctly ONLY if frames
    * genuinely fit in one vblank.  Exists so the field can A/B "steady 29.97"
    * against "nominally 40, actually 35-38 with jitter" — that comparison is
    * the reason ADR-0035 exists and it should stay possible. */
   int  net_session_fps_snap;
   /* Per-phase frame bracket (phase 5h, `EVT core_phase`).  0 = off,
    * 1 = coarse (~10 clock reads/frame), 2 = default: + per-scanline video,
    * GBC mixing and dynarec compilation (~350 reads/frame), 3 = + the
    * direct-sound FIFO drain (~1250 reads/frame).  A clock read is ~1 us on
    * Allegrex, so the level IS the probe budget; 1 vs 2 is the A/B that
    * prices the probe by measurement instead of by assertion. */
   int  core_phase;
   /* Staging-buffer placement for the GU blit (ADR-0034, VID_BLIT_*).
    * 0 = cached RAM + an 82 KiB cache writeback every frame (the original
    * path), 1 = the same RAM through the uncached mirror, **2 = VRAM above
    * the display buffers — the DEFAULT, decided by hardware**:
    *
    *              stage    gu    tot   (PSP-1000, us/frame)
    *   0 cached    1337   1176   2513
    *   1 uncached  1041   1174   2215
    *   2 vram      1071    728   1800
    *
    * VRAM saves 713 us/frame against cached, and — against the prediction —
    * most of that is `gu`, because the GE reads its source texture out of
    * VRAM far faster than out of main RAM.  0 and 1 are kept for the A/B. */
   int  blit_mode;
   /* ADR-0040 `gu_defer`: 0 block on the GE inside the blit (historical,
    * DEFAULT), 1 let the list run and sync just before the swap, after the
    * vblank wait has already absorbed most of it.  Worth the most during a
    * clamped session, where the loop idles ~9 ms; A/B it there, not solo. */
   int  gu_defer;
   /* ADR-0042 `rfu_rx_cap`: most RFU packets a JOINING console's emulated
    * adapter will hand the game in one frame.  0 = unlimited = historical
    * behaviour and the DEFAULT.  The field crash correlates with bunched
    * delivery (6/frame vs the rig's 2-3) and the game's receive queue
    * latching full is an unrecoverable error, so 2-3 is the interesting
    * setting.  Held packets wait one frame in our own queue; they are never
    * dropped.  No effect on a hosting console. */
   int  rfu_rx_cap;
   /* ADR-0075b `rfu_pace_max_hold`: how many frames of host data the paced
    * client may hold before the OLDEST is discarded.  This is the latency
    * dial, and it is a config.ini key rather than a harness one because the
    * measurement that set it can only be made by a person playing.
    *
    * Why it matters more than it looks: with the pace gate delivering one
    * packet per frame and the game asking about once per frame, arrival and
    * delivery are equal -- so ANY depth the queue reaches is a fixed point and
    * it never drains on its own.  The cap is the only thing that drains it,
    * which means the cap IS the standing latency.  Measured on hardware at 12:
    * q_hi parked at 12 for the whole session (~210 ms behind) with only 39
    * discards, and the player reported constant lag that no longer grew.
    *
    * Shedding it costs a one-time handful of discards; once the queue sits at
    * the cap, arrival == delivery holds it there with no further loss.  Lower
    * is more responsive, higher absorbs more network stall (the worst observed
    * inter-packet gap is 32 ms, ~2 frames).  Must stay well under the game's
    * 32-FRAME link timeout; 0 = unbounded = the build that died at 39. */
   int  rfu_pace_max_hold;
   /* Let config.ini's `net_session_fps` win over the trading profile's forced
    * rate in a PLAYABLE build.  0 = profile decides (57.00 speed / 29.97
    * compat), 1 = the ini value is used verbatim.
    *
    * 57.00 was chosen for a worst case that no longer holds: the note on it
    * says "the join is ~2 fps short of nominal, so every pair pays 2.73 fps".
    * With the ME renderer, dirty pages and same-frame the join now holds 57.00
    * with skipped=0 and a worst frame of 14.7-16.8 ms against a 17.54 ms
    * budget -- so nominal (59.73) is worth trying, and a 16.74 ms budget is
    * the test of whether that worst frame still fits.
    *
    * SET IT IDENTICALLY ON BOTH CONSOLES.  Gen-3's RFU counts link timeouts
    * in FRAMES; two consoles that disagree about how long a frame is are the
    * one thing it cannot survive, which is what the forced rate was
    * protecting against. */
   int  net_session_fps_force;
   /* ARQ retransmit floor, microseconds.  0 = the build-time ND_RTO_MIN_US
    * (200,000 here, set on the psp/Makefile command line).
    *
    * WHY IT IS NOW A PLAYER KEY.  The floor was chosen when srtt was ~49 ms,
    * where 200 ms is a sane 4x margin.  The playable build's idle-poll took
    * srtt to ~11 ms, so the floor is now 18x srtt -- and because the channel
    * is reliable ORDERED, one lost packet stalls everything behind it for a
    * whole RTO.  Measured at a battle start, hardware 2026-09-08: retx 13 ->
    * 33 in one window, rto_us backing off 200,000 -> 400,000, a payload
    * resent 600 ms after it was queued, the host logging rfu_noresp 1..8 and
    * then giving up.  The client's own answers were healthy (mean 1.7 ms) and
    * the pace queue was only 11 deep -- nothing was slow except waiting out
    * the RTO.
    *
    * 50,000 keeps the same 4-5x margin over the srtt we actually have now.
    * Careful going much lower: a floor near srtt retransmits packets that
    * were merely in flight, and the earlier ARQ campaign measured a duplicate
    * storm (dup 2 -> 5) at 40 ms when srtt was 49 ms. */
   int  nd_rto_min_us;
   /* config.ini gblink_delay as written, -1 when absent: main_psp.c owns the
    * default and range (it used to read the file again for this one key). */
   int  gblink_delay_cfg;
   /* STANDBY (sleep/wake).  0 = off, the historical behaviour: the console
    * cannot wake with the Media Engine running and comes back to a black
    * screen.  1 = tear the engine down on suspend and rebuild it on wake.
    *
    * ON by default as of 2.0: validated on the PSP-1000, 3000 and Go.  It was
    * off while it was being proven, because the failure mode is a console
    * that needs a battery pull and a Go has no battery to pull -- but the
    * proving is done, and a sleep feature nobody switches on is not a
    * feature.
    *
    * Safety is structural rather than conservative: with this on, the Media
    * Engine refuses to start unless it can take the kernel's SceMeRpc system
    * event handler, which is what parks the second core on suspend.  A
    * console that cannot do that falls back to single-core rendering -- slow,
    * but it will never hard-reset when someone closes the lid. */
   int  standby;
   /* "Quit to game list" returns to the browser IN THIS PROCESS when the
    * unload is proven exact (docs/INPROC-RETURN.md).  1 = yes (default);
    * 0 = the kill switch: always relaunch the EBOOT, the 3.1 behaviour. */
   int  quick_return;
   /* Load the Media Engine kernel module at all.  1 = yes (default, and what
    * every performance measurement assumes); 0 = never touch the second core,
    * CPU rendering only.
    *
    * Distinct from `me_mode`, which only decides whether the RENDERER uses it:
    * with me_mode = 0 the module is still loaded and handshaked.  Separated
    * because "has this process ever loaded a kernel module" is exactly the
    * question standby resume turns on, and nothing could ask it before. */
   int  me_boot;
   /* Session overlay chip (bottom-right "linked/hosting" status) while a
    * wireless session is live.  1 = shown (default), 0 = hidden.  The
    * WLAN-off warning chip is NOT gated by this: hiding the reason wireless
    * is broken costs a field-debug round trip. */
   int  osd_wireless;
   /* "Fast link" (Wireless menu; docs/GBA-LINK-UI.md): 1 = a GBA Link Cable
    * session this console HOSTS sends two frames of buttons per packet.
    * (Acks ride in every running cable session, Fast link or not:
    * nd_ack_ride_us, main_psp.c.)  0 (default).  As a guest the host's value is used and
    * this one is neither read nor rewritten. */
   int  fast_link;
   char group[9];     /* wireless room code (adhocctl group, <= 8 chars) */
   char nick[24];     /* wireless nickname */
   int  console;      /* FE_CONSOLE_* browser/core selection; defaults to GBA */
   char last_rom[320]; /* browser preselect -- holds the longest relative
                       * ROM path accepted by the browser (319 bytes), so it
                       * has to fit a folder prefix plus a
                       * full No-Intro name.  64 could not even hold the
                       * name alone; the only symptom was the browser
                       * quietly opening on the first game instead of the
                       * last one played. */
   /* User control remapping (Settings > Controls, docs/CONTROL-REMAP.md).
    * CONFIG.INI `bind_*` keys, validated on load; a config without them --
    * every config before this existed -- resolves to the 3.0 layout.  The
    * old `btn_swap` key (3.0's "A/B buttons" row, now removed) is converted
    * into bind_a/bind_b at load and has no field of its own. */
   ctl_map controls;
#ifdef VITA
   /* THE PS VITA'S OWN KEYS (vita/ui_host_hooks.h section 8).  The Play
    * together pages write them; vita/link_vita.c reads them. */
   int  net_conn;       /* UI_CONN_*: 0 Wi-Fi (default), 1 Internet, 2 ad hoc */
   int  link_delay;     /* 0 = Auto (default), 1..8 frames of input delay  */
   char net_addr[PCFG_NET_ADDRS][24];  /* the last host addresses typed,
                                        * newest first; "" = unused        */
   int  local_partner;  /* local link: 0 = the partner starts from a copy
                         * of my save (default), 1 = from its own save file */
   int  local_layout;   /* VID_PAIR_SIDE (default) / VID_PAIR_INSET         */
   /* 1 (default): the menus answer the front panel.  0 = they ignore it --
    * the switch for a panel that misbehaves.  Read; written only when the
    * key is already in the file. */
   int  touch;
   int  touch_keep;
   /* REWIND (vita/rewind_vita.h; Settings > Rewind).  `rewind` = seconds
    * of history kept, 0 = off, one of pcfg_rewind_steps[] (anything else in
    * the file becomes the nearest).  `rewind_mb` = the memory it may take,
    * in MiB: read, clamped, and written only when the key is already in the
    * file -- a number for whoever tunes the build, not a setting. */
   int  rewind;
   int  rewind_mb;
   int  rewind_mb_keep;
   /* How many more games begin with the two lines a new player needs (how
    * the menu opens, how to rewind): 3 on a fresh install, counted down by
    * vita/main_vita.c, 0 for ever after. */
   int  hints;
   /* START and SELECT wait a moment before the game sees them, so a
    * shortcut chord (SELECT+L, START+SELECT...) does not also press them in
    * the game (vita/main_vita.c chord_guard).  1 by default; 0 = straight
    * through, as on the PSP. */
   int  chord_guard;
   /* The live console's `color`, `blend` and `wide` (pcfg_display),
    * mirrored like scale and filter. */
   int  color, blend, wide, wide_fill;
   int  sgb_border;     /* the GB profile's Super Game Boy border (pcfg_display) */
   int  quality;      /* 0 legacy auto; 1 native; 2/3 enhancement factor */
#endif
} psp_config;

#ifdef VITA
/* Settings > Rewind's choices, in seconds: Off, 30 s, 1, 3 and 5 minutes. */
#define PCFG_REWIND_STEPS 5
extern const int pcfg_rewind_steps[PCFG_REWIND_STEPS];
#define PCFG_REWIND_DEFAULT 60
#define PCFG_REWIND_MB_DEFAULT 24
#define PCFG_LINK_DELAY_MAX 8
/* Remember a host address the player typed (CONFIG.INI net_addr1..4):
 * newest first, no duplicates, the oldest falls off.  Marks nothing dirty:
 * the caller saves. */
void pcfg_net_addr_push(const char *addr);
#endif

/* Bounds for `net_session_fps` (ADR-0033), in hundredths of a frame/second.
 * The ceiling is the GBA's own 59.7275 Hz — clamping ABOVE nominal is not a
 * clamp.  The floor is where the audio pitch drop (which follows the pace by
 * design, ADR-0027 §audio) stops being a tape slowdown and starts being a
 * different instrument; a session that needs less than 20 fps has a problem
 * pacing cannot fix.
 *
 * **THE DEFAULT IS 29.97, NOT 40.00 (ADR-0035).**  We pace by inserting whole
 * PSP vblanks, so the only rates a console can hold REGARDLESS of per-frame
 * work are 59.94/N.  The field measured 35-38 fps achieved against a 40.00
 * target: 40 needs about half the frames to fit inside one 16.68 ms vblank,
 * and when per-frame work sits just above that, none do.  29.97 = exactly two
 * vblanks, which needs no headroom at all.  Requests are SNAPPED to the
 * nearest achievable rate and the snap is logged — see pace_snap_x100(). */
/* SHIPPING DEFAULT: 29.97. Do not raise this without oracle-verified runs.
 *
 * 45.00 was promoted here on 2026-08-07 on the strength of 8/8 trades, and a
 * second night falsified it: 12/16 across both nights (0.75), with runs dying
 * on the frozen-client signature. The claim this comment used to make -- "a 50%
 * speed-up at no measured cost in reliability" -- was FALSE, and was written
 * from a single night's sample.
 *
 * Trade success is a SHOULDER, not a cliff (Logs/CLIFF-FINDINGS.md):
 *
 *     29.97 -> 1.00 (16/16)    45.00 -> 0.75 (12/16)    47.00 -> 0.50 (4/8)
 *     49.00 -> 0.00 (0/8)      53.00 -> 0.00 (0/8)      59.73 -> 0.00 (0/16)
 *
 * Only 29.97 is bulletproof. Everything above it is a reliability trade, so the
 * shipping default must be the reliable one until the mechanism is understood.
 *
 * Best current explanation (well-motivated, NOT proven): every adapter timeout
 * is counted in EMULATED CPU CYCLES (rfu.c:1087 -> main.c:844) while the radio
 * is wall clock, so what the game experiences is link latency measured in
 * EMULATED FRAMES -- 1.05 / 2.48 / 2.63 / 2.92 / 3.69 / 23.85 for the rates
 * above, which orders every outcome correctly. It does not yet predict
 * individual runs. */
#define PCFG_SESSION_FPS_DEF  2997
#define PCFG_SESSION_FPS_MIN  2000
#define PCFG_SESSION_FPS_MAX  5973

extern psp_config g_pcfg;

enum { PCFG_FF_3X, PCFG_FF_UNLIMITED, PCFG_FF_SMOOTH, PCFG_FF_COUNT };
int pcfg_ff_mode(void);
void pcfg_ff_set_mode(int mode);
const char *pcfg_ff_name(void); /* shared by settings and OSD */

/* Reads a decimal frame rate ("40", "40.5", "40.00") from `ini` as hundredths,
 * clamped to PCFG_SESSION_FPS_MIN..MAX.  Returns `def` when the key is absent
 * or unparseable.  Shared with the harness override in main_psp.c so the
 * harness and config.ini cannot disagree about what "40.00" means. */
int  pcfg_fps_x100(const char *ini, const char *key, int def);

/* CONFIG.INI IS READ ONCE AND WRITTEN ONCE (fe_ini_doc, fe_util.h): every
 * key below is the same fe_ini_get/fe_ini_set call it always was, answered
 * from one in-memory copy that is bound to the path while these run.
 *
 * pcfg_load_doc takes a document the caller already loaded (main() loads it
 * first thing, so boot reads the file ONCE);
 * NULL, or a document that failed to load (a file over FE_INI_DOC_CAP),
 * falls back to the per-key file reads, which answer the same.  pcfg_load
 * loads its own. */
void pcfg_load(const char *ini_path);
void pcfg_load_doc(const char *ini_path, fe_ini_doc *doc);
/* Rewrites all keys to the load path: ONE atomic write (path.tmp, renames),
 * nothing at all when no value changed.  A file too big to hold is left
 * untouched (config_save_refused). */
void pcfg_save(void);
/* GB/GBC clock saves: the local time (seconds since 1970) this console
 * first ran a release that stamps them in UTC -- stored on the first call
 * with a clock reading, returned unchanged ever after.  If it cannot be
 * stored, now_local (and it is asked again next boot).  0 = no reading. */
long pcfg_gb_rtc_utc_since(long now_local);
/* Browser selection changes only this key. Returns -1 on persistence failure. */
int pcfg_remember_rom(const char *name);
/* Browser console switch (TRIANGLE): sets g_pcfg.console and writes only the
 * `console` key.  Returns -1 on an invalid value or persistence failure. */
int pcfg_remember_console(int console);

/* ---- PER-CONSOLE DISPLAY PROFILES (docs/DISPLAY-FEATURES.md) ------------
 *
 * GBA, GB and GBC each keep their own picture settings.  CONFIG.INI:
 *
 *     scale_gba / scale_gb / scale_gbc         VID_SCALE_*
 *     filter_gba / filter_gb / filter_gbc      VID_FILTER_*
 *     ambient_gba / ambient_gb / ambient_gbc   PCFG_AMB_*
 *     gb_palette_gb / gb_palette_gbc           GBCORE_PALETTE_* id
 *
 * MIGRATION: a key that is absent takes the pre-profile value -- `scale`,
 * `filter`, `gb_palette` -- so the first boot after an upgrade looks exactly
 * like the last one on every console.  The legacy keys are still WRITTEN
 * (scale/filter from the GBA profile, gb_palette from the GB one) so an older
 * build reading this file keeps the GBA look it had.
 *
 * g_pcfg.scale / filter / ambient / gb_palette are the live mirror of ONE
 * profile, the "live console" (g_pcfg.console whenever it was last selected).
 * Everything that already reads those fields keeps working unchanged; code
 * that CHANGES one calls pcfg_display_commit() so the profile follows. */
enum
{
   PCFG_AMB_OFF      = 0,   /* black bars (the 3.1 look)                    */
   PCFG_AMB_ART      = 1,   /* blurred hero art; black when there is none   */
   PCFG_AMB_ART_GAME = 2,   /* hero art, else a blurred early game frame    */
   PCFG_AMB_MODES    = 3
};
/* Off until the owner picks a look (Fable's mockups). */
#define PCFG_AMBIENT_DEF  PCFG_AMB_OFF

typedef struct
{
   int scale, filter, ambient, gb_palette;
#ifdef VITA
   /* COLOURS and MOTION BLEND (vita/video_vita.h vid_set_look), CONFIG.INI
    * color_gba / color_gbc and blend_gba / blend_gb / blend_gbc:
    *   color  GBA: 0 as the game sends them, 1 as a GBA's screen showed
    *               them, 2 the same and as dark as the unlit screen was;
    *          GBC: 0 as sent, 1 as a Game Boy Color's screen showed them
    *               (the default; an absent key takes gbc_color_correction);
    *          GB:  none -- an original Game Boy has its palette row.
    *   blend  1 = each frame mixed with the one before, as the slow LCD
    *          did (what games that flicker to fake transparency rely on). */
   int color, blend, quality;
   /* WIDE VIEW (gpsp/wide.c), CONFIG.INI wide_gba: more of the game beside
    * the console's 240 pixels instead of bars.
    *   0 off; 1 in the games it has a profile for (the default);
    *   2 also wherever a rotated/scaled background goes on by itself;
    *   3 also, in any other game, what the game has already drawn of the
    *     places beside the screen.
    * A Game Boy has none.
    * wide_fill_gba: what the margins show where a game has no more (a
    * menu): 1 its own edges continued softly (the default), 2 the picture
    * stretched, 0 black bars. */
   int wide, wide_fill;
   /* SUPER GAME BOY BORDER (CONFIG.INI sgb_border_gb, the GB profile's):
    * 1 = a game running as a Super Game Boy (GB palette Auto) is shown in
    * the border it sends (the default), 0 = the picture alone. */
   int sgb_border;
#endif
} pcfg_display;
#ifdef VITA
enum { PCFG_QUALITY_AUTO, PCFG_QUALITY_ORIGINAL, PCFG_QUALITY_2X,
       PCFG_QUALITY_3X, PCFG_QUALITY_MODES };
const char *pcfg_quality_name(int quality);
/* How many `color` values a console has (GBA 3, GBC 2, GB 0). */
int pcfg_color_modes(int console);
/* `wide` values of a console (GBA 3, the others 0) and their names. */
#define PCFG_WIDE_DEF 1
#define PCFG_WIDE_FILL_DEF 1
int pcfg_wide_modes(int console);
const char *pcfg_wide_name(int wide);
const char *pcfg_wide_fill_name(int fill);
/* The value's name on the Settings row. */
const char *pcfg_color_name(int console, int color);
#endif

extern pcfg_display g_pdisp[FE_CONSOLE_COUNT];

#ifdef VITA
/* ---- PER-GAME SETTINGS ------------------------------------------------------
 *
 * <data>/games/<ROM file name>.ini, read when a game starts and written when
 * one of its values changes.  Two kinds of value live there:
 *
 *   override=1  the DISPLAY section is this game's own: pcfg_display_select /
 *               commit / save read and write `disp` instead of the console's
 *               profile while the game runs, so a change made in its Settings
 *               never reaches the other games.  Turned on with the current
 *               look copied in (nothing changes on screen); turned off, the
 *               console's own look comes back and the keys are dropped.
 *   sensor_*    the sensor cartridge's controls (fe_sensor.h; ignored by
 *               games without one): how motion is read, how strongly, which
 *               way round, the hold that counts as level, and the light a
 *               light-sensor game sees. */
enum { PCFG_SENSOR_MOTION, PCFG_SENSOR_STICK, PCFG_SENSOR_MODES };
enum { PCFG_SENS_LOW, PCFG_SENS_NORMAL, PCFG_SENS_HIGH, PCFG_SENS_MODES };
#define PCFG_SUN_CLOCK     11   /* the light follows the time of day */
#define PCFG_SUN_DEF        6
typedef struct
{
   int on;                    /* a game is running (pcfg_game_begin)       */
   int console;
   char path[320];
   int override;
   pcfg_display disp;
   int sensor_mode, sensor_sens, sensor_rev, sunlight;
   int level_set;             /* level_right/level_down hold a calibration */
   int level_right, level_down;   /* milli-g of the hold counted as level  */
} pcfg_game;
extern pcfg_game g_pgame;
/* Read `path` for the game about to run on `console` and, when it has its own
 * look, select it.  Call after pcfg_display_select(console). */
void pcfg_game_begin(const char *path, int console);
/* Leave the game: its settings are written if changed, the console's own
 * profile is selected again.  Safe when no game ran. */
void pcfg_game_end(void);
/* Write the file now (0 = written or nothing to write). */
int  pcfg_game_save(void);
/* Give the running game its own look (1, starting from the current one) or
 * hand it back to the console's profile (0).  Selects the result. */
void pcfg_game_set_override(int on);
const char *pcfg_sensor_mode_name(int m);
const char *pcfg_sens_name(int s);
#endif

/* Mirror `console`'s profile into g_pcfg (scale/filter/ambient/gb_palette)
 * and make it the live console.  Touches no file and no video state: the
 * caller applies it (vid_set_mode etc.) when a picture is on screen. */
void pcfg_display_select(int console);
/* Copy g_pcfg's live display fields back into the live console's profile. */
void pcfg_display_commit(void);
/* The console whose profile g_pcfg currently mirrors. */
int  pcfg_display_console(void);
/* Persist the live console's display keys, seeding absent scale/filter/quality
 * profile keys once on Vita before legacy mirrors can change their defaults.
 * Existing keys of other consoles are never replaced. Used by the in-game
 * TRIANGLE preset cycle. Commits first.
 * Returns -1 on a persistence failure. */
int  pcfg_display_save(void);
/* "GBA" / "GB" / "GBC" -- the settings section names the profile it edits. */
const char *pcfg_console_tag(int console);
const char *pcfg_ambient_name(int mode);

#ifdef __cplusplus
}
#endif

#endif /* CONFIG_PSP_H */
