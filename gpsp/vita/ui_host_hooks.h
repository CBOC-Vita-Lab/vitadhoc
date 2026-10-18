/* ui_host_hooks.h -- what the UI needs from the program around it.
 *
 * On the PSP, psp/ui_psp.c calls a handful of things that live in
 * main_psp.c (the PSP main loop, which is NOT part of the Vita build).  On
 * the Vita the main loop (vita/main_vita.c) and the link layer
 * (vita/link_vita.c, vita/local_vita.c) provide them.  This header is the
 * COMPLETE list, with exact prototypes and semantics; ui_psp.c includes it
 * under -DVITA.  Every hook in sections 3..6 has a weak "not available"
 * default in vita/ui_hooks_default.c, so the application links before the
 * file that really implements it exists; the host twin's simulations are in
 * vita/host/ui_host_test.c.
 *
 *   1  PROVIDED FOR YOU            nothing to implement
 *   2  THE MAIN LOOP               g_running, the session chip, Mystery Gift
 *   3  THE NETWORK'S STATE         ui_host_net_status
 *   4  ROOMS                       ui_host_rooms_begin / _poll / _end
 *   5  THE LOCAL LINK              ui_host_local_state
 *   6  CHEATS                      vita/cheats_vita.h (not hooks: a contract)
 *   7  WHAT THE UI RETURNS         the actions and what each one expects
 *   8  CONFIG.INI KEYS             the keys the UI owns and the link layer reads
 *   9  TEXTURES AND FRAMES         formats of what the main loop hands over
 *
 * THE RULE FOR EVERYTHING HERE: NOTHING BLOCKS.  The menus run at the
 * display's rate with the game frozen behind them; a hook that waits for the
 * network or the card freezes the picture.  Each hook says how often it is
 * called; anything slower than a few hundred microseconds must be answered
 * from a copy kept up to date somewhere else (link_frame, an I/O thread).
 *
 * ===========================================================================
 * 1  PROVIDED FOR YOU (do not define these in a main loop)
 * ===========================================================================
 *   char g_dir_base[128]      the data root, vita/pspcompat.c
 *                             (pspcompat_set_root; "ux0:data/GBAdhoc")
 *   pwr_hold / pwr_release    no-ops on the Vita (macros in ui_psp.c)
 *   everything in psp/video_psp.h and vita/video_vita.h   vita/video_vita.c
 *   pspcompat_touch()         the front panel (vita/pspcompat.h).  The UI
 *                             reads it by itself inside ui_frame / ui_browser
 *                             / ui_wake_menu: a main loop passes no touch
 *                             data in.  It may read it too, for taps on the
 *                             GAME picture while the menu is closed.
 *   pspcompat_ime_*()         the system keyboard.  The UI opens it, polls it
 *                             and switches vid_set_common_dialog itself; the
 *                             main loop only has to keep calling ui_frame and
 *                             vid_swap while ui_active(), as it always does.
 *   pspcompat_dialogs_init()  sceAppUtilInit + sceCommonDialogSetConfigParam,
 *                             idempotent; call it at boot if the ad hoc
 *                             NetCheck dialog may come before any keyboard.
 *
 * LINK DEPENDENCIES THAT ARE NOT HOOKS (portable code already in the tree):
 *   frontend-common/fe_util.c fe_evt.c fe_favs.c fe_linkmode.c
 *   frontend-common/fe_host.c     for fe_host_gb_palette_count/_name/_set
 *                                 (Settings > GB palette)
 *   psp/stb_impl.c                stb_image, for box art
 *   vita/ui_hooks_default.c       the weak defaults (vita/ui.mk lists it)
 *   SceTouch_stub, SceAppUtil_stub  (vita/ui.mk VITA_UI_LIBS)
 */
#ifndef UI_HOST_HOOKS_H
#define UI_HOST_HOOKS_H

