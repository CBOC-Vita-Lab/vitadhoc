/* app_vita.h -- what the Vita app's own files share with each other.
 *
 *   main_vita.c    boot, the game list, the game loop, menus, save states
 *   link_vita.c    THE NETWORK: Wi-Fi rooms, a direct address, ad hoc; the
 *                  session's life and the LINKING screen.  Implements part
 *                  A below and ui_host_hooks.h sections 3-4.
 *   local_vita.c   THE LOCAL LINK: both machines on this console.
 *                  Implements part B below and ui_host_hooks.h section 5.
 *   stubs_vita.c   a weak do-nothing definition of every function of parts
 *                  A and B, so the app links with neither file, with one,
 *                  or with both; a real definition simply wins
 *
 * The two link files do not call each other and main_vita.c needs no edit
 * when either arrives: everything they do happens through the calls listed
 * here, which main_vita.c already makes.  Nothing here is an interface to
 * the PSP frontend's files: those are psp/ui_psp.h, psp/osd_psp.h,
 * psp/config_psp.h and vita/ui_host_hooks.h.
 *
 * ============================================================================
 * THE CALL ORDER (main_vita.c).  "net:" is part A, "local:" part B.
 * ============================================================================
 *   boot      link_boot()                       once, config.ini is read
 *   per game  link_host_config(&cfg)            before fe_host_boot
 *             [fe_host_boot, cheats_load]
 *             link_game_begin()
 *             link_local_game_begin()
 *   every iteration of the game loop, menu open or not:
 *             g_app_pad, g_app_keys are set (the pad; the pad through the
 *                control map)
 *             local:  link_local_pads(keys1, keys2, have2)   while
 *                     link_local_active(); it may change g_app_keys
 *             net:    link_frame()
 *             local:  link_local_frame()
 *     menu open:
 *             link_ui_view(&v); link_local_ui_view(&v); ui_set_link(&v)
 *             ui_frame(pad, link_locks_states(), link_session_info())
 *             its actions:
 *               UI_ACT_NET_HOST        link_action(1)     } refused by
 *               UI_ACT_NET_JOIN        link_action(0)     } main_vita.c
 *               UI_ACT_NET_DISCONNECT  link_disconnect()    while
 *                                                           link_local_active()
 *               UI_ACT_NET_LOCAL       link_local_start()   refused while
 *                                                           link_net_busy()
 *               UI_ACT_NET_LOCAL_STOP  link_local_stop()
 *               UI_ACT_NET_LOCAL_SWAP  link_local_next_view()
 *     menu closed:
 *             a tap on the screen:   local: link_local_tap(nx, ny) while
 *                                    link_local_active()
 *             the right stick, flicked left or right (one pad or two):
 *                                    local: link_local_next_view()
 *             states (quick or from the menu) are refused while
 *                link_locks_states(); while link_local_active(), after a
 *                state SAVE:         local: link_local_state_saved(file)
 *                and after a LOAD:   local: link_local_state_loaded(file)
 *             fast forward is off while link_session_up() (the local link
 *                fast-forwards both machines); the pause overlay while
 *                link_session_up()
 *             fe_host_run_frame() -> the audio sink:
 *                  link_local_active()  local: link_local_audio(samples)
 *                                       may take the frame's sound
 *             ...and the video sink (picture ownership, in this order):
 *                  link_local_active()  local: link_local_present(frame)
 *                  link_owns_picture()  nobody draws the game
 *                  else                 main_vita.c: vid_draw_frame
 *             net:  link_draw()         the LINKING screen, over the game
 *     while the pacer waits for the refresh:
 *             net:  link_idle_pump()
 *   on the I/O worker thread (io_step), while a game is loaded:
 *             local: link_local_io()
 *   suspend / resume, whatever screen is up:
 *             link_power(0 | 1); link_local_power(0 | 1)
 *   the game is unloaded (quit to the list, exit, app_quit_game):
 *             link_local_game_end()             first: it ends the pair
 *             link_game_end()
 *             [fe_host_shutdown]
 *   exit      link_shutdown()
 *
 * CHEATS.  cheats_vita.h asks that every code leaves the machine for a
 * NETWORK session, from before the session's machines start until the
 * single game is back.  Only the link layer knows those two moments exactly
 * (a frame late is a desync), so link_vita.c calls cheats_suspend(1) and
 * cheats_suspend(0) itself; main_vita.c does not.  The local link does not
 * suspend: codes act on the player's own machine.
 */
