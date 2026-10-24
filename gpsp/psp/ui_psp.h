/* ui_psp.h — native UI v1 (plan §8, Phase 2): ROM browser, in-game menu
 * (Select+Start hold — chord detected by the main loop), wireless panel
 * (Host / Join via scan or room code), settings screens.
 *
 * The main loop owns the core; the UI returns actions for it to execute.
 * While ui_active(), the core is paused (no retro_run) but the wireless
 * pump keeps running.  All drawing goes through video_psp overlay
 * primitives; one ui_frame() call per displayed frame.
 */
#ifndef UI_PSP_H
#define UI_PSP_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include "fe_console.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum
{
   UI_ACT_NONE = 0,
   UI_ACT_RESUME,          /* close the menu */
   UI_ACT_SAVESTATE,
   UI_ACT_LOADSTATE,
   UI_ACT_NET_HOST,        /* host on ui_group() */
   UI_ACT_NET_JOIN,        /* join ui_group() */
   UI_ACT_NET_DISCONNECT,
   /* Mystery Gift (2.0.5) is deliberately NOT a host/join variant.  It is a
    * different radio mode -- infrastructure Wi-Fi to the phone's hotspot, not
    * ad-hoc IBSS -- so it cannot share the session state machine, and the two
    * are interlocked as mutually exclusive.  See psp/mgift_net.h. */
   UI_ACT_NET_MGIFT,       /* start listening for a Mystery Gift Station */
   UI_ACT_NET_MGIFT_STOP,  /* stop listening and drop the association */
   UI_ACT_EXIT,            /* exit-with-flush */
   UI_ACT_GAMELIST,        /* relaunch back to the ROM browser */
   /* ADR-0071: the trading profile changed.  The main loop saves config and
    * RELAUNCHES the EBOOT — the session rate is latched at startup, so there
    * is no honest way to apply it live. */
   UI_ACT_RELAUNCH
#ifdef VITA
   /* THE PS VITA (appended; what each expects of the main loop is written
    * out in vita/ui_host_hooks.h, section 7).  The local link: both machines
    * on this console. */
   ,
   UI_ACT_NET_LOCAL,       /* start it; g_pcfg.local_partner says how */
   UI_ACT_NET_LOCAL_STOP,
   UI_ACT_NET_LOCAL_SWAP   /* the pad drives the other machine */
#endif
} ui_action;

/* The in-game menu draws over the game (docs/UI-OVERLAY.md): the frame it
 * sits on -- a w x h PSP-5650 picture at the top-left of a 256x256 texture
 * (main_psp.c's wake snapshot), or NULL for a flat page.  Set before
 * ui_open(); the pointer is only read while the menu is up. */
void ui_set_backdrop(const uint16_t *frame, int w, int h);
/* Five 64x64 PSP-5650 textures the state-slot screen loads the previews
 * into (main_psp.c carves them from the netdrv arena's tail), or NULL. */
void ui_set_thumb_buffer(uint16_t *buf);
/* Harness `ui_backdrop = <file>`: a raw 480x272 PSP-5650 picture drawn 1:1
 * behind the menu instead of the game, for the pixel comparison with the
 * design model.  Allocates 512 KiB; call before the core loads a ROM. */
void ui_backdrop_inject(const char *rel);
void ui_open(void);
void ui_close(void);
int  ui_active(void);
/* Menu = HOME (docs/CONTROL-REMAP.md section 10): a HOME press while the
 * in-game menu is open.  On the menu's top page it closes the menu, exactly
 * as Resume does; on any other page it is ignored (a sub-page may be in the
 * middle of something -- a capture, a scan -- and O already walks back).
 * Returns 1 when the menu closed. */
int  ui_home_press(void);
/* 1 while Settings > Controls is waiting for the player to press the input
 * for a binding.  The main loop keeps its own always-on chords (the
 * screenshot) quiet meanwhile: the player is pressing them to TEACH them. */
int  ui_capturing(void);