#ifdef __cplusplus
extern "C" {
#endif

/* ===========================================================================
 * 2  THE MAIN LOOP
 * ======================================================================== */

/* 1 while the application runs.  Every modal screen (the browser, the wake
 * overlay, browser Settings, the notices) loops on it and returns as soon
 * as it is 0, so "quit" from anywhere is: clear it. */
extern volatile int g_running;

/* Settings > Session overlay changed g_pcfg.osd_wireless: re-apply the
 * session chip (osd_chip_session with the current text, or NULL to hide). */
void osd_session_chip_refresh(void);

/* Settings > Colours or Motion blend changed g_pcfg.color / g_pcfg.blend of
 * the console in context (pcfg_display_console()): show it.  The renderer
 * takes a GBA's colours and the blend (vid_set_look); a Game Boy Color's
 * colours are its core's.  Also what a main loop calls when a game boots.
 * Weak default: the renderer's part. */
void ui_host_look_changed(void);

/* ---- Mystery Gift (declared in psp/ui_psp.h; listed here for completeness)
 *   int         mgift_ui_active(void);   1 while the listener is up
 *   const char *mgift_ui_line1(void);    what is happening
 *   const char *mgift_ui_line2(void);    the gift / progress, or ""
 * A build without a Mystery Gift station returns 0 / "" / "" and answers 0
 * here, which removes the row from the Play together page (the weak default
 * does exactly that): */
int ui_host_mgift_available(void);

/* ---- ROM folders beside roms/ ---------------------------------------------
 * The game list shows roms/ first and then the games of each of these
 * folders (the same depth, the same file types).  An entry from one is
 * named by its whole path ("ux0:ROMS/GBA/Game.gba"; psp/rom_paths.h:
 * psp_rom_path_join hands such a name back as it is), and its save and its
 * states are written beside it.  main_vita.c builds the list once at start
 * from vita.ini `extra_roms` or the well-known folders, keeping the ones
 * that exist; never the roms/ folder itself, never one twice.  The folders
 * are device paths without a trailing slash; the strings are valid for the
 * life of the program.  The weak defaults have none. */
#define UI_EXTRA_ROOTS_MAX 8
int         ui_host_extra_roots(void);
const char *ui_host_extra_root(int i);

/* ===========================================================================
 * 3  THE NETWORK'S STATE          (implemented by link_vita.c)
 * ===========================================================================
 * How the consoles reach each other is CONFIG.INI `net_conn`
 * (g_pcfg.net_conn), one of these -- numerically app_vita.h's LINK_CONN_*
 * (checked at the bottom of this file when both headers are included): */
enum
{
   UI_CONN_WIFI = 0,        /* same Wi-Fi network or phone hotspot: rooms
                             * are found by themselves                    */
   UI_CONN_INTERNET,        /* the joiner types the host's address         */
   UI_CONN_ADHOC,           /* no router: the system's ad hoc connection   */
   UI_CONN_COUNT
};

enum { UI_WIFI_OFF = 0,     /* not associated: no access point, flight
                             * mode, or the radio is in ad hoc use         */
       UI_WIFI_CONNECTING,  /* associating / waiting for an address        */
       UI_WIFI_CONNECTED }; /* associated and holding an address           */

/* What the router did about the host's port (Connection: Internet). */
enum { UI_PUBLIC_UNKNOWN = 0,  /* not hosting, or nothing tried            */
       UI_PUBLIC_ASKING,       /* asking the router (UPnP) right now       */
       UI_PUBLIC_OPEN,         /* public_addr is reachable from outside    */
       UI_PUBLIC_CLOSED };     /* the router did not open it: the player
                                * must forward the port himself            */

/* Who holds the one radio (net_vita.h "one radio, one use"). */
enum { UI_RADIO_FREE = 0, UI_RADIO_INFRA, UI_RADIO_ADHOC };

typedef struct ui_net_status
{
   int  wifi;             /* UI_WIFI_*                                     */
   char ssid[33];         /* the network's name, "" unknown                */
   char ip[16];           /* this console, "192.168.1.23"; "" = none       */
   int  signal_pct;       /* access-point signal 0..100, -1 unknown        */
   /* While this console HOSTS on Wi-Fi or Internet: "192.168.1.23:19019",
    * the address a joiner on the same network types (the bound port is only
    * known once the room is up).  "" when not hosting. */
   char host_addr[24];
   /* While it hosts with Connection: Internet: what a joiner OUTSIDE the
    * network types, "203.0.113.9:19019", once known; "" until then. */
   char public_addr[24];
   int  public_state;     /* UI_PUBLIC_*                                   */
   int  radio;            /* UI_RADIO_*                                    */
} ui_net_status;

/* Fill *out (the UI zeroes it first; anything left untouched reads as
 * "unknown").  Called when a Play together page opens and about TWICE A
 * SECOND while one is on screen -- never per frame, so it may make the few
 * sceNetCtl calls directly (net_vita_wifi_status), but nothing that waits.
 * The weak default reports UI_WIFI_OFF. */
void ui_host_net_status(ui_net_status *out);

/* ===========================================================================
 * 4  ROOMS: the Join page's list  (implemented by link_vita.c)
 * ===========================================================================
 * The PSP's blocking 10-second scan is gone.  The Join page opens a scan,
 * asks for the list EVERY FRAME while it is on screen and closes the scan
 * when it leaves; rooms appear, change and disappear by themselves.
 *
 *   ui_host_rooms_begin(NULL)        Connection: Wi-Fi -- discover the rooms
 *                                    on this network (lan_scan_open with the
 *                                    subnet broadcast)
 *   ui_host_rooms_begin("a.b.c.d[:port]")
 *                                    Connection: Internet -- look at ONE
 *                                    address the player typed (lan_scan with
 *                                    target_ip_be/target_port); the room
 *                                    behind it, if any, is listed
 *   Ad hoc has no list (the system's dialog does the finding): the UI never
 *   opens a scan with net_conn == UI_CONN_ADHOC.
 *
 * begin: 0 = the scan is open; < 0 = UI_ROOMS_E_* and nothing is open (the
 *        page shows the reason and calls begin again about once a second,
 *        so "connect to Wi-Fi, come back" needs no button).  begin while a
 *        scan is open replaces it.
 * poll:  the rooms known NOW, at most `max` (the UI passes UI_ROOMS_MAX),
 *        newest information first is not required: the UI sorts.  >= 0 = how
 *        many, < 0 = UI_ROOMS_E_* (the scan died: the UI calls end, then
 *        begin again later).  Must not block: lan_scan_poll +
 *        lan_scan_rooms and a copy.
 * end:   close the scan.  Harmless when none is open.  The UI calls it
 *        before it returns UI_ACT_NET_JOIN, so the room it hands over (see
 *        ui_join_target) must not point into the scan.
 */
#define UI_ROOMS_MAX 16
enum
{
   UI_ROOMS_E_NO_WIFI  = -1,   /* not connected to a network                */
   UI_ROOMS_E_BUSY     = -2,   /* the radio is in ad hoc use, or a session
                                * is running                                */
   UI_ROOMS_E_FAILED   = -3,   /* the socket could not be opened            */
   UI_ROOMS_E_ADDRESS  = -4    /* begin(addr): not an address               */
};

typedef struct ui_room
{
   unsigned id;           /* the same room keeps the same id from poll to
                           * poll (lan_room.room_id): the cursor stays on
                           * it while the list changes around it           */
   char title[24];        /* the game, as the host announces it.  The
                           * list shows link_games.h's name for a GBA code
                           * it knows and this text otherwise, so a host
                           * does best to announce the ROM's short display
                           * name, not the header's twelve capitals        */
   char nick[16];         /* the host's player name                        */
   char code[5];          /* GBA header code / first four of a GB title    */
   int  number;           /* the host's room number 0..99 (Settings > Room)*/
   int  kind;             /* 0 GBA Wireless Adapter, 1 GBA Link Cable,
                           * 2 Game Boy / Color Link Cable: LAN_KIND_* =
                           * psp/room_label.h ROOM_*                        */
   int  players;          /* in the room now, the host included            */
   int  max_players;      /* 2 for every cable room                        */
   int  open;             /* 1: accepting a join right now                 */
   int  compatible;       /* 0: the other console runs a different
                           * GBAdhoc version (lan_room.compatible)         */
   int  rtt_ms;           /* probe round trip, -1 unknown                  */
   char addr[24];         /* "192.168.1.23:19019": where to join it, and
                           * what the page shows for an Internet room      */
} ui_room;

int  ui_host_rooms_begin(const char *addr);
int  ui_host_rooms_poll(ui_room *out, int max);
void ui_host_rooms_end(void);

/* ===========================================================================
 * 5  THE LOCAL LINK: two players on this Vita  (implemented by local_vita.c)
 * ===========================================================================
 * Called every frame while the Play together page or its "Two players on
 * this Vita" page is on screen; must be a few loads.  The UI zeroes *out
 * first; the weak default says "not available" with a reason. */
typedef struct ui_local_state
{
   int available;         /* 1: UI_ACT_NET_LOCAL would work for the running
                           * game right now (a GBA game booted with the
                           * second machine loaded, no network session)    */
   const char *why_not;   /* available == 0: one sentence for the page,
                           * e.g. "Game Boy games link over Wi-Fi only."
                           * Static storage; NULL reads as a generic line  */
   int active;            /* both machines are running (link_local_active) */
   int controlled;        /* active: the VIEW -- which machine the one pad
                           * drives, is heard and (large + small) is the
                           * large picture: 0 the player's own game, 1 the
                           * partner's                                     */
   int have_pad2;         /* a second controller drives the partner (PS TV,
                           * or a pad paired through a plugin): the view
                           * then only moves the sound and the large picture,
                           * and the row says so                           */
   int partner_save;      /* the partner's own save file exists
                           * (<rom>.p2.sav; with another game chosen, that
                           * game's own save): 1 yes, 0 not yet            */
   const char *partner_game; /* the partner's cartridge when it is not the
                           * player's: a name for the page (static storage);
                           * NULL = the same game                          */
   int saves_written;     /* active: times the partner's save file was
                           * written since the start                       */
} ui_local_state;
void ui_host_local_state(ui_local_state *out);

/* THE PARTNER'S GAME.  The partner's machine may hold another cartridge of
 * the same console (the other version of a pair: Ruby and Sapphire, Red and
 * Blue); it then starts from THAT game's own save, never as a copy.  The
 * choice is remembered per game by the implementation.
 *   ui_host_local_roms()        the picker opens: scan (a directory read:
 *                               the one call here that may take a moment)
 *                               and return how many candidates there are
 *   ui_host_local_rom_name(i)   a name for row i (static storage, valid
 *                               until the next call)
 *   ui_host_local_rom_pick(i)   choose row i; i < 0 = this game again.
 *                               0 = done (ui_host_local_state reports it),
 *                               < 0 = refused (the pair is running, or the
 *                               file is gone)
 * The weak defaults list nothing. */
int         ui_host_local_roms(void);
const char *ui_host_local_rom_name(int i);
int         ui_host_local_rom_pick(int i);

/* ===========================================================================
 * 6  CHEATS
 * ===========================================================================
 * Not hooks: the in-game Cheats page calls vita/cheats_vita.h directly --
 *   cheats_count / cheats_get / cheats_enabled_count / cheats_set_enabled /
 *   cheats_validate / cheats_add / cheats_remove / cheats_rename /
 *   cheats_suspended / cheats_error_text / cheats_kind_name /
 *   cheats_format_hint / cheats_db_game / cheats_note / cheats_db_match
 * and never the loaders.  The main loop keeps its side of that contract
 * (cheats_db_open at start, cheats_load at boot, cheats_apply after a state
 * load, cheats_flush when the menu closes, cheats_button from the right
 * stick), and link_vita.c calls cheats_suspend around a NETWORK session
 * (app_vita.h says why it and not the main loop; not the local link).  The
 * page re-reads cheats_count()/cheats_get() every frame, so a list that
 * changes under it is drawn as it is.
 * vita/ui_hooks_default.c also carries weak "no cheats" versions of those
 * fifteen: they are what the UI's own bench (vita/host) links; in the app
 * cheats_vita.c's definitions replace every one of them.
 *
 * ===========================================================================
 * 7  WHAT THE UI RETURNS: ui_frame's actions on the Vita (psp/ui_psp.h)
 * ===========================================================================
 * Call ui_set_link() (below) and then ui_frame() once per displayed frame
 * while ui_active().  What each action expects of the main loop:
 *
 *   UI_ACT_RESUME / SAVESTATE / LOADSTATE / EXIT / GAMELIST / RELAUNCH
 *        as on the PSP (VITA-INTEGRATION.md).  UI_ACT_RELAUNCH is never
 *        returned on the Vita (nothing in Settings needs a restart).
 *
 *   UI_ACT_NET_HOST        link_action(1).  The room: ui_group() (the PSP
 *        group id of Settings > Room, "GPSP07"), ui_host_cable() (1 = Link
 *        Cable, 0 = Wireless Adapter), g_pcfg.net_conn, g_pcfg.nick.
 *        THE MENU STAYS OPEN (unlike the PSP): it moves to the waiting page
 *        and draws it from ui_set_link + ui_host_net_status -- "Starting the
 *        room" until the view says UI_LINK_HOSTING (or .error), then the
 *        room, "Waiting for player 2" and the address to read out.  So:
 *          - do the bring-up without blocking, and report through the view;
 *          - do NOT put the LINKING screen up for "hosting, nobody there
 *            yet": the player is reading the waiting page, or has pressed
 *            "Keep playing" and is in the game;
 *          - when the other console arrives and the session's setup needs
 *            the screen, call ui_close() and draw LINKING as usual;
 *          - ad hoc: the NetCheck dialog draws itself over whatever frame is
 *            presented (vid_set_common_dialog); the menu underneath is fine.
 *
 *   UI_ACT_NET_JOIN        link_action(0); what to join is *ui_join_target()
 *        (a ui_join_info, psp/ui_psp.h): a listed room's address, or
 *        "ad hoc: the system's dialog finds the host".  The
 *        menu has closed (as on the PSP) and the scan is closed: draw the
 *        LINKING screen until the session runs or fails.  For every failure
 *        use ui_link_fail_text(UI_FAIL_*, &l1, &l2) so the wording stays in
 *        one place, with ui_linking_view.fail = 1.  While the LINKING
 *        screen is up, OR ui_linking_tapped() into the pad's edges: its
 *        footer ("O cancel" / "O back") is a touch target too.
 *
 *   UI_ACT_NET_DISCONNECT  link_disconnect(): stop hosting, or end the
 *        session by agreement.  The menu has returned to its top page.
 *        It can also arrive while a Host is still being brought up (the
 *        waiting page's "Cancel"): then it means "never mind", and the
 *        view goes back to UI_LINK_OFF with no error.
 *
 *   UI_ACT_NET_LOCAL       link_local_start(): both machines on this Vita.
 *        How the partner starts: g_pcfg.local_partner (0 = a copy of the
 *        player's save, 1 = its own save file <rom>.p2.sav, created if
 *        missing).  The menu stays open on the local page; success shows as
 *        ui_host_local_state().active, failure as ui_link_view.error.
 *   UI_ACT_NET_LOCAL_STOP  link_local_stop().  Menu stays open.
 *   UI_ACT_NET_LOCAL_SWAP  the pad drives the other machine
 *        (link_local_next_view()).  Menu stays open; the page shows
 *        ui_host_local_state().controlled.
 *        The LAYOUT of the two pictures is g_pcfg.local_layout
 *        (VID_PAIR_SIDE / VID_PAIR_INSET): read it every frame in
 *        link_local_present(), the page changes it live.
 *
 *   UI_ACT_NET_MGIFT / _MGIFT_STOP   as on the PSP; only reachable when
 *        ui_host_mgift_available().
 *
 * Also for the main loop (psp/ui_psp.h, psp/osd_psp.h):
 *   ui_typing()        1 while the system keyboard is up: keep presenting
 *                      frames, fire no shortcut (as for ui_capturing())
 *   osd_toast_place()  the UI moves toasts off its header while a menu or
 *                      the game list is up and back when it closes; a main
 *                      loop never needs to call it
 *
 * ui_frame's arguments are the PSP's: pad (pspcompat_pad()),
 * session_active = link_locks_states() (a NETWORK session is up: save
 * states are refused and the page says so; the local link does not lock
 * them), session_info = a status sentence or NULL (shown only when the view
 * gives the page nothing better).
 *
 * ui_set_link(const ui_link_view *v) -- psp/ui_psp.h; set it before every
 * ui_frame (link_ui_view()).  The PSP's fields, and on the Vita also:
 *   v->conn        UI_CONN_* the session runs on
 *   v->peer        the other player's name, or NULL/"" while unknown
 *   v->rtt_ms      round trip to the other console (netdrv srtt), -1 unknown
 *   v->signal_pct  Wi-Fi / ad hoc signal 0..100, -1 unknown
 *   v->error       state == UI_LINK_OFF only: why the last Host / local
 *                  start did not work, one sentence, kept until the next
 *                  action; NULL or "" = nothing to say.  ui_link_fail_text()
 *                  has the sentences.
 * Strings are copied.  ui_set_link(NULL) = no session.
 *
 * ===========================================================================
 * 8  CONFIG.INI KEYS the UI owns (psp/config_psp.h, g_pcfg; -DVITA only)
 * ===========================================================================
 *   net_conn       g_pcfg.net_conn        UI_CONN_*            default 0
 *   link_delay     g_pcfg.link_delay      0 = Auto, 1..8 = frames of input
 *                                         delay for the mirror / GB link
 *                                         (fe_host_gblink_start's `delay`);
 *                                         Auto = the link layer picks from
 *                                         the measured round trip.  Both
 *                                         consoles must agree: the HOST's
 *                                         value is the session's.
 *   nick           g_pcfg.nick            the player's name shown to others
 *                                         (lan_room_info.nick); first run:
 *                                         the console's user name
 *   group          g_pcfg.group           "GPSPnn", Settings > Room
 *   net_addr1..4   g_pcfg.net_addr[0..3]  the last host addresses typed,
 *                                         newest first (the UI maintains it)
 *   local_partner  g_pcfg.local_partner   0 copy of my save, 1 own save file
 *   local_layout   g_pcfg.local_layout    VID_PAIR_SIDE / VID_PAIR_INSET
 *   touch          g_pcfg.touch           1 (default); 0 = the menus ignore
 *                                         the front panel (read, written
 *                                         only if already in the file)
 *
 * ===========================================================================
 * 9  TEXTURES AND FRAMES the main loop hands the UI
 * ===========================================================================
 * (Not functions to implement -- the formats the existing ui_psp.h entry
 * points expect on the Vita.)
 *
 *   ui_set_backdrop(frame, w, h)        frame: w x h PSP-ORDER 565 (R in bits
 *   ui_wake_menu(frame, w, h, game)     0-4) at the top-left of a buffer 256
 *                                       texels wide.  Make it from the core's
 *                                       libretro-RGB565 frame with
 *                                       vid_frame_to_psp565(dst, 256, ...).
 *   ui_set_thumb_buffer(buf)            5 x (64 x 64) texels, PSP-order 565,
 *                                       filled by the UI from the .thumb
 *                                       files; the main loop only allocates.
 *   <state>.thumb / <state>.shot        psp/state_slots.h, PSP-order 565 --
 *                                       written by the main loop from the
 *                                       same converted frame, so a state
 *                                       saved on a PSP previews correctly
 *                                       here and the reverse.
 */

#ifdef GBADHOC_APP_VITA_H
/* A file that includes app_vita.h first gets the two enums compared. */
typedef char ui_conn_matches_link_conn[
   (UI_CONN_WIFI == LINK_CONN_WIFI && UI_CONN_INTERNET == LINK_CONN_INTERNET &&
    UI_CONN_ADHOC == LINK_CONN_ADHOC && UI_CONN_COUNT == LINK_CONN_COUNT)
   ? 1 : -1];
#endif

#ifdef __cplusplus
}
#endif

#endif /* UI_HOST_HOOKS_H */