#ifndef GBADHOC_APP_VITA_H
#define GBADHOC_APP_VITA_H

#include <stddef.h>
#include <stdint.h>

#include "fe_console.h"
#include "fe_host.h"
#include "ui_psp.h"          /* ui_link_view */

#ifdef __cplusplus
extern "C" {
#endif

#ifndef GBADHOC_VERSION
#define GBADHOC_VERSION "dev"
#endif

#define APP_PATH_CAP 512

/* ---- main_vita.c --------------------------------------------------------- */

/* The running game. */
typedef struct app_game
{
   fe_console_t console;
   char rom_path[APP_PATH_CAP];
   char save_path[APP_PATH_CAP];
   char state_path[APP_PATH_CAP];     /* slot 1; psp/state_slots.h has the rest */
   char boot_path[APP_PATH_CAP];      /* what the cores load: rom_path, or its
                                       * patched copy (rom_patch.h)          */
   int  patched;
   char code[5];                      /* GBA header code (0xAC), "" on GB/GBC   */
   char gb_title[17];                 /* GB cartridge title (0x134)             */
   char key[100];                     /* the ROM's name under roms/             */
   char name[64];                     /* a name for screens                     */
   long rom_bytes;
} app_game;
extern app_game g_game;

extern char g_app_root[APP_PATH_CAP];     /* ux0:data/GBAdhoc                  */
extern char g_app_rom_dir[APP_PATH_CAP];  /* <root>/roms                       */
extern char g_app_config[APP_PATH_CAP];   /* <root>/config.ini                 */

/* This iteration's pad, PSP bit layout (pspcompat_pad). */
extern unsigned g_app_pad;
/* What the player's machine is handed this frame: the libretro joypad mask
 * the control map makes of the pad (ctl_game_mask), set before
 * link_local_pads() and link_frame() and read by the core during the frame.
 * Either may change it -- the local link clears it while the one pad drives
 * the partner's machine. */
extern uint32_t g_app_keys;
/* The loop's frame clock: when this iteration began, and the refresh the
 * frame is meant for (link_vita.c's stall absorption reads both). */
extern uint64_t g_app_frame_t0;
extern unsigned g_app_vc_target;
/* Something drew a whole frame this iteration (the game, a link screen).
 * link_local_present() and link_draw() are accounted by main_vita.c; a link
 * file that draws a whole frame from anywhere else sets it. */
extern int g_app_drew;

/* The frame the player was looking at, as the UI wants it (PSP-order 565 at
 * a 256-texel stride): taken for the menu's backdrop and the LINKING screen.
 * Returns 1 when there is one. */
int  app_snapshot_take(void);
void app_backdrop_from_snapshot(void);     /* ui_set_backdrop(snapshot) */
/* The last frame the core presented (libretro RGB565), or NULL. */
const uint16_t *app_last_frame(unsigned *w, unsigned *h, size_t *pitch_bytes);
/* The directory gba_bios.bin is read from (the data root, or the application
 * itself when the build carries one): fe_host's system_dir, and what the
 * link's compatibility word must hash. */
const char *app_bios_dir(void);
/* The single machine's frame is gone (a session took the cartridge). */
void app_frame_forget(void);
/* The frame as it is shown: the same, or with the wide picture's margins
 * (gpsp/wide.h); app_last_frame is always the console's own picture. */
const uint16_t *app_shown_frame(unsigned *w, unsigned *h, size_t *pitch_bytes);
/* The wide picture follows the Settings row, the console and the pair. */
void app_wide_apply(void);

/* Sound: what is queued belongs to another timeline. */
void app_audio_flush(void);
/* The pacer: do not count the time just spent as a late frame. */
void app_pace_reset(void);

/* A fatal condition inside a session's reload: leave the game cleanly.  The
 * game is unloaded (its save flushed) at the end of this iteration and the
 * player is back in the game list; `why`, when not empty, is shown to him on
 * the way ("The game was closed" / why). */
void app_quit_game(const char *why);

/* fe_host_config's stall-absorption budget (see link_vita.c). */
unsigned app_vcount(void);

/* ============================================================================
 * PART A -- THE NETWORK                                        (link_vita.c)
 * Also this file's: ui_host_hooks.h section 3 (ui_host_net_status) and
 * section 4 (ui_host_rooms_begin / _poll / _end), whose weak defaults are in
 * ui_hooks_default.c.
 * ========================================================================== */

/* How the consoles reach each other: config.ini net_conn, g_pcfg.net_conn.
 * The same numbers as ui_host_hooks.h's UI_CONN_* (checked there). */
#define LINK_CONN_WIFI      0   /* same Wi-Fi network / hotspot: rooms are found */
#define LINK_CONN_INTERNET  1   /* the joiner types the host's address           */
#define LINK_CONN_ADHOC     2   /* no router: console to console                 */
#define LINK_CONN_COUNT     3

/* Once, after the configuration is loaded (net_vita_init...). */
void link_boot(void);
/* For Share game data (vita/share_vita.c): 1 when the network stack is up,
 * and the rooms' base port (vita.ini link_port; 0 = the default). */
int  link_net_info(uint16_t *disc_port);
/* At exit. */
void link_shutdown(void);

/* fe_host's link hooks for this boot, before fe_host_boot: the stall
 * absorption, the bulk lane, the partner-cartridge lookup.  main_vita.c has
 * already asked for the second GBA machine (cfg->gba_dual, _session). */
void link_host_config(fe_host_config *cfg);
/* The game is up: apply the adapter tuning. */
void link_game_begin(void);
/* The game is going away: end any session, give the radio back.  May not
 * block for long: the "Returning to game list" picture is on screen. */
void link_game_end(void);

/* The menu's actions (ui_host_hooks.h section 7 says what each expects).
 * link_action(1): host -- the menu stays open on its waiting page and reads
 *    the progress from link_ui_view; nothing may block.
 * link_action(0): join *ui_join_target() -- the menu has closed; the
 *    LINKING screen is this file's (link_draw).
 * link_disconnect(): stop hosting, cancel a bring-up, or end the session by
 *    agreement. */
void link_action(int is_host);
void link_disconnect(void);

/* Once per iteration, before the frame runs (menu open or not). */
void link_frame(void);
/* After the frame ran, menu closed: draw the LINKING screen when a session
 * is setting up or has just ended (ui_linking_draw over
 * app_backdrop_from_snapshot; OR ui_linking_tapped() into the pad's edges).
 * Returns 1 when it drew: the game's frame is covered. */
int  link_draw(void);
/* While the pacer waits for the blank: look at the network again. */
void link_idle_pump(void);
/* The console is about to suspend (0) / has resumed (1). */
void link_power(int resumed);

/* A network session's machines are running or being set up: fast forward
 * and the pause overlay are off. */
int  link_session_up(void);
/* Anything of the network is in use -- from a Host or Join action until the
 * radio has been given back, the "hosting, nobody there yet" wait included.
 * The local link cannot start then (main_vita.c refuses; local_vita.c reads
 * it for ui_host_local_state().available). */
int  link_net_busy(void);
/* Save and load states are refused (a session's state is not in them).  Not
 * while a host only waits: the link starts from the live game. */
int  link_locks_states(void);
/* The game's frame must not be drawn by main_vita.c this iteration (the
 * LINKING screen is up, or the session presents the picture itself). */
int  link_owns_picture(void);
/* The session's one-line status, or NULL when there is nothing to say: the
 * in-game menu's status line and the OSD's session chip.  The chip is set
 * from it by osd_session_chip_refresh() (main_vita.c; ui_host_hooks.h) --
 * call that when the line changes. */
const char *link_session_info(void);
/* The session as the menus draw it: fill *v (main_vita.c has zeroed it, with
 * rtt_ms and signal_pct at -1, and calls ui_set_link afterwards).  Strings
 * must stay valid until the next call.  v->error: why the last Host did not
 * work (ui_link_fail_text).  It may be reported for as long as that is the
 * last thing that happened here: main_vita.c shows a reason only from the
 * action that asked (link_action(1) for this one, link_local_start for the
 * local file's) until the menu's next action. */
void link_ui_view(ui_link_view *v);

/* ============================================================================
 * PART B -- THE LOCAL LINK: both machines on this Vita         (local_vita.c)
 * Also this file's: ui_host_hooks.h section 5 (ui_host_local_state), whose
 * weak default is in ui_hooks_default.c.
 *
 * The pair is frontend-common/fe_gbadual.h's fe_gbadual_local_*: the
 * player's machine (instance A) carries on, the partner's (instance B) is
 * drawn and heard too.  main_vita.c keeps calling fe_host_run_frame(), which
 * steps both.  Instance B's lifetime is not this file's: fe_host_boot brings
 * it up with every GBA game (main_vita.c asks for it) and main_vita.c
 * unloads it with the game, right after link_local_game_end().
 *
 * WHO DRAWS.  While link_local_active(), main_vita.c's video sink does not
 * draw the game: it hands the player's machine's frame to
 * link_local_present(), which draws a WHOLE frame -- both pictures, with
 * vid_draw_frame_pair() in the layout g_pcfg.local_layout (read it every
 * frame; the menu changes it live), the partner's frame being the one its
 * fe_gbadual_local.video callback last kept.  main_vita.c then draws the
 * OSD over it and presents.  The menu's backdrop and the state previews
 * stay the player's machine's frame.
 *
 * WHO FEEDS THE PADS.  main_vita.c reads both controllers and puts each
 * through the player's control map; link_local_pads() gets the two libretro
 * joypad masks every iteration (fe_gbadual_keys_from_retro() makes the
 * partner's KEYINPUT of one).  With a second controller (have2) each pad
 * drives its own machine and there is nothing to decide.  With one, this
 * file owns "which machine does the pad drive" (ui_local_state.controlled):
 * it gives keys1 to that machine and sets g_app_keys to 0 when that is the
 * partner's.  link_local_next_view() switches (the menu's "Swap controlled
 * game", and the right stick flicked sideways); link_local_tap() is a
 * finger on the screen, in native 960x544 pixels: hit-test
 * vid_pair_rects() and drive the machine under it.
 *
 * SOUND.  One machine is heard at a time, the one the pad drives (nothing
 * is mixed): main_vita.c's audio sink offers the player's machine's sound to
 * link_local_audio(), which takes it when the partner's is to be heard and
 * queues that instead (from its fe_gbadual_local.audio callback).
 * ========================================================================== */

/* The game is up (after link_game_begin) / is going away (BEFORE
 * link_game_end: stop the pair, write the partner's save). */
void link_local_game_begin(void);
void link_local_game_end(void);

/* The menu's actions.  Start: how the partner starts is
 * g_pcfg.local_partner (a copy of the player's save, or its own file
 * <rom>.p2.sav); the menu stays open and reads the outcome from
 * ui_host_local_state().active or link_local_ui_view's error. */
void link_local_start(void);
void link_local_stop(void);
/* Both machines are running on this Vita. */
int  link_local_active(void);

/* Once per iteration, after link_frame(), menu open or not (the partner's
 * save file, when its game saved). */
void link_local_frame(void);
/* While link_local_active(), every iteration before the frame runs: the
 * first pad and the second controller, each through the control map
 * (libretro joypad masks, RETRO_DEVICE_ID_JOYPAD_* bits); have2 = a second
 * controller exists.  May change g_app_keys (see above). */
void link_local_pads(uint32_t keys1, uint32_t keys2, int have2);
/* From the video sink, with the player's machine's frame (libretro RGB565):
 * draw both machines. */
void link_local_present(const uint16_t *pix, unsigned w, unsigned h,
                        size_t pitch_bytes);
/* The next view: which machine the one pad drives (and which is large). */
void link_local_next_view(void);
/* A tap at native x,y while the menu is closed: 1 = it was on the other
 * machine's picture, which the pad now drives; 2 = on the picture already
 * in hand (the main loop opens the menu, as a tap does with one game);
 * 0 = beside both. */
int  link_local_tap(int x, int y);
/* While link_local_active(): the player's machine's state was just written
 * to / loaded from `state_file` (a slot's file, psp/state_slots.h).  Saved:
 * the partner's machine and the cable go beside it (<state_file>.p2).
 * Loaded: the pair continues from that when it is there and belongs to this
 * state; else the cable is plugged in again on the reloaded game. */
void link_local_state_saved(const char *state_file);
void link_local_state_loaded(const char *state_file);
/* From main_vita.c's audio sink while link_local_active(): the player's
 * machine's sound for the frame that just ended.  1 = taken (the partner's
 * is being heard instead, or a change between the two is fading): do not
 * queue it. */
int  link_local_audio(const int16_t *stereo_lr, size_t frames);
/* On the I/O worker (main_vita.c's io_step), every pass: write the
 * partner's save file if one is waiting.  Never called where there is no
 * worker; the file is then written on the emulation thread. */
void link_local_io(void);
/* The console is about to suspend (0) / has resumed (1). */
void link_local_power(int resumed);
/* After link_ui_view(v): v->error = why the last local start did not work
 * (ui_link_fail_text(UI_FAIL_LOCAL...)), or leave *v alone.  How long it is
 * shown is main_vita.c's business, as for link_ui_view. */
void link_local_ui_view(ui_link_view *v);

#ifdef __cplusplus
}
#endif

#endif /* GBADHOC_APP_VITA_H */