/* Boot-only, main-thread presentation. No worker and no core calls.  The
 * only allocation is a launch without a browser pick (harness, variant):
 * ui_loading_begin then decodes the hero art by file name and FREES it
 * before returning -- i.e. before fe_host_boot (docs/DISPLAY-FEATURES.md). */
void ui_loading_begin(const char *path);
void ui_loading_update(const char *stage, unsigned done, unsigned total);
void ui_loading_finish(int success);
/* Full-screen notice for a boot that cannot continue (memory gate, a save
 * that could not be read). */
void ui_notice(const char *title, const char *l1, const char *l2,
               const char *l3, unsigned hold_ms);
/* Harness `loading_dump = N`: GE-dump the first N loading-screen redraws to
 * log/ge_loading_<n>.bmp (docs/DISPLAY-FEATURES.md screenshots). */
void ui_loading_dump_shots(int n);
/* The console's darkest palette shade (its browser skin's first stripe
 * colour), libretro RGB565 -- the ambient bars' no-art fallback. */
uint16_t ui_console_shade(int console);

/* One frame of menu UI: input edges + draw (vid_overlay_begin(1)..end).
 * pad = raw SceCtrl button mask; session_active gates savestates + shows
 * the wireless status screen; session_info is the status line (or NULL). */
ui_action ui_frame(unsigned pad, int session_active, const char *session_info);
void ui_set_state_base(const char *slot1_path);
int ui_state_slot(void); /* selected slot, 1..5 */
/* The SAVE STATES page (docs/UI-OVERLAY.md section 13) after the main loop
 * acted on its UI_ACT_SAVESTATE / UI_ACT_LOADSTATE (rc 0 = done): a save
 * keeps the page open and reports on its plate; a load closes the menu, or
 * stays and reports if it failed.  1 = the page reported it (no toast). */
int ui_state_save_done(int rc);
int ui_state_load_done(int rc);
/* Harness `shot_probe = 1`: four timed .shot reads as the page first opens,
 * EVT shot_read n= us=. */
void ui_shot_probe_arm(int on);

#ifdef GPSP_CHEATS
/* ---- cheats (docs/CHEATS.md) ---------------------------------------------
 * The cheat list of the game at rom_path (only its file name matters):
 * <app>/cheats/<name minus extension>.cht or .txt, plus the player's choices
 * in <name>.on.  attach = 1 for the game that is RUNNING (the enabled cheats
 * go to the core); 0 only edits the list (the ROM browser).  Returns the
 * number of cheats that are on.  No heap, no stdio. */
int  ui_cheats_load(const char *rom_path, int console, int attach);
void ui_cheats_unload(void);
#endif

/* Group selected by the last Host/Join action (scan pick or room code). */
const char *ui_group(void);

/* ---- link mode: Link Cable vs Wireless Adapter (docs/GBA-LINK-UI.md) ----
 *
 * The running game, once at boot: its 4-byte GBA header code (0xAC; NULL on
 * GB/GBC), its GB title (0x134, NULL on GBA), its key in linkmode.txt (the
 * ROM's name relative to roms/), its display name, and whether this boot
 * can host a Link Cable room at all (fe_host_gba_cable_ready). */
void ui_set_game(const char *gba_code, const char *gb_title,
                 const char *key, int cable_ready);
/* linkmode.txt beside favourites.txt: read once (boot / browser), written
 * on each change from Settings. */
void ui_linkmode_load(const char *rom_dir);
/* What Host does for the running game: UI_HOST_CABLE / UI_HOST_ADAPTER /
 * UI_HOST_ASK (a BOTH game: the HOST chooser), after the override. */
enum { UI_HOST_CABLE = 0, UI_HOST_ADAPTER = 1, UI_HOST_ASK = 2 };
int ui_host_mode(void);
/* A GBA game's link mode after its override: LINK_CABLE / LINK_ADAPTER /
 * LINK_BOTH (link_games.h).  Needs ui_linkmode_load first. */
int ui_link_mode_for(const char *gba_code, const char *key);
/* The mode the last UI_ACT_NET_HOST chose: 1 = Link Cable, 0 = adapter. */
int ui_host_cable(void);

/* The session as the menus draw it.  Set by main_psp.c before ui_frame. */
enum { UI_LINK_OFF = 0, UI_LINK_HOSTING, UI_LINK_LINKED };
typedef struct
{
   int state;            /* UI_LINK_* */
   int cable;            /* 1 Link Cable (GBA mirror or GB), 0 adapter */
   int player;           /* cable: 1 / 2, 0 = not yet known */
   const char *room;     /* "Room 7" */
   const char *game;     /* cable host: the game the joiner must open */
#ifdef VITA
   int conn;             /* UI_CONN_* the session runs on */
   const char *peer;     /* the other player's name; NULL/"" = not known */
   int rtt_ms;           /* round trip to the other console, -1 unknown */
   int signal_pct;       /* Wi-Fi / ad hoc signal 0..100, -1 unknown */
   const char *error;    /* UI_LINK_OFF: why the last Host / local start
                          * failed, one sentence; NULL/"" = nothing */
#endif
   int fast;             /* Fast link in the running GBA cable session: 1 /
                            0, -1 = not one (fe_host_fast_link) */
   int fast_src;         /* FE_GBLINK_FL_* : own / the host's / old peer */
} ui_link_view;
void ui_set_link(const ui_link_view *v);
/* Turbo buttons are off while a link or wireless session is up (main_psp.c
 * turbo_frame); the Controls pages say so in the cheats' words. */
void ui_set_turbo_locked(int on);

#ifdef VITA
/* ---- the Vita's Play together pages (vita/ui_host_hooks.h) --------------
 *
 * What UI_ACT_NET_JOIN joins.  Valid from the action until the next one. */
typedef struct ui_join_info
{
   int  conn;            /* UI_CONN_* (g_pcfg.net_conn when it was picked) */
   int  listed;          /* 1: a room picked from the list (the fields
                          * below are the ui_room it was); 0: ad hoc, the
                          * system's dialog finds the host                  */
   char addr[24];        /* listed: "ip:port" to join                      */
   unsigned id;          /* listed: ui_room.id                             */
   int  kind;            /* listed: ui_room.kind (LAN_KIND_*); ad hoc: the
                          * kind this game would host, as a first guess     */
   int  number;          /* listed: the room number                        */
   char code[5];
   char title[24];
   char nick[16];
} ui_join_info;
const ui_join_info *ui_join_target(void);

/* Every way a link can fail, in the player's words: two lines for the
 * LINKING screen (ui_linking_view.l1 / .l2 with .fail = 1) or one sentence
 * for ui_link_view.error (l1 alone reads complete).  Never NULL. */
enum
{
   UI_FAIL_NO_WIFI = 0,     /* this console is not on a network            */
   UI_FAIL_NO_ANSWER,       /* nobody answered at that room / address      */
   UI_FAIL_ROOM_FULL,
   UI_FAIL_VERSION,         /* the other console runs a different build    */
   UI_FAIL_GAME,            /* the two games are not the same game         */
   UI_FAIL_HOST_LEFT,       /* the room closed while joining               */
   UI_FAIL_LINK_LOST,       /* a running session lost the other console    */
   UI_FAIL_DESYNC,          /* the two machines fell out of step           */
   UI_FAIL_ADDRESS,         /* the typed address is not one                */
   UI_FAIL_ADHOC_CANCELLED, /* the system's connection screen was closed   */
   UI_FAIL_RADIO_BUSY,      /* Wi-Fi and ad hoc cannot both be in use      */
   UI_FAIL_PORT,            /* the host's port could not be opened         */
   UI_FAIL_MEMORY,
   UI_FAIL_LOCAL,           /* the second machine would not start          */
   UI_FAIL_COUNT
};
void ui_link_fail_text(int why, const char **l1, const char **l2);

/* The LINKING screen and the finger: ui_linking_draw() registers its
 * footer's hints ("O cancel", "O back") as touch targets.  Whoever draws
 * the screen calls this once per frame it is up and treats the result as
 * button edges beside the pad's: PSP_CTRL_CIRCLE when the hint was tapped,
 * 0 otherwise. */
unsigned ui_linking_tapped(void);

/* 1 while the system keyboard is up for one of the UI's text fields (the
 * main loop keeps presenting frames; shortcuts stay quiet, as for
 * ui_capturing). */
int ui_typing(void);
#endif

/* THE LINKING SCREEN: one screen for cable, adapter and GB, in the
 * Overlay's language over the game as it stood (the menu backdrop; set it
 * with ui_set_backdrop first).  main_psp.c draws it each frame a session
 * sets up, and once during a blocking radio bring-up.  Strings may be NULL.
 * `fail`: the two lines are a failure (l1 bright, l2 dim, footer O back). */
typedef struct
{
   const char *game;     /* the line under the wordmark */
   const char *room;     /* the header cluster */
   const char *l1, *l2, *amount, *l3;
   int frac;             /* the bar, per mille; -1 = none */
   int fail;
   int busy;             /* the ring turns */
   const char *footer;   /* NULL = O cancel (busy) / O back (fail) */
} ui_linking_view;
void ui_linking_draw(const ui_linking_view *v);
/* Harness ui_scan_fake: the scan list answers with these group names
 * (comma-separated; "-" = no rooms) instead of the radio. */
void ui_scan_fake_set(const char *csv);

/* ---- Mystery Gift status, for the screen that shows it -------------------
 * Implemented in main_psp.c, where the rest of the wireless state lives (the
 * same arrangement as osd_session_chip_refresh below).  The frontend hands
 * the UI finished strings rather than the session struct, so ui_psp.c needs
 * no knowledge of the wire protocol. */
int         mgift_ui_active(void);    /* 1 while the listener is up */
/* Which stored network profile the player picked, 1-based (0 = first that
 * exists).  Read by the frontend when it associates. */
int         ui_mgift_conf(void);
const char *mgift_ui_line1(void);     /* what is happening */
const char *mgift_ui_line2(void);     /* the gift / progress, or "" */

/* "Quit to game list" without a black screen (docs/INPROC-RETURN.md).
 * ui_return_hold: the menu goes, the frame it sat on stays (the backdrop
 * set with ui_set_backdrop), and a chip says "Returning to game list..."
 * with the loading screen's ring -- drawn and presented now.
 * ui_return_tick: the same, at most every 50 ms, so the ring moves while
 * the game unloads (the ROM scan ticks it too).  The next ui_browser()
 * crossfades from it into the browser over UI_RETURN_FADE_FRAMES.
 * ui_return_fade_in: instead, the next ui_browser() fades in from black
 * (the relaunch fallback, after the firmware's reload). */
#define UI_RETURN_FADE_FRAMES 16
void ui_return_hold(void);
void ui_return_tick(void);
void ui_return_fade_in(void);
/* Harness `ret_dumps = 1`: log/ge_ret<N>_hold.bmp and ge_ret<N>_fade<k>.bmp. */
void ui_return_dumps(int on);

/* Blocking pre-game ROM browser (generic build with no baked ROM and no
 * harness). Fills out with the full ROM path. Returns 0 on pick, -1 on
 * exit request / empty dir, and 1 when START->SETTINGS changed something
 * only read at boot (Media Engine mode) and the app must relaunch. */
int ui_browser(const char *rom_dir, char *out, size_t out_sz,
               int *out_state_slot, fe_console_t *console_out);

/* Pure extension predicate shared by the browser and its host test.  The
 * console selection is explicit: .gbc files are not folded into GB mode. */
static inline int ui_rom_matches_console(const char *name, fe_console_t console)
{
   const char *ext;
   size_t len, i;
   static const char *const suffix[] = { ".gba", ".gb", ".gbc" };
   if (!name || console < FE_CONSOLE_GBA || console > FE_CONSOLE_GBC)
      return 0;
   len = strlen(name);
   ext = suffix[console];
   if (len < strlen(ext))
      return 0;
   name += len - strlen(ext);
   for (i = 0; ext[i]; i++)
   {
      unsigned char a = (unsigned char)name[i];
      unsigned char b = (unsigned char)ext[i];
      if (a >= 'A' && a <= 'Z') a = (unsigned char)(a + ('a' - 'A'));
      if (b >= 'A' && b <= 'Z') b = (unsigned char)(b + ('a' - 'A'));
      if (a != b) return 0;
   }
   return 1;
}

static inline size_t ui_rom_stem_length(const char *name)
{
   const char *dot, *slash;
   if (!name) return 0;
   dot = strrchr(name, '.');
   slash = strrchr(name, '/');
   return dot && (!slash || dot > slash) ? (size_t)(dot - name) : strlen(name);
}

/* Wake-from-sleep overlay: the frozen frame with a plate over it.
 * `frame` may be NULL (nothing was captured), in which case the plate sits
 * on the theme background.  Returns 0 to resume, 1 for the game list. */
int ui_wake_menu(const uint16_t *frame, int frame_w, int frame_h,
                 const char *game);
/* Harness `wake_shot = N`: the next wake overlay dumps log/ge_wake.bmp on
 * its third frame and continues on its own. */
void ui_wake_shot_arm(void);
void ui_wake_inject(int choice);   /* harness: sleep_sim_choice */
void ui_wake_inject_frame(int frame);   /* harness: sleep_sim_choice_frame */
void ui_batt_inject(int v);        /* harness: ui_batt (the header's battery) */
void ui_ctl_warn_inject(int on);   /* harness: ui_ctl_warn (the stick note) */
void ui_turbo_lock_inject(int on); /* harness: ui_turbo_lock (link wording) */

/* Harness self-drive (.gpsp-harness.ini ui_demo=1): scripted walk through
 * menu -> settings (cycle scale) -> wireless -> resume, with EVT markers
 * (ui_open/ui_screen/ui_demo_done) and a GE dump of the menu screen. */
void ui_demo_start(void);
/* .gpsp-harness.ini ui_controls_demo=1: the same self-drive, through
 * Settings > Controls -- unbind, capture, a steal, a chord, reset -- with GE
 * dumps log/ge_ctl_*.bmp (docs/CONTROL-REMAP.md). */
void ui_controls_demo_start(void);
/* ui_controls_demo=2 (with browser=1): the browser opens START settings,
 * walks to Controls (log/ge_ctlb_*.bmp), backs out and boots the game. */
void ui_controls_browser_demo(void);
/* ui_controls_demo=3: Settings > Controls > Menu, flipped once (ge_cth_*);
 * =4: save slot 2 on the Save states page, then delete it -- the question,
 * a "keep", the delete (ge_del_*).  docs/CONTROL-REMAP.md sections 10-11. */
void ui_home_demo_start(int which);
int  ui_demo_running(void);
void ui_demo_shots(void);          /* arm the gallery dump + auto-pick     */
/* ui_browser_demo=1 (with browser=1): the browser walks its 3.0 states --
 * star, favourites, empty favourites, console flare, empty console -- and
 * GE-dumps each to log/ge_gallery_*.bmp, then exits. */
void ui_browser_demo_shots(void);
/* ui_browser_script = <file under the app dir>: a text script of presses,
 * dumps and per-frame dump ranges for the README shoot (see ui_psp.c). */
void ui_browser_script_load(const char *rel);
/* Harness `share_burst = N`: file datagrams the sharing PSP sends a frame
 * (docs/SAVE-SHARE.md; default 4). */
void ui_share_burst(unsigned n);
void ui_set_theme_black(int b);    /* runtime page theme (-1 = leave)      */

#ifdef __cplusplus
}
#endif

#endif /* UI_PSP_H */
