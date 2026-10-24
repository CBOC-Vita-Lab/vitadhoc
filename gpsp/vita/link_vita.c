/* link_vita.c -- GBAdhoc on the PS Vita: two consoles linked over a network.
 *
 * Part A of app_vita.h and sections 3-4 of ui_host_hooks.h.  psp/main_psp.c
 * is the reference for what a session does (net_bringup, ui_net_action,
 * gblink_begin, link_setup_watch, link_end, gblink_service, link_draw_facts,
 * plat_gblink_wait, exit_assist_frame; docs/GBA-LINK-MIRROR.md, GB-LINK.md,
 * GBA-LINK-UI.md); this file is that behaviour on the Vita's three ways of
 * connecting, with ONE difference that shapes everything in it:
 *
 *   NOTHING HERE BLOCKS.  The PSP brings its radio up inside one call that
 *   takes up to fifteen seconds, with a still picture on screen.  Here every
 *   bring-up is a state machine advanced once per iteration of the game loop
 *   (link_frame), the menus and the game keep running under it, and every
 *   step can be cancelled and has a time limit and a sentence for when it
 *   fails (ui_link_fail_text).
 *
 * THREE CONNECTIONS (config.ini net_conn), each for all three kinds of link:
 *
 *   Wi-Fi      both consoles on one network or phone hotspot.  Hosting
 *              announces a room (transport_lan); the Join page's list is a
 *              scan that stays open while the page shows; joining is by the
 *              listed room's address.
 *   Internet   the same transport, by a typed address.  The host shows its
 *              address at once and asks the router for a public one (UPnP,
 *              non-blocking); the joiner's address is probed by the Join
 *              page (the same scan, aimed) and joined as a listed room.
 *   Ad hoc     no router: the system's NetCheck dialog makes the connection
 *              (it draws itself into our frames, vid_set_common_dialog),
 *              then PDP datagrams (transport_vadhoc).  No room list: the
 *              joiner's JOIN is a broadcast and the protocol string decides
 *              who answers.
 *
 * THREE KINDS (the game decides, docs/GBA-LINK-UI.md):
 *
 *   GBA Link Cable    the mirror: each console runs BOTH GBAs with the cable
 *                     between them, only buttons cross (fe_gblink over
 *                     fe_gbadual's machines).  The host keeps playing until
 *                     the other console arrives.
 *   GBA Wireless      one core per console; the game's own adapter protocol
 *   Adapter           rides netdrv (fe_np_start) and pairs the players.
 *   GB / GBC cable    the mirror with two Game Boys per console.
 *
 * WHO MAY LINK WITH WHOM.  The mirror is only a mirror if both consoles
 * compute the same machines cycle for cycle: the same build, the same CPU
 * backend (the x86 and ARM dynarecs do not agree, upstream doc 16.6; nor
 * does either with the interpreter) and the same BIOS.  All three are
 * hashed into a cable room's compatibility word and into its netdrv
 * protocol string, so a console that cannot mirror is greyed in the Join
 * list ("different version") and, with no list (ad hoc), is simply never
 * answered.  GB/GBC rooms hash the build only (TGB Dual is one interpreter
 * everywhere); adapter rooms likewise.
 *
 * THE INPUT DELAY (config.ini link_delay) is the HOST's: the session's
 * CONFIG message carries it and the joiner follows (fe_gblink.c).  Auto
 * measures the round trip to the console that just arrived -- a few echoes
 * answered by its transport's I/O thread (lan_transport_echo), because
 * netdrv's own estimate needs traffic the host has not sent yet -- and
 * takes one frame more than the one-way trip and its spread need; without a
 * measurement (ad hoc) it goes by the kind of connection.
 *
 * TEST SWITCHES (vita.ini, beside the main loop's own; all optional):
 *   link_port = N          the rooms' base port (default 19019)
 *   link_loss / link_dup / link_latency / link_jitter
 *                          the transport's fault shim: percent, percent, ms,
 *                          ms.  On a console too: it is how stall absorption
 *                          is exercised without a bad network.
 *   link_build = TEXT      added to the build's identity: a console that
 *                          pretends to be another release
 *   link_upnp = off | A.B.C.D:PORT   no router request / ask this address
 *   link_upnp_ms = 1500    how long that test address may take per answer
 *                          instead of the multicast group (the fake gateway)
 *   link_absorb = 0        no stall absorption
 *   link_copies = N        frames of buttons per unreliable datagram (4)
 *   link_nack = 0          no resend requests
 *   link_peek = ADDR:N     log N 32-bit words of the player's machine's
 *                          memory at hexadecimal ADDR (EVT link_peek) when a
 *                          session starts running, every ten seconds of it
 *                          and when it ends: a test cartridge's verdict
 *
 * RAN / ONLY COMPILED.  This file is plain C and runs whole on the Linux
 * twin of the app (hostcore/link_tests.py: two or three processes over the
 * loopback, x86 and ARM under qemu).  What it stands on there is real for
 * Wi-Fi and Internet (transport_lan on BSD sockets, the same file the
 * console builds) and a fake for ad hoc (netdrv/tests/vadhoc_fake.c).  The
 * Sony calls under it -- net_vita.c's stack and Wi-Fi status, the NetCheck
 * dialog, PDP, the renderer's dialog update -- have only been compiled.
 *
 * WHAT ONLY TWO CONSOLES CAN CONFIRM (marked HARDWARE: where assumed):
 *   - the NetCheck dialog draws over whatever frame we present (the menu's
 *     waiting page for a host, the LINKING screen for a joiner), ends OK /
 *     cancelled / error as transport_vadhoc.c expects, and closes a few
 *     frames after an abort.  While it is up this file does not read the
 *     pad at all: whether the pad still reaches the app then is unknown;
 *   - after an ad hoc session the console returns to its access point by
 *     itself, and how long that takes (the menus show the Wi-Fi's state and
 *     the Join page retries by itself once a second);
 *   - room discovery on a phone hotspot: it needs the subnet broadcast (or
 *     the host's beacon) to be forwarded.  A hotspot that isolates its
 *     clients forwards neither, nor unicast: nothing here can link through
 *     one.  One that only drops broadcasts leaves the list empty, and the
 *     waiting page's "Not listed? Connection: Internet, type <address>" is
 *     the way in;
 *   - MTU: the largest datagram sent is the bulk lane's 1037 bytes (netdrv
 *     frames are 576), far inside 1500 on Wi-Fi and inside PDP's 1444;
 *   - what the once-a-second Wi-Fi look costs the emulation thread
 *     (net_vita_wifi_status: five small system calls), and whether a console
 *     roaming between access points reads "disconnected" for longer than the
 *     two looks this file allows before it ends the link;
 *   - sleep: the session is ended BEFORE the console suspends, so what a
 *     socket does across a suspend is never relied on;
 *   - the real numbers behind the delay: EVT link_rtt (the echo), EVT
 *     gblink_win (stalls, lead_min) and EVT net_stats srtt_us of the first
 *     sessions decide whether DELAY_WIFI / DELAY_ADHOC are right.
 */
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

#include <arpa/inet.h>

#include "fe_host.h"
#include "fe_evt.h"
#include "fe_util.h"
#include "fe_gblink.h"
#include "netpacket_host.h"

#include "ui_psp.h"
#include "osd_psp.h"
#include "config_psp.h"
#include "ctl_map.h"
#include "room_label.h"
#include "video_vita.h"

#include "plat.h"
#include "app_vita.h"
#include "ui_host_hooks.h"
#include "cheats_vita.h"
#include "net_vita.h"
#include "transport_lan.h"
#include "transport_vadhoc.h"
#include "upnp_igd.h"

/* The room kinds are one vocabulary in three headers. */
typedef char link_kinds_agree[((int)LAN_KIND_GBA_WIRELESS == (int)ROOM_ADAPTER &&
                               (int)LAN_KIND_GBA_CABLE == (int)ROOM_CABLE_GBA &&
                               (int)LAN_KIND_GB_CABLE == (int)ROOM_CABLE_GB)
                              ? 1 : -1];

/* ---- numbers ------------------------------------------------------------------ */

/* The input delay when it is not measured, by connection. */
#define DELAY_WIFI        2
#define DELAY_INTERNET    4      /* also Auto's floor there is 3: see below */
#define DELAY_ADHOC       3      /* HARDWARE: nobody has measured the Vita's ad
                                  * hoc radio; the PSP's needs 4 at a third of
                                  * the CPU.  Read EVT gblink_win lead_min. */
#define DELAY_MAX         PCFG_LINK_DELAY_MAX
/* Auto: echoes sent (one a frame) and frames waited for their answers. */
#define MEASURE_ECHOES    6
#define MEASURE_FRAMES    24
#define FRAME_US          16683u

/* Time limits, in sixtieths of a second ON THE CLOCK (L.ticks), not in loop
 * iterations: a console that is busy, or a display that is not 60 Hz, must
 * not turn twelve seconds into twenty. */
#define T_JOIN_ANSWER     (12u * 60u)   /* a joiner: the host's first answer */
#define T_ADHOC_FLIP      (4u * 60u)    /* ad hoc joiner: try the other kind */
#define T_ADHOC_ANSWER    (20u * 60u)   /* ...and give up */
#define T_DIALOG          (150u * 60u)  /* the system's connection screen: its
                                         * own JOIN limit is 60 s, and a host
                                         * waits in it for the other console */
#define T_FLUSH           600u          /* the partner's last acknowledgement */
#define T_IGD_REMOVE      180u          /* the router's goodbye */
#define T_STALL_NOTE      90u           /* "waiting for the other Vita" */
/* The Wi-Fi (or the ad hoc cell) is looked at this often, and is gone after
 * this many looks in a row say so. */
#define WATCH_EVERY       60u
#define WATCH_STRIKES     2

/* fe_host_config.gblink_wait (stall absorption), the PSP's numbers. */
#define WAIT_PRESENT_US   1500u
#define WAIT_MAX_US       12000u

/* The link session's bulk lane: 1024 data + 13 header (psp/main_psp.c). */
#define BULK_BYTES        1037u
/* Scanlines slot 0 may run ahead while the GB cable is quiet (GB-LINK.md 8). */
#define GB_BATCH          154u

/* ---- state -------------------------------------------------------------------- */

enum { LK_NONE = 0, LK_ADAPTER, LK_CABLE, LK_GB };

enum
{
   PH_OFF = 0,
   PH_DIALOG,        /* ad hoc: the system's connection screen is up         */
   PH_JOINING,       /* adapter joiner: waiting for the host's answer        */
   PH_HOSTING,       /* the room is open, nobody in it; the game runs        */
   PH_MEASURE,       /* cable host: a console arrived, timing the way to it  */
   PH_LINKED,        /* adapter: in the room.  cable / GB: the session       */
   PH_FAILED         /* a join that did not link: its two lines, until O     */
};

/* Why a link did not form or did not last, beyond what the session knows. */
enum { LW_NONE = 0, LW_FULL, LW_NOANSWER, LW_CANCEL, LW_WIFI, LW_RADIO,
       LW_SLEEP, LW_OVERLOAD };

enum { TR_NONE = 0, TR_LAN, TR_ADHOC };

static struct
{
   int  phase, kind, conn, is_host;
   int  transport;                /* TR_*: what is open                      */
   int  adhoc_stack;              /* vadhoc_net_init done, radio claimed     */
   int  net_up;                   /* fe_np_start*() succeeded                */
   lan_transport *lan;
   unsigned ticks;                /* the clock, in sixtieths of a second     */
   unsigned t_phase;              /* ...when this phase began                */
   unsigned t_watch, t_peek, t_win;
   unsigned prev_pad;
   int  screen;                   /* the LINKING screen is up this iteration */
   int  screen_drew;              /* ...and was drawn last iteration         */
   int  held;                     /* fe_host_gblink_hold(1) is in force      */
   int  cheats_off;               /* cheats_suspend(1) is in force           */

   /* the room */
   char room[ROOM_LABEL_CAP];     /* "Room 7"                                */
   int  number;
   int  room_kind;                /* LAN_KIND_*                              */
   char peer[24];                 /* the other player's name                 */
   int  players, open;            /* as last announced                       */
   int  peers;                    /* adapter host: consoles in the room      */

   /* ad hoc */
   int  dlg_abort;                /* 1 cancelled by the player, 2 timed out  */
   int  flips;                    /* joiner: times it changed its guess      */
   int  can_flip;                 /* ...and whether there is another guess   */
   int  flip_now;                 /* this attempt ends in a flip             */

   /* the session (cable / GB) */
   int  why;                      /* LW_*                                    */
   int  clear;                    /* host: the console it turned away must
                                   * leave before the next one may start     */
   unsigned t_setup, t_ended;     /* when the setup began / the end was seen */
   int  ended;
   int  fail_hold, fail_code;     /* the failure screen, its UI_FAIL_*       */
   unsigned delay;                /* this session's, as the host chose it    */
   unsigned echoes;
   unsigned t_stall, last_frames;
   int  stalled;
   int  stall_told;
   int  end_asked;

   /* watching the radio */
   int  strikes, signal_pct;

   /* Internet: the router */
   upnp_igd *igd;
   int  igd_removing;
   unsigned igd_t;
   int  pub_state;                /* UI_PUBLIC_*                             */
   char pub_addr[24];
   char host_addr[24];

   /* what the menus and the chip read */
   char info[48];
   const char *error;             /* why the last Host did not work          */
   int  resume_note;              /* LW_SLEEP: say so once the game is back  */

   /* stall absorption */
   uint32_t work_ema;             /* a frame's work, estimated, us           */
   uint32_t frame_wait_us;        /* this frame's absorbed waiting           */
   uint64_t wait_prev;
} L;

/* What does not change with the session: the switches, who we are. */
static struct
{
   int      port;                 /* rooms' base port, 0 = LAN_PORT_DEFAULT  */
   int      loss, dup, latency, jitter;
   char     build[40];
   char     upnp[32];
   int      upnp_ms;
   int      absorb, copies, nack;
   unsigned peek_addr, peek_words;
   int      drc;
   int      up;                   /* net_vita_init succeeded                 */
   uint32_t compat[3];            /* by LAN_KIND_*                           */
   char     proto_cable[ND_PROTO_LEN], proto_gb[ND_PROTO_LEN];
   int      exit_assist;
} S;

/* The Join page's scan (ui_host_rooms_*). */
static struct { lan_scan *scan; int claimed; } R;

static void link_teardown(void);
static void chip_update(void);
static void win_note(int final);

/* ---- small things ---------------------------------------------------------------- */

static uint64_t link_now_us(void)
{
   return plat_time_us();
}

/* rfu.c reads the frontend's clock once per emulated frame (ADR-0045). */
uint32_t gpsp_rfu_now_us(void);
uint32_t gpsp_rfu_now_us(void)
{
   return (uint32_t)plat_time_us();
}

static int is_gba(void)
{
   return g_game.console == FE_CONSOLE_GBA;
}

static void phase_set(int ph)
{
   if (L.phase != ph)
      fe_evt("link_phase %d -> %d kind=%d conn=%d host=%d", L.phase, ph,
             L.kind, L.conn, L.is_host);
   L.phase = ph;
   L.t_phase = L.ticks;
}

static unsigned phase_age(void)
{
   return L.ticks - L.t_phase;
}

/* The single machine stops (as behind the menu) while a LINKING screen
 * covers it: a game nobody can see must not play on. */
static void hold_game(int on)
{
   if (L.held == on)
      return;
   L.held = on;
   fe_host_gblink_hold(on);
   if (on)
      app_audio_flush();
   else
      app_pace_reset();
}

static void cheats_off(int on)
{
   if (L.cheats_off == on)
      return;
   L.cheats_off = on;
   cheats_suspend(on);
   fe_evt("link_cheats suspended=%d", on);
}

static const char *fail_line(int code)
{
   const char *a, *b;
   ui_link_fail_text(code, &a, &b);
   return a;
}

/* ---- who we are: the compatibility words ------------------------------------------ */

#define LINK_ABI "vita-link-1"

static const char *backend_name(void)
{
#if defined(__arm__)
   return S.drc ? "arm" : "arm-interp";
#elif defined(__x86_64__) || defined(__i386__)
   return S.drc ? "x86" : "x86-interp";
#else
   return "interp";
#endif
}

/* The GBA BIOS the core will run: the player's file, or the built-in one.
 * Two consoles with different BIOSes are two different machines. */
static uint32_t bios_word(void)
{
   static uint8_t buf[0x4000];
   char path[APP_PATH_CAP + 16];
   uint32_t crc = 0;
   FILE *f;
   snprintf(path, sizeof(path), "%s/gba_bios.bin", app_bios_dir());
   f = fopen(path, "rb");
   if (f)
   {
      if (fread(buf, 1, sizeof(buf), f) == sizeof(buf))
         crc = fe_crc32(0, buf, sizeof(buf));
      fclose(f);
   }
   return crc;
}

static void compat_make(void)
{
   char build[160];
   snprintf(build, sizeof(build), "%s|%s|%s", GBADHOC_VERSION, LINK_ABI,
            S.build);
   S.compat[LAN_KIND_GBA_WIRELESS] = lan_compat_hash("gpSP adapter", build);
   S.compat[LAN_KIND_GB_CABLE] = lan_compat_hash("GBAdhoc GB link v2", build);
   snprintf(build, sizeof(build), "%s|%s|%s|%s|%08x", GBADHOC_VERSION,
            LINK_ABI, S.build, backend_name(), (unsigned)bios_word());
   S.compat[LAN_KIND_GBA_CABLE] = lan_compat_hash("GBAdhoc cable v1", build);
   /* netdrv matches the whole string at JOIN: 23 characters at most. */
   snprintf(S.proto_cable, sizeof(S.proto_cable), "GBAdhoc cable v1/%06x",
            (unsigned)(S.compat[LAN_KIND_GBA_CABLE] & 0xFFFFFFu));
   snprintf(S.proto_gb, sizeof(S.proto_gb), "GBAdhoc GB link v2/%04x",
            (unsigned)(S.compat[LAN_KIND_GB_CABLE] & 0xFFFFu));
   fe_evt("link_compat backend=%s adapter=%08x cable=%08x gb=%08x",
          backend_name(), (unsigned)S.compat[0], (unsigned)S.compat[1],
          (unsigned)S.compat[2]);
}

/* ---- boot, per game ------------------------------------------------------------------ */

void link_boot(void)
{
   char ini[APP_PATH_CAP + 16];
   int rc;

   memset(&L, 0, sizeof(L));
   L.signal_pct = -1;
   snprintf(ini, sizeof(ini), "%s/vita.ini", g_app_root);
   S.port    = (int)fe_ini_get_int(ini, "link_port", 0);
   S.loss    = (int)fe_ini_get_int(ini, "link_loss", 0);
   S.dup     = (int)fe_ini_get_int(ini, "link_dup", 0);
   S.latency = (int)fe_ini_get_int(ini, "link_latency", 0);
   S.jitter  = (int)fe_ini_get_int(ini, "link_jitter", 0);
   S.absorb  = (int)fe_ini_get_int(ini, "link_absorb", 1) != 0;
   S.copies  = (int)fe_ini_get_int(ini, "link_copies", 4);
   S.nack    = (int)fe_ini_get_int(ini, "link_nack", 1) != 0;
   S.drc     = (int)fe_ini_get_int(ini, "drc", 1) != 0;
   if (!fe_ini_get(ini, "link_build", S.build, sizeof(S.build)))
      S.build[0] = '\0';
   if (!fe_ini_get(ini, "link_upnp", S.upnp, sizeof(S.upnp)))
      S.upnp[0] = '\0';
   S.upnp_ms = (int)fe_ini_get_int(ini, "link_upnp_ms", 1500);
   if (S.upnp_ms < 200)
      S.upnp_ms = 200;
   {
      char pk[40];
      S.peek_addr = S.peek_words = 0;
      if (fe_ini_get(ini, "link_peek", pk, sizeof(pk)))
      {
         char *colon = strchr(pk, ':');
         S.peek_addr = (unsigned)strtoul(pk, NULL, 16);
         S.peek_words = colon ? (unsigned)strtoul(colon + 1, NULL, 10) : 8;
         if (S.peek_words > 48)
            S.peek_words = 48;
      }
   }
   if (S.port < 0 || S.port > 65000)
      S.port = 0;
   if (S.copies < 0 || S.copies > 16)
      S.copies = 4;
   /* The PSP's player build has the exit assist on (PLAY_EXIT_ASSIST). */
   S.exit_assist = (int)fe_ini_get_int(ini, "rfu_exit_assist", 1) != 0;

   rc = net_vita_init();
   S.up = rc == 0;
   fe_evt("link_boot net=%d sce=0x%08x port=%d fault=%d/%d/%d/%d absorb=%d "
          "copies=%d nack=%d", S.up, (unsigned)net_vita_last_sce_error(),
          S.port ? S.port : LAN_PORT_DEFAULT, S.loss, S.dup, S.latency,
          S.jitter, S.absorb, S.copies, S.nack);
   compat_make();
}

void link_shutdown(void)
{
   link_teardown();
   if (R.scan)
      ui_host_rooms_end();
   if (L.igd)
   {
      /* A mapping not removed expires with its lease (two hours). */
      upnp_igd_close(L.igd);
      L.igd = NULL;
   }
   if (S.up)
      net_vita_term();
   S.up = 0;
}

/* ---- fe_host's link hooks ----------------------------------------------------------- */

/* fe_host_config.gblink_wait: the partner's buttons are late.  Pump the
 * network and say whether this display frame still has time to finish: stop
 * once its refresh has passed, or once the rest of the frame (its usual
 * work and the present) would no longer fit.  The machines advance on the
 * inputs alone, so this changes when a step returns, never what it
 * computes (psp/main_psp.c plat_gblink_wait). */
static int link_wait(uint32_t waited_us)
{
   uint64_t now = link_now_us();
   uint32_t elapsed;

   if (waited_us && L.wait_prev)
      L.frame_wait_us += (uint32_t)(now - L.wait_prev);
   L.wait_prev = now;
   if (!S.absorb || waited_us >= WAIT_MAX_US)
      return 0;
   if ((int)(app_vcount() - g_app_vc_target) >= 1)
      return 0;
   elapsed = (uint32_t)(now - g_app_frame_t0);
   if (elapsed + L.work_ema + WAIT_PRESENT_US >= FRAME_US)
      return 0;
   fe_np_pump();
   /* Nothing new: give the processor up for a moment instead of spinning
    * (the transport's I/O thread is what fills the ring). */
   if (!net_vita_rx_depth())
      plat_sleep_us(100);
   return 1;
}

/* The partner's GB cartridge in this console's library: a folder and its
 * first level of folders (roms/, then the extra folders: link_find_rom), by
 * exact size and header.  The session checks the SHA-1 itself. */
static int find_rom_in(const char *dir, uint32_t size,
                       const uint8_t header[0x1C], char *path, size_t cap,
                       int depth)
{
   DIR *d = opendir(dir);
   struct dirent *e;
   int found = 0;

   if (!d)
      return 0;
   while (!found && (e = readdir(d)) != NULL)
   {
      char p[APP_PATH_CAP];
      const char *dot = strrchr(e->d_name, '.');
      struct stat st;
      if (e->d_name[0] == '.')
         continue;
      if (snprintf(p, sizeof(p), "%s/%s", dir, e->d_name) >= (int)sizeof(p) ||
          stat(p, &st) != 0)
         continue;
      if (S_ISDIR(st.st_mode))
      {
         if (depth == 0)
            found = find_rom_in(p, size, header, path, cap, 1);
         continue;
      }
      if (!dot || (strcasecmp(dot, ".gb") && strcasecmp(dot, ".gbc")) ||
          (uint32_t)st.st_size != size)
         continue;
      {
         FILE *f = fopen(p, "rb");
         uint8_t h[0x1C];
         if (!f)
            continue;
         if (fseek(f, 0x134, SEEK_SET) == 0 &&
             fread(h, 1, sizeof(h), f) == sizeof(h) &&
             !memcmp(h, header, sizeof(h)))
         {
            snprintf(path, cap, "%s", p);
            found = 1;
         }
         fclose(f);
      }
   }
   closedir(d);
   return found;
}

static int link_find_rom(uint32_t size, const uint8_t header[0x1C], char *path,
                         size_t cap)
{
   int i;
   /* roms/ first (it answers as it always did), then the extra folders the
    * game list shows (ui_host_extra_roots): the partner's game may be one
    * the player keeps there. */
   if (find_rom_in(g_app_rom_dir, size, header, path, cap, 0))
      return 1;
   for (i = 0; i < ui_host_extra_roots(); i++)
      if (find_rom_in(ui_host_extra_root(i), size, header, path, cap, 0))
         return 1;
   return 0;
}

void link_host_config(fe_host_config *cfg)
{
   cfg->gb_find_rom         = link_find_rom;
   cfg->gb_bulk_enable      = net_vita_bulk_enable;
   cfg->gb_bulk_send        = net_vita_bulk_send;
   cfg->gb_bulk_recv        = net_vita_bulk_recv;
   cfg->gb_bulk_max         = BULK_BYTES;
   cfg->gblink_wait         = link_wait;
   /* The PSP ships with the redundant datagram off: a send costs it a
    * millisecond of the emulation thread.  Here it is free and it is the
    * cheapest cure for a lost datagram (net_vita.c has the measurements). */
   cfg->gblink_input_copies = (unsigned)S.copies;
   cfg->gblink_nack         = S.nack;
   cfg->gblink_live         = 1;     /* link from the running game */
   cfg->gblink_batch        = GB_BATCH;
}

/* The emulated Wireless Adapter as the PSP's player build tunes it
 * (psp/main_psp.c, the PLAY_* block; each number's story is there).  They
 * are properties of the adapter emulation and of gen-3's link protocol, not
 * of the PSP's radio, except the two poll cadences, which only matter where
 * a poll is expensive and cost nothing here. */
extern void rfu_set_rx_cap(unsigned n);
extern void rfu_set_frame_pace(unsigned n);
extern void rfu_set_pace_max_hold(unsigned n);
extern void rfu_set_cushion(unsigned n);
extern void rfu_set_shed_keep(unsigned n);
extern void rfu_set_hold(unsigned on);
extern void rfu_set_timeout_scale(unsigned to_x100, unsigned rtx_x100);
extern void rfu_set_poll_min_cycles(unsigned n);
extern void rfu_set_idle_poll_cycles(unsigned n);
extern void rfu_set_disc_defer(unsigned n);
extern void rfu_set_disc_grace(unsigned frames);

static void rfu_tune(void)
{
   int hold = g_pcfg.rfu_pace_max_hold;
   if (hold < 0)  hold = 0;
   if (hold > 28) hold = 28;         /* 32 is the game's link timeout */
   rfu_set_rx_cap((unsigned)(g_pcfg.rfu_rx_cap > 0 ? g_pcfg.rfu_rx_cap : 0));
   rfu_set_frame_pace(1);            /* one delivery a frame: the game's rate */
   rfu_set_pace_max_hold((unsigned)hold);
   rfu_set_cushion(0);
   rfu_set_shed_keep(2);             /* bound the standing latency           */
   rfu_set_hold(1);                  /* hold, never discard                  */
   rfu_set_timeout_scale(100, 100);
   rfu_set_poll_min_cycles(34952);   /* an eighth of a frame                 */
   rfu_set_idle_poll_cycles(34952);
   rfu_set_disc_defer(0);
   rfu_set_disc_grace(120);          /* the game's own clean disconnect      */
   fe_evt("rfu_tuning frame_pace=1 max_hold=%d shed_keep=2 hold=1 "
          "disc_grace=120 poll=34952 exit_assist=%d", hold, S.exit_assist);
}

void link_game_begin(void)
{
   L.error = NULL;
   L.resume_note = 0;
   L.work_ema = 6000;
   L.prev_pad = g_app_pad;
   compat_make();                    /* the BIOS may have appeared */
   if (is_gba())
      rfu_tune();
}

/* vita.ini link_peek: what a test cartridge says about the link, from the
 * player's machine's memory. */
static void peek_evt(const char *when)
{
   char line[48 * 9 + 8];
   unsigned i, n = 0;

   if (!S.peek_words)
      return;
   for (i = 0; i < S.peek_words; i++)
   {
      uint32_t w = 0;
      if (fe_host_mem_read(S.peek_addr + 4u * i, &w, 4) != 0)
         break;
      n += (unsigned)snprintf(line + n, sizeof(line) - n, "%s%08x",
                              i ? "," : "", (unsigned)w);
   }
   if (i)
      fe_evt("link_peek when=%s frame=%u addr=%08x w=%s", when,
             fe_host_frame_count(), S.peek_addr, line);
}

/* ---- the exit assist (psp/main_psp.c exit_assist_frame; ADR-0079) ---------------------
 * Pokemon Emerald (USA), leaving the Trade Center over the adapter: the
 * child's own EXIT_ROOM key is echoed back by the parent in the same
 * exchange on real hardware, and over any transport with latency the parent
 * has moved on before the echo can be sent.  When every condition of that
 * lost-echo state is provably present, write the one byte the echo would
 * have written; the game then runs its own negotiated exit.  The pointer
 * value is unique to that ROM, so no other game can match. */
static void exit_assist_frame(void)
{
   unsigned kcb = 0;
   unsigned char ka = 0, ls[4];
   int local, peer;

   if (!S.exit_assist || !L.net_up || L.kind != LK_ADAPTER)
      return;
   if (fe_host_mem_read(0x03000E14, &kcb, 4) != 0 || kcb != 0x080871C5u)
      return;
   if (fe_host_mem_read(0x03000E19, &ka, 1) != 0 || ka < 45)
      return;
   if (fe_host_mem_read(0x03000E10, ls, 4) != 0)
      return;
   local = L.is_host ? 0 : 1;
   peer = 1 - local;
   if (ls[local] != 0x81 /* BUSY */ || ls[peer] != 0x83 /* EXITING */)
      return;
   ls[local] = 0x83;
   if (fe_host_mem_write(0x03000E10 + (unsigned)local, &ls[local], 1) == 0)
      fe_evt("rfu_exit_assist local=%d ka=%u", local, ka);
}

/* ---- the radio ------------------------------------------------------------------------ */

static unsigned lan_flags(uint32_t ip_be)
{
   /* 127.x is the twin's "Wi-Fi": broadcasts stay on the loopback. */
   return (ntohl(ip_be) >> 24) == 127 ? LAN_F_LOOPBACK : 0;
}

/* Wi-Fi / Internet: the socket.  0, or a UI_FAIL_* code + 1 negated. */
static int infra_open(int is_host, uint32_t target_ip_be, uint16_t target_port)
{
   net_vita_wifi w;
   lan_config c;

   if (!S.up || !net_vita_wifi_status(&w))
      return -(UI_FAIL_NO_WIFI + 1);
   if (net_vita_claim(NET_VITA_INFRA) != 0)
      return -(UI_FAIL_RADIO_BUSY + 1);
   memset(&c, 0, sizeof(c));
   c.mode = is_host ? LAN_MODE_HOST : LAN_MODE_JOIN;
   c.disc_port = (uint16_t)S.port;
   c.local_ip_be = w.ip_be;
   c.netmask_be = w.netmask_be;
   c.flags = lan_flags(w.ip_be);
   c.target_ip_be = target_ip_be;
   c.target_port = target_port;
   L.lan = lan_transport_create(&c);
   if (!L.lan)
   {
      fe_evt("link_error stage=lan_create why=\"%s\"", lan_last_error());
      net_vita_release(NET_VITA_INFRA);
      return -(UI_FAIL_PORT + 1);
   }
   if (S.loss | S.dup | S.latency | S.jitter)
      lan_transport_set_fault(L.lan, S.loss, S.dup, S.latency, S.jitter,
                              (uint32_t)link_now_us() | 1u);
   net_vita_link_lan(L.lan);
   L.transport = TR_LAN;
   L.signal_pct = w.rssi_pct;
   L.strikes = 0;
   lan_transport_addr_str(L.lan, L.host_addr, sizeof(L.host_addr));
   fe_evt("link_lan mode=%s addr=%s loopback=%d", is_host ? "host" : "join",
          L.host_addr, (c.flags & LAN_F_LOOPBACK) != 0);
   return 0;
}

static void announce(int players, int open)
{
   lan_room_info ri;
   char code[5];

   if (L.transport != TR_LAN || !L.is_host || !L.lan)
      return;
   if (players == L.players && open == L.open)
      return;
   L.players = players;
   L.open = open;
   memset(&ri, 0, sizeof(ri));
   if (L.room_kind == LAN_KIND_GB_CABLE)
      room_code_gb(g_game.gb_title, sizeof(g_game.gb_title), code);
   else
      room_code_gba(g_game.code, code);
   room_make(L.room_kind, L.number, code, ri.name, sizeof(ri.name));
   ri.number = (uint8_t)L.number;
   ri.kind = (uint8_t)L.room_kind;
   ri.players = (uint8_t)players;
   ri.max_players = L.kind == LK_ADAPTER ? ND_MAX_CLIENTS : 2;
   ri.flags = open ? LAN_ROOM_F_OPEN : LAN_ROOM_F_BUSY;
   snprintf(ri.nick, sizeof(ri.nick), "%.15s", g_pcfg.nick);
   snprintf(ri.title, sizeof(ri.title), "%.19s", g_game.name);
   memcpy(ri.code, code, sizeof(ri.code));
   ri.compat = S.compat[L.room_kind];
   lan_transport_announce(L.lan, &ri);
   fe_evt("link_room name=%s number=%d kind=%d players=%d/%d open=%d "
          "title=\"%s\" nick=\"%s\" compat=%08x", ri.name, L.number,
          L.room_kind, players, ri.max_players, open, ri.title, ri.nick,
          (unsigned)ri.compat);
}

/* ---- Internet: the router (upnp_igd.h; all of it inside poll) ------------------------- */

static void igd_start(void)
{
   uint32_t ip = lan_transport_ip(L.lan);
   uint16_t port = lan_transport_port(L.lan);

   L.pub_state = UI_PUBLIC_UNKNOWN;
   L.pub_addr[0] = '\0';
   if (!strcasecmp(S.upnp, "off"))
   {
      L.pub_state = UI_PUBLIC_CLOSED;
      return;
   }
   if (L.igd)                         /* the last room's goodbye, unfinished */
      upnp_igd_close(L.igd);
   L.igd_removing = 0;
   L.igd = upnp_igd_open(ip, port, port, "GBAdhoc");
   if (!L.igd)
   {
      L.pub_state = UI_PUBLIC_CLOSED;
      return;
   }
   if (S.upnp[0])
   {
      uint32_t tip = 0;
      uint16_t tport = 0;
      if (lan_parse_addr(S.upnp, &tip, &tport, 1900) == 0)
         upnp_igd_test_target(L.igd, tip, tport, (unsigned)S.upnp_ms);
   }
   L.pub_state = UI_PUBLIC_ASKING;
   fe_evt("link_upnp start port=%u", port);
}

static void igd_service(void)
{
   int st;
   if (!L.igd)
      return;
   st = upnp_igd_poll(L.igd);
   if (L.igd_removing)
   {
      if (st == UPNP_IGD_REMOVED || st == UPNP_IGD_FAILED ||
          L.ticks - L.igd_t > T_IGD_REMOVE)
      {
         fe_evt("link_upnp removed state=%d", st);
         upnp_igd_close(L.igd);
         L.igd = NULL;
         L.igd_removing = 0;
      }
      return;
   }
   if (L.pub_state != UI_PUBLIC_ASKING)
      return;
   if (st == UPNP_IGD_MAPPED)
   {
      int priv = upnp_igd_external_is_private(L.igd);
      const char *ip = upnp_igd_external_ip(L.igd);
      /* A router may open the port and still not say its own address: the
       * way in is open, and the page says where to look the address up
       * (an empty address with UI_PUBLIC_OPEN). */
      if (ip && ip[0])
         snprintf(L.pub_addr, sizeof(L.pub_addr), "%s:%u", ip,
                  (unsigned)upnp_igd_external_port(L.igd));
      else
         L.pub_addr[0] = '\0';
      /* A "public" address that is itself private: the router sits behind
       * another one (carrier-grade NAT) and nobody outside can come in,
       * however well the mapping went. */
      L.pub_state = priv ? UI_PUBLIC_CLOSED : UI_PUBLIC_OPEN;
      fe_evt("link_upnp mapped addr=%s port=%u private=%d",
             L.pub_addr[0] ? L.pub_addr : "?",
             (unsigned)upnp_igd_external_port(L.igd), priv);
   }
   else if (st == UPNP_IGD_FAILED)
   {
      const char *ip = upnp_igd_external_ip(L.igd);
      if (ip && ip[0])
         snprintf(L.pub_addr, sizeof(L.pub_addr), "%s:%u", ip,
                  (unsigned)lan_transport_port(L.lan));
      L.pub_state = UI_PUBLIC_CLOSED;
      fe_evt("link_upnp failed why=\"%s\"", upnp_igd_error(L.igd));
      upnp_igd_close(L.igd);
      L.igd = NULL;
   }
}

static void igd_stop(void)
{
   L.pub_state = UI_PUBLIC_UNKNOWN;
   L.pub_addr[0] = '\0';
   if (!L.igd || L.igd_removing)
      return;
   if (upnp_igd_state(L.igd) == UPNP_IGD_MAPPED)
   {
      upnp_igd_remove(L.igd);        /* finishes in igd_service */
      L.igd_removing = 1;
      L.igd_t = L.ticks;
   }
   else
   {
      upnp_igd_close(L.igd);
      L.igd = NULL;
   }
}

/* ---- Share game data (vita/share_vita.c) -------------------------------- */

int link_net_info(uint16_t *disc_port)
{
   if (disc_port)
      *disc_port = (uint16_t)S.port;
   return S.up;
}

/* ---- netdrv over whichever transport --------------------------------------------------- */

static int np_start(void)
{
   static nd_transport tp;
   fe_np_config npc;
   uint32_t first = 0, floor_us = 0;
   int rc;

   if (net_vita_link_iface(&tp) != 0)
      return -1;
   net_vita_arq_profile(L.transport == TR_ADHOC ? NET_VITA_LINK_ADHOC
                        : L.conn == LINK_CONN_INTERNET ? NET_VITA_LINK_INTERNET
                                                       : NET_VITA_LINK_LAN,
                        &first, &floor_us);
   fe_np_set_arq_timers(first, floor_us);
   {
      /* GBAdhoc 4.0: in a running GBA Link Cable session a bare ack waits
       * up to 30 ms for an input packet to ride on (fe_host switches it on
       * at RUNNING and off at the close; Street Fighter 45.2 -> 47.6 frames
       * a second on PSP hardware with this alone).  Kept under the
       * retransmit floor less one pump period, or the sender resends what
       * arrived (main_psp.c's clamp, with this transport's floor). */
      int fl = floor_us ? (int)floor_us : 30000;
      int ride_max = fl - 17000, ride = 30000;
      if (ride_max < 0)
         ride_max = 0;
      if (ride > ride_max)
         ride = ride_max;
      fe_np_set_ack_ride_product((uint32_t)ride);
      fe_evt("net_tuning ack_ride_cfg product_us=%d max_us=%d", ride,
             ride_max);
   }
   fe_np_set_gb_protocol(L.kind == LK_CABLE ? S.proto_cable
                         : L.kind == LK_GB ? S.proto_gb : NULL);
   memset(&npc, 0, sizeof(npc));
   npc.transport = &tp;
   npc.is_host = L.is_host;
   npc.nick = g_pcfg.nick;
   npc.now_us = link_now_us;
   rc = L.kind == LK_ADAPTER ? fe_np_start(&npc) : fe_np_start_gb(&npc);
   if (rc != 0)
   {
      fe_evt("link_error stage=np_start why=\"%s\"", fe_np_start_reason());
      return -1;
   }
   L.net_up = 1;
   fe_evt("link_net_up kind=%d host=%d arq=%u/%u proto=\"%s\"", L.kind,
          L.is_host, (unsigned)first, (unsigned)floor_us,
          L.kind == LK_CABLE ? S.proto_cable : L.kind == LK_GB ? S.proto_gb
                                                               : "core");
   return 0;
}

static void np_stop(void)
{
   if (!L.net_up)
      return;
   if (L.transport == TR_LAN && L.lan)
   {
      lan_stats s;
      lan_transport_get_stats(L.lan, &s);
      fe_evt("lan_stats tx=%u wouldblock=%u txfail=%u tx_us=%u/%u rx=%u "
             "ringdrop=%u bulk=%u/%u bulkdrop=%u fault=%u/%u/%u pollerr=%u "
             "nat=%u", s.tx_pkts, s.tx_would_block, s.tx_fail,
             s.tx_pkts ? s.tx_us / s.tx_pkts : 0u, s.tx_max_us, s.rx_pkts,
             s.rx_ring_drop, s.bulk_tx, s.bulk_rx, s.bulk_drop,
             s.fault_dropped, s.fault_delayed, s.fault_dup, s.io_poll_err,
             s.nat_aliases);
   }
   else if (L.transport == TR_ADHOC)
   {
      vadhoc_stats s;
      vadhoc_get_stats(&s);
      fe_evt("vadhoc_stats tx=%u wouldblock=%u txfail=%u tx_us=%u/%u rx=%u "
             "ringdrop=%u bulk=%u/%u bcast=%u+%u", s.tx_pkts,
             s.tx_would_block, s.tx_fail,
             s.tx_calls ? s.tx_us / s.tx_calls : 0u, s.tx_max_us, s.rx_pkts,
             s.rx_ring_drop, s.bulk_tx, s.bulk_rx, s.bcast_calls,
             s.bcast_unicast);
   }
   fe_np_stop();                     /* BYE to whoever is there */
   L.net_up = 0;
}

/* Everything of the network goes: the session's driver, the bulk lane, the
 * transport, the claim on the radio.  The link session itself (fe_gblink),
 * if there is one, is NOT closed here: it has seen its partner go and its
 * end is link_end()'s.  Bounded: joining the transport's I/O thread takes
 * one of its poll slices (50 ms; ad hoc 250 ms). */
static void net_down(void)
{
   np_stop();
   net_vita_bulk_enable(0);
   net_vita_link_none();
   if (L.transport == TR_LAN)
   {
      igd_stop();
      lan_transport_destroy(L.lan);
      L.lan = NULL;
      net_vita_release(NET_VITA_INFRA);
   }
   else if (L.transport == TR_ADHOC)
      vadhoc_transport_close();
   if (L.adhoc_stack)
   {
      /* HARDWARE: the console goes back to its access point by itself
       * after this (transport_vadhoc.c H11); the menus wait for
       * net_vita_wifi_status() to say so before offering Wi-Fi again. */
      vadhoc_net_term();
      net_vita_release(NET_VITA_ADHOC);
      L.adhoc_stack = 0;
   }
   if (L.transport != TR_NONE)
      fe_evt("link_net_down radio=%d", net_vita_use());
   L.transport = TR_NONE;
   L.players = L.open = L.peers = 0;
   L.host_addr[0] = '\0';
   L.strikes = 0;
   fe_np_set_gb_protocol(NULL);
}

/* The single game is the player's again. */
static void link_off(void)
{
   hold_game(0);
   cheats_off(0);
   L.kind = LK_NONE;
   L.why = LW_NONE;
   L.clear = 0;
   L.fail_hold = 0;
   L.flip_now = 0;
   L.ended = 0;
   L.end_asked = 0;
   L.screen = 0;
   L.peer[0] = '\0';
   phase_set(PH_OFF);
   chip_update();
}

/* A Host that did not work: the waiting page reads the reason from the
 * view.  A Join: the LINKING screen shows it until O. */
static void bringup_failed(int code)
{
   fe_evt("link_failed code=%d host=%d phase=%d", code, L.is_host, L.phase);
   net_down();
   if (L.is_host)
   {
      link_off();
      L.error = fail_line(code);
      /* The waiting page reads it from the view; a host who went back to
       * his game ("Keep playing") gets it there. */
      if (!ui_active())
         osd_toast("%s", L.error);
      return;
   }
   L.fail_code = code;
   L.screen = 1;
   hold_game(1);
   phase_set(PH_FAILED);
}

/* ---- the session (cable / GB): start ------------------------------------------------------- */

static unsigned delay_pick(const char **how)
{
   unsigned d, base = L.conn == LINK_CONN_INTERNET ? DELAY_INTERNET
                    : L.conn == LINK_CONN_ADHOC ? DELAY_ADHOC : DELAY_WIFI;
   uint32_t mn = 0, mx = 0, last = 0;
   int n;

   *how = "fixed";
   if (g_pcfg.link_delay >= 1)
      return g_pcfg.link_delay > DELAY_MAX ? DELAY_MAX
                                           : (unsigned)g_pcfg.link_delay;
   *how = "kind";
   n = L.transport == TR_LAN
          ? lan_transport_echo_result(L.lan, &mn, &mx, &last) : 0;
   if (n < 2)
      return base;
   /* One frame for the two consoles' phase (they are not synchronised: up
    * to a frame apart), plus what the trip one way and its spread need.
    * The spread is taken from the round trips, so it is counted twice over:
    * the margin for the samples being few. */
   d = 1 + (mx / 2 + (mx - mn) + FRAME_US - 1) / FRAME_US;
   if (d < 2)
      d = 2;
   /* An internet path's quiet moment is not its evening. */
   if (L.conn == LINK_CONN_INTERNET && d < 3)
      d = 3;
   if (d > DELAY_MAX)
      d = DELAY_MAX;
   *how = "measured";
   fe_evt("link_rtt n=%d min_us=%u max_us=%u last_us=%u", n, (unsigned)mn,
          (unsigned)mx, (unsigned)last);
   return d;
}

/* Start the link session over the driver that is up (psp/main_psp.c
 * gblink_begin).  The LINKING screen draws over the game as it stood. */
static int session_begin(void)
{
   const char *how = "host";
   int rc;

   if (ui_active())
      ui_close();                    /* the session's setup needs the screen */
   app_snapshot_take();
   app_backdrop_from_snapshot();
   L.why = LW_NONE;
   L.t_setup = L.ticks;
   L.ended = 0;
   L.fail_hold = 0;
   L.end_asked = 0;
   L.stalled = 0;
   L.stall_told = 0;
   L.prev_pad = g_app_pad;           /* a held O is not a cancel */
   L.delay = L.is_host ? delay_pick(&how) : 0;
   /* From before the machines start: every code leaves the core. */
   cheats_off(1);
   hold_game(0);
   /* Fast link (Settings): decides only when this console hosts; a guest
    * runs the host's (fe_host.h). */
   fe_host_set_fast_link(g_pcfg.fast_link);
   fe_evt("fast_link_setting own=%d role=%s", g_pcfg.fast_link,
          L.is_host ? "host" : "guest");
   rc = fe_host_gblink_start(L.is_host, L.delay, 0);
   app_frame_forget();               /* the single machine's frame is gone */
   app_audio_flush();
   app_pace_reset();
   fe_evt("link_session_begin role=%s delay=%u how=%s rc=%d",
          L.is_host ? "host" : "guest", L.delay, how, rc);
   if (rc == -3)
   {
      /* the game could not be put back: nothing to return to */
      net_down();
      link_off();
      app_quit_game("The link could not start and the game had to close.");
      return rc;
   }
   if (rc != 0)
   {
      net_down();
      link_off();
      if (L.is_host)
         L.error = fail_line(UI_FAIL_MEMORY);
      osd_toast("The link could not start");
      return rc;
   }
   L.screen = 1;
   phase_set(PH_LINKED);
   return 0;
}

/* ---- the menu's actions ---------------------------------------------------------------- */

/* After the transport is open and claimed: the driver, and where that
 * leaves a host or a joiner of each kind. */
static void after_transport(void)
{
   if (np_start() != 0)
   {
      bringup_failed(UI_FAIL_MEMORY);
      return;
   }
   /* The adapter's state is not in a save state, nor is a cheat's write
    * something the other console's game expects: both are the session's
    * from here (docs: states locked while linked). */
   if (L.kind == LK_ADAPTER)
      cheats_off(1);
   if (L.is_host)
   {
      L.players = L.open = -1;       /* force the first announcement */
      announce(1, 1);
      if (L.transport == TR_LAN && L.conn == LINK_CONN_INTERNET)
         igd_start();
      phase_set(PH_HOSTING);
      hold_game(0);
      fe_evt("link_hosting room=\"%s\" kind=%d addr=%s", L.room, L.kind,
             L.host_addr);
      return;
   }
   if (L.kind == LK_ADAPTER)
   {
      L.screen = 1;
      hold_game(1);
      phase_set(PH_JOINING);
      return;
   }
   session_begin();                  /* cable joiner, GB joiner */
}

static void adhoc_start(void)
{
   int rc;

   if (!S.up || net_vita_claim(NET_VITA_ADHOC) != 0)
   {
      bringup_failed(UI_FAIL_RADIO_BUSY);
      return;
   }
   rc = vadhoc_net_init(NULL);
   if (rc != VADHOC_OK && rc != VADHOC_ERR_ALREADY)
   {
      fe_evt("link_error stage=adhoc_init rc=%d sce=0x%08x at=%s", rc,
             (unsigned)vadhoc_last_sce_error(), vadhoc_stage());
      net_vita_release(NET_VITA_ADHOC);
      bringup_failed(UI_FAIL_PORT);
      return;
   }
   L.adhoc_stack = 1;
   /* The cell is left unnamed, as both programs that work on hardware
    * leave it (VitaShell, vitaQuake).  HARDWARE: whether the firmware would
    * honour a name is unknown (transport_vadhoc.c H6), and a joiner has no
    * list to learn the host's room from, so a name could only keep the two
    * apart.  Inside the cell netdrv's protocol string picks the partner. */
   vadhoc_dialog_set_group(NULL);
   /* HARDWARE (H5): CREATE for the host, JOIN with the firmware's 60 s
    * search for the joiner: the PSP's lesson (a joiner that may create
    * makes a second cell of its own). */
   rc = vadhoc_dialog_start(L.is_host ? VADHOC_DIALOG_CREATE
                                      : VADHOC_DIALOG_JOIN,
                            L.is_host ? 0u : 60000000u);
   if (rc != VADHOC_OK)
   {
      fe_evt("link_error stage=adhoc_dialog rc=%d sce=0x%08x", rc,
             (unsigned)vadhoc_last_sce_error());
      bringup_failed(UI_FAIL_PORT);
      return;
   }
   vid_set_common_dialog(1);         /* vid_swap lets the dialog draw */
   L.dlg_abort = 0;
   if (!L.is_host)
   {
      L.screen = 1;
      hold_game(1);
   }
   phase_set(PH_DIALOG);
}

/* The connection screen has finished, one way or another. */
static void adhoc_dialog_done(int st)
{
   int rc;

   vadhoc_dialog_end();
   vid_set_common_dialog(0);
   fe_evt("link_adhoc_dialog state=%d abort=%d sce=0x%08x frames=%u", st,
          L.dlg_abort, (unsigned)vadhoc_last_sce_error(), phase_age());
   if (L.dlg_abort == 1)
   {
      /* "never mind": back to nothing, with no reason to give */
      net_down();
      link_off();
      if (!L.is_host)
         osd_toast("Link cancelled.");
      return;
   }
   if (st != VADHOC_DLG_OK)
   {
      /* closed by the player; else it failed or ran out of time: a joiner
       * found nobody, a host could not open the cell */
      bringup_failed(st == VADHOC_DLG_CANCELLED && L.dlg_abort == 0
                        ? UI_FAIL_ADHOC_CANCELLED
                        : L.is_host ? UI_FAIL_PORT : UI_FAIL_NO_ANSWER);
      return;
   }
   rc = vadhoc_transport_open();
   if (rc != VADHOC_OK)
   {
      fe_evt("link_error stage=adhoc_open rc=%d sce=0x%08x at=%s", rc,
             (unsigned)vadhoc_last_sce_error(), vadhoc_stage());
      bringup_failed(UI_FAIL_PORT);
      return;
   }
   if (S.loss | S.latency | S.jitter)
      vadhoc_set_fault(S.latency, S.jitter, S.loss,
                       (uint32_t)link_now_us() | 1u);
   net_vita_link_adhoc();
   L.transport = TR_ADHOC;
   L.strikes = 0;
   L.signal_pct = -1;
   after_transport();
}

void link_action(int is_host)
{
   const ui_join_info *t = ui_join_target();
   int gba = is_gba(), rk, rc;

   L.error = NULL;
   if (L.phase != PH_OFF || fe_host_gblink_active())
   {
      if (!is_host)
         osd_toast("Already linked. Disconnect first.");
      return;
   }
   L.is_host = is_host;
   L.conn = is_host ? g_pcfg.net_conn : t->conn;
   if (L.conn < 0 || L.conn >= LINK_CONN_COUNT)
      L.conn = LINK_CONN_WIFI;
   if (is_host)
   {
      rk = !gba ? ROOM_CABLE_GB : ui_host_cable() ? ROOM_CABLE_GBA
                                                  : ROOM_ADAPTER;
      L.number = room_number(ui_group());
   }
   else
   {
      rk = t->kind;
      L.number = t->number;
      snprintf(L.peer, sizeof(L.peer), "%s", t->nick);
   }
   if (L.number < 0)
      L.number = 0;
   /* An ad hoc joiner has no list to learn the host's room number from:
    * its screens name the connection instead of guessing a room. */
   if (!is_host && L.conn == LINK_CONN_ADHOC)
      snprintf(L.room, sizeof(L.room), "Ad hoc");
   else
      snprintf(L.room, sizeof(L.room), ROOM_LABEL_WORD " %d", L.number);
   /* What the room is and what this game can be in it: the Join list has
    * already refused the pairs that cannot work; a room that is not this
    * console's kind at all is refused here too. */
   if (rk != ROOM_ADAPTER && rk != ROOM_CABLE_GBA && rk != ROOM_CABLE_GB)
      rk = gba ? ROOM_ADAPTER : ROOM_CABLE_GB;
   if ((rk == ROOM_CABLE_GB) != !gba)
   {
      L.kind = LK_NONE;
      bringup_failed(UI_FAIL_GAME);
      return;
   }
   L.room_kind = rk;
   L.kind = rk == ROOM_CABLE_GB ? LK_GB : rk == ROOM_CABLE_GBA ? LK_CABLE
                                                               : LK_ADAPTER;
   L.flips = 0;
   L.flip_now = 0;
   L.can_flip = !is_host && L.conn == LINK_CONN_ADHOC && gba &&
                ui_host_mode() == UI_HOST_ASK && fe_host_gba_cable_ready();
   L.why = LW_NONE;
   L.prev_pad = g_app_pad;
   fe_evt("link_action role=%s conn=%d kind=%d room=%d listed=%d addr=%s",
          is_host ? "host" : "join", L.conn, L.kind, L.number,
          is_host ? 0 : t->listed, is_host ? "-" : t->addr);
   if (L.kind == LK_CABLE && !fe_host_gba_cable_ready())
   {
      /* this boot has no second machine: not enough memory at load */
      bringup_failed(UI_FAIL_MEMORY);
      return;
   }
   if (!is_host)
   {
      app_snapshot_take();
      app_backdrop_from_snapshot();
   }

   if (L.conn == LINK_CONN_ADHOC)
   {
      adhoc_start();
      return;
   }
   if (is_host)
      rc = infra_open(1, 0, 0);
   else
   {
      uint32_t ip = 0;
      uint16_t port = 0;
      if (!t->listed ||
          lan_parse_addr(t->addr, &ip, &port,
                         (uint16_t)(S.port ? S.port : LAN_PORT_DEFAULT)) != 0)
      {
         bringup_failed(UI_FAIL_ADDRESS);
         return;
      }
      rc = infra_open(0, ip, port);
   }
   if (rc != 0)
   {
      bringup_failed(-rc - 1);
      return;
   }
   after_transport();
}

void link_disconnect(void)
{
   fe_evt("link_disconnect phase=%d session=%d", L.phase,
          fe_host_gblink_state());
   L.error = NULL;
   if (L.phase == PH_DIALOG)
   {
      /* The dialog closes a few frames after it is asked to: keep polling
       * (link_frame), then give everything back. */
      if (!L.dlg_abort)
      {
         L.dlg_abort = 1;
         vadhoc_dialog_abort();
      }
      return;
   }
   if (fe_host_gblink_active())
   {
      int st = fe_host_gblink_state();
      /* A link session ends by agreement, so both saves match: the two
       * consoles settle on a frame both can still reach and stop there
       * (link_end follows).  That takes frames, and frames only run with
       * the menu closed. */
      if (st == FE_GBLINK_SETUP)
         L.why = LW_CANCEL;
      fe_host_gblink_request_end();
      L.end_asked = 1;
      if (ui_active())
         ui_close();
      if (st == FE_GBLINK_RUNNING)
         osd_toast("Ending the link...");
      return;
   }
   if (L.phase == PH_OFF)
      return;
   {
      int hosting = L.is_host && (L.phase == PH_HOSTING ||
                                  L.phase == PH_MEASURE);
      char room[ROOM_LABEL_CAP];
      snprintf(room, sizeof(room), "%s", L.room);
      net_down();
      link_off();
      if (hosting)
         osd_toast("Stopped hosting %s", room);
      else
         osd_toast("Disconnected");
   }
}

/* Whatever is up, down, now: the game is going away, or the console is
 * going to sleep.  The other console is told (BYE) and sees "link lost". */
static void link_teardown(void)
{
   if (L.phase == PH_DIALOG)
   {
      /* A dialog cannot be left open: ask, and wait out the few frames it
       * takes (bounded; the picture on screen is the caller's). */
      int n = 0;
      vadhoc_dialog_abort();
      while (vadhoc_dialog_poll() == VADHOC_DLG_RUNNING && n++ < 120)
         plat_sleep_us(2000);
      vadhoc_dialog_end();
      vid_set_common_dialog(0);
   }
   net_down();
   if (fe_host_gblink_active())
   {
      /* The driver going took the partner with it: the session has failed
       * as "partner lost" (GBA: the player's machine carries on from where
       * it stands; GB: back to the moment the link began). */
      fe_evt("link_session_abandoned state=%d", fe_host_gblink_state());
      (void)fe_host_gblink_close();
      app_frame_forget();
   }
   if (L.phase != PH_OFF || L.kind != LK_NONE)
      link_off();
   hold_game(0);
   cheats_off(0);
}

void link_game_end(void)
{
   link_teardown();
   if (R.scan)
      ui_host_rooms_end();
   L.error = NULL;
   L.resume_note = 0;
   L.info[0] = '\0';
}

/* ---- power ---------------------------------------------------------------------------- */

void link_power(int resumed)
{
   if (!resumed)
   {
      /* The console is about to sleep.  Sockets and the ad hoc cell do not
       * survive it (HARDWARE: taken as given; a session that tried to carry
       * on would only time out on waking), and the other player should not
       * sit watching a frozen link for the eight seconds it takes his
       * console to notice: end it now, tell him (BYE), say why afterwards. */
      if (L.phase != PH_OFF || fe_host_gblink_active())
      {
         int linked = fe_host_gblink_active() || L.phase == PH_LINKED;
         fe_evt("link_suspend phase=%d session=%d", L.phase,
                fe_host_gblink_state());
         link_teardown();
         L.resume_note = linked ? LW_SLEEP : LW_RADIO;
      }
      if (R.scan)
         ui_host_rooms_end();
      return;
   }
   if (L.resume_note)
   {
      osd_toast("%s", L.resume_note == LW_SLEEP
                ? "The link ended when the Vita went to sleep."
                : "The room was closed when the Vita went to sleep.");
      fe_evt("link_resume note=%s", L.resume_note == LW_SLEEP ? "sleep"
                                                               : "room");
      L.resume_note = 0;
   }
}

/* ---- the session (cable / GB): setup, end ------------------------------------------------ */

/* While a session sets up: O cancels (the footer says so), and a joiner
 * learns what the session cannot tell it -- netdrv refused us (the room has
 * its two players), or nobody answered at all (psp/main_psp.c
 * link_setup_watch). */
static void setup_watch(unsigned pad_new)
{
   fe_gblink_progress pr;
   unsigned age = L.ticks - L.t_setup;

   if (L.why != LW_NONE)
      return;
   if (pad_new & CTL_CIRCLE)
   {
      L.why = LW_CANCEL;
      fe_evt("link_cancel ticks=%u", age);
      fe_host_gblink_request_end();
      return;
   }
   if (L.is_host)
      return;
   fe_host_gblink_facts(&pr);
   if (pr.peer_up)
      return;
   if (fe_np_stop_reason() == ND_STOP_REFUSED)
      L.why = LW_FULL;
   else if (L.can_flip && age > T_ADHOC_FLIP && L.flips < 3)
   {
      /* Ad hoc has no room list: whether the host of a game that links
       * both ways opened a Link Cable or a Wireless Adapter room is a
       * guess.  Nobody answered this one: ask as the other. */
      L.flips++;
      L.flip_now = 1;                /* link_end: not a failure, a retry */
      fe_evt("link_adhoc_flip to=adapter n=%d", L.flips);
      L.why = LW_NOANSWER;
      fe_host_gblink_request_end();
      return;
   }
   else if (age > (L.conn == LINK_CONN_ADHOC ? T_ADHOC_ANSWER : T_JOIN_ANSWER))
      L.why = LW_NOANSWER;
   if (L.why != LW_NONE)
   {
      fe_evt("link_join_refused why=%d stop=%d ticks=%u", L.why,
             fe_np_stop_reason(), age);
      fe_host_gblink_request_end();
   }
}

/* A setup that did not become a link, as one of the UI's sentences
 * (psp/main_psp.c link_fail_text; the wording is ui_link_fail_text's). */
static int session_fail_code(const fe_gblink_progress *pr)
{
   int e = pr->error == FE_GBLINK_E_PEER_ABORT && pr->peer_reason >= 0
              ? pr->peer_reason : pr->error;
   if (L.why == LW_FULL)
      return UI_FAIL_ROOM_FULL;
   if (L.why == LW_WIFI)
      return UI_FAIL_NO_WIFI;
   if (L.why == LW_RADIO)
      return UI_FAIL_LINK_LOST;
   if (e == FE_GBLINK_E_PROTO)
      return UI_FAIL_VERSION;
   if (e == FE_GBLINK_E_ROM_MISSING || e == FE_GBLINK_E_ROM_BAD)
      return UI_FAIL_GAME;
   if (e == FE_GBLINK_E_MEMORY || e == FE_GBLINK_E_START)
      return UI_FAIL_MEMORY;
   if (L.why == LW_NOANSWER || e == FE_GBLINK_E_NO_PEER)
      /* A listed room that does not answer has closed since the list
       * showed it; an address or a cell may never have had one. */
      return L.conn == LINK_CONN_WIFI ? UI_FAIL_HOST_LEFT : UI_FAIL_NO_ANSWER;
   return UI_FAIL_HOST_LEFT;
}

/* The ad hoc joiner's second guess: the same cell, the other protocol. */
static void adhoc_flip(void)
{
   (void)fe_host_gblink_close();
   app_frame_forget();
   np_stop();
   net_vita_bulk_enable(0);
   L.kind = L.kind == LK_CABLE ? LK_ADAPTER : LK_CABLE;
   L.room_kind = L.kind == LK_CABLE ? ROOM_CABLE_GBA : ROOM_ADAPTER;
   L.why = LW_NONE;
   L.ended = 0;
   if (L.kind == LK_ADAPTER)
      cheats_off(1);
   after_transport();
}

/* The session is DONE or FAILED.  What the player sees next
 * (psp/main_psp.c link_end):
 *   - an agreed end: the game, "Link ended. Game saved.";
 *   - a host whose would-be partner never linked: it keeps its room (a
 *     toast says why), the game was only away for a moment;
 *   - a setup that did not link (a joiner): the failure's two lines until O;
 *   - a link lost after it ran: GBA, each player carries on and saves as
 *     usual; GB, both machines go back to where the link began.
 * Never before the partner has acknowledged what we sent (our FINAL or
 * ABORT), for at most ten seconds. */
static void link_end(int st, unsigned pad_new)
{
   fe_gblink_progress pr;
   int fail_screen, keep_room, desync, was_cable, why = L.why;

   fe_host_gblink_facts(&pr);
   desync = pr.error == FE_GBLINK_E_DESYNC ||
            (pr.error == FE_GBLINK_E_PEER_ABORT &&
             pr.peer_reason == FE_GBLINK_E_DESYNC);
   keep_room = st == FE_GBLINK_FAILED && !pr.ran && L.is_host &&
               why == LW_NONE && L.net_up;
   fail_screen = st == FE_GBLINK_FAILED && !pr.ran && !keep_room &&
                 why != LW_CANCEL && !L.is_host && !L.flip_now;
   if (!L.ended)
   {
      L.ended = 1;
      L.t_ended = L.ticks;
      fe_evt("link_ended state=%d ran=%d err=%s peer_reason=%d why=%d "
             "keep_room=%d fail_screen=%d", st, pr.ran,
             fe_gblink_error_text(pr.error), pr.peer_reason, why, keep_room,
             fail_screen);
   }
   if (!fe_host_gblink_flushed() && L.ticks - L.t_ended <= T_FLUSH)
      return;
   if (L.flip_now)
   {
      L.flip_now = 0;
      if (!pr.ran && L.net_up)
      {
         adhoc_flip();
         return;
      }
   }
   if (fail_screen)
   {
      if (!L.fail_hold)
      {
         /* The radio goes now (the host is free for the next console);
          * the screen stays until O. */
         L.fail_hold = 1;
         L.fail_code = session_fail_code(&pr);
         fe_evt("link_fail_screen code=%d", L.fail_code);
         net_down();
         return;
      }
      if (!(pad_new & CTL_CIRCLE))
         return;
   }
   was_cable = L.kind == LK_CABLE;
   if (!keep_room)
      net_down();
   if (pr.ran)
   {
      win_note(1);
      peek_evt("end");               /* the machine as the session leaves it */
   }
   if (fe_host_gblink_close() == -3)
   {
      net_down();
      link_off();
      app_quit_game("The game could not be reloaded after the link.");
      return;
   }
   app_frame_forget();
   app_audio_flush();
   app_pace_reset();
   L.ended = 0;
   L.fail_hold = 0;
   L.end_asked = 0;
   L.screen = 0;
   if (keep_room)
   {
      int e = pr.error == FE_GBLINK_E_PEER_ABORT && pr.peer_reason >= 0
                 ? pr.peer_reason : pr.error;
      cheats_off(0);
      L.clear = 1;
      L.why = LW_NONE;
      L.peer[0] = '\0';
      announce(1, 1);
      phase_set(PH_HOSTING);
      osd_toast("%s", e == FE_GBLINK_E_ROM_MISSING || e == FE_GBLINK_E_ROM_BAD
                ? "A Vita with a different game tried to join."
                : "The other Vita left before the link started.");
      fe_evt("link_host_keeps_room room=\"%s\"", L.room);
      return;
   }
   link_off();
   if (st == FE_GBLINK_DONE)
      osd_toast("Link ended. Game saved.");
   else if (why == LW_CANCEL)
      osd_toast("Link cancelled.");
   else if (fail_screen)
      ;                              /* its two lines were the answer */
   else if (!pr.ran)
      osd_toast(why == LW_WIFI ? "Wi-Fi was lost. The link did not start."
                               : "The link did not start.");
   else if (desync)
      osd_toast("The games fell out of step. Link ended, nothing saved.");
   else if (why == LW_WIFI)
      osd_toast(was_cable ? "Wi-Fi was lost. Link ended; your game carries on."
                          : "Wi-Fi was lost. Link ended, nothing saved.");
   else
      osd_toast(was_cable
                ? "Link lost: the other Vita is gone. Your game carries on."
                : "Link lost: the other Vita is gone. Nothing saved.");
}

/* ---- once per loop iteration --------------------------------------------------------------- */

/* The network left without being asked to: the Wi-Fi went, the ad hoc cell
 * dissolved, the driver could not keep its promise. */
static void link_lost(int why)
{
   fe_evt("link_lost why=%d phase=%d session=%d", why, L.phase,
          fe_host_gblink_state());
   if (fe_host_gblink_active())
   {
      /* The session sees its partner go and fails as "partner lost";
       * link_end tells the player which it really was. */
      if (L.why == LW_NONE)
         L.why = why;
      net_down();
      return;
   }
   {
      int hosting = L.is_host;
      net_down();
      link_off();
      if (why == LW_WIFI)
      {
         if (hosting)
            L.error = fail_line(UI_FAIL_NO_WIFI);
         osd_toast("Wi-Fi was lost. %s", hosting ? "The room is closed."
                                                  : "The link has ended.");
      }
      else if (why == LW_OVERLOAD)
         osd_toast("Link error: the connection could not keep up.");
      else
         osd_toast("The connection was lost. The link has ended.");
   }
}

/* Once a second: is the thing the transport stands on still there? */
static void radio_watch(void)
{
   int ok = 1;

   if (L.transport == TR_LAN)
   {
      net_vita_wifi w;
      ok = net_vita_wifi_status(&w);
      if (ok)
         L.signal_pct = w.rssi_pct;
   }
   else if (L.transport == TR_ADHOC)
      ok = vadhoc_transport_connected();
   else
      return;
   if (ok)
   {
      L.strikes = 0;
      return;
   }
   if (++L.strikes >= WATCH_STRIKES)
      link_lost(L.transport == TR_LAN ? LW_WIFI : LW_RADIO);
}

static void peer_name_take(void)
{
   const char *n = fe_np_peer_nick();
   if (L.transport == TR_ADHOC && (!n || !n[0]))
   {
      static vadhoc_peer vp[2];
      if (vadhoc_peers(vp, 2) > 0)
         n = vp[0].nick;
   }
   if (n && n[0])
      snprintf(L.peer, sizeof(L.peer), "%.23s", n);
}

/* The Wireless Adapter kind: netdrv carries the game's own protocol. */
static void adapter_service(unsigned pad_new)
{
   if (L.phase == PH_JOINING)
   {
      if (fe_np_session_active())
      {
         L.screen = 0;
         hold_game(0);
         phase_set(PH_LINKED);
         peer_name_take();
         osd_toast("Joined %s", L.room);
         return;
      }
      if (pad_new & CTL_CIRCLE)
      {
         fe_evt("link_cancel phase=joining");
         net_down();
         link_off();
         osd_toast("Link cancelled.");
         return;
      }
      if (fe_np_stop_reason() == ND_STOP_REFUSED)
         bringup_failed(UI_FAIL_ROOM_FULL);
      else if (L.can_flip && phase_age() > T_ADHOC_FLIP && L.flips < 3)
      {
         /* ad hoc, no list: nobody hosts by adapter; ask as a cable */
         L.flips++;
         fe_evt("link_adhoc_flip to=cable n=%d", L.flips);
         np_stop();
         cheats_off(0);
         L.kind = LK_CABLE;
         L.room_kind = ROOM_CABLE_GBA;
         after_transport();
      }
      else if (phase_age() > (L.conn == LINK_CONN_ADHOC ? T_ADHOC_ANSWER
                                                         : T_JOIN_ANSWER))
         bringup_failed(L.conn == LINK_CONN_WIFI ? UI_FAIL_HOST_LEFT
                                                 : UI_FAIL_NO_ANSWER);
      return;
   }
   if (L.is_host)
   {
      int n = fe_np_peer_count();
      if (n != L.peers)
      {
         int was = L.peers;
         L.peers = n;
         announce(1 + n, 1 + n < ND_MAX_CLIENTS);
         if (n > was)
         {
            peer_name_take();
            osd_toast("%s joined %s", L.peer[0] ? L.peer : "A player",
                      L.room);
         }
         else
            osd_toast("A player left %s", L.room);
      }
      phase_set(n > 0 ? PH_LINKED : PH_HOSTING);
   }
   else if (L.phase == PH_LINKED && !fe_np_session_active())
   {
      int r = fe_np_stop_reason();
      fe_evt("link_adapter_end stop=%d", r);
      net_down();
      link_off();
      osd_toast("%s", r == ND_STOP_HOST_BYE ? "The host closed the room."
                : "Link lost: the other Vita is gone.");
      return;
   }
   exit_assist_frame();
}

/* EVT gblink_win: one line every ten seconds while the machines run -- the
 * record of how the link felt (psp/main_psp.c gblink_win_note, docs/
 * GB-LINK.md section 14):
 *   frames/stalls/st_ep/st_max   display frames emulated / lost waiting for
 *                     the partner's buttons / runs of those / the longest
 *   absorbed/abs_ms   late buttons waited for and still on time (mean/max)
 *   core_us, step_us  both machines / the whole session step, mean/max
 *   delay, lead_min, lead_x10   the input delay; the partner's buttons in
 *                     hand beyond the frame needed, worst and mean x 10.  A
 *                     lead_min of 0 with stalls says the delay is too short
 *                     for this connection; a mean of several frames says it
 *                     could be shorter.
 *   nacks/resends     resend requests sent / answered
 *   work_us           the loop's estimate of a frame's work
 *   srtt_ms           netdrv's round trip */
static void win_note(int final)
{
   fe_gblink_window w;

   if (!final && L.ticks - L.t_win < 600u)
      return;
   L.t_win = L.ticks;
   if (!fe_host_gblink_window(&w) || !w.steps)
      return;
   fe_evt("gblink_win frames=%u stalls=%u st_ep=%u st_max=%u absorbed=%u "
          "abs_ms=%u.%u/%u.%u core_us=%u/%u step_us=%u/%u delay=%u "
          "lead_min=%d lead_x10=%u nacks=%u resends=%u work_us=%u srtt_ms=%u",
          w.frames, w.stalls, w.stall_episodes, w.streak_max, w.absorbed,
          w.absorbed ? w.absorb_us / w.absorbed / 1000u : 0u,
          w.absorbed ? w.absorb_us / w.absorbed % 1000u / 100u : 0u,
          w.absorb_us_max / 1000u, w.absorb_us_max % 1000u / 100u,
          w.frames ? w.adv_us / w.frames : 0u, w.adv_us_max,
          w.steps ? w.step_us / w.steps : 0u, w.step_us_max, w.delay,
          w.lead_n ? (int)w.lead_min : -1,
          w.lead_n ? w.lead_sum * 10u / w.lead_n : 0u, w.nacks, w.resends,
          (unsigned)L.work_ema, (unsigned)(fe_np_srtt_us() / 1000u));
}

/* The mirror kinds: a host waits, then the session (psp/main_psp.c
 * gblink_service). */
static void mirror_service(unsigned pad_new)
{
   int st;

   if (!fe_host_gblink_active())
   {
      int ready;
      if (L.phase != PH_HOSTING && L.phase != PH_MEASURE)
         return;
      ready = fe_np_gb_peer_ready(NULL);
      if (L.clear)
      {
         /* the console it just turned away must be gone first */
         if (!ready)
         {
            L.clear = 0;
            fe_evt("link_host_clear");
         }
         return;
      }
      if (!ready)
      {
         if (L.phase == PH_MEASURE)
            phase_set(PH_HOSTING);
         return;
      }
      if (L.phase == PH_HOSTING)
      {
         peer_name_take();
         announce(2, 0);
         fe_evt("link_host_peer room=\"%s\" peer=\"%s\"", L.room, L.peer);
         L.echoes = 0;
         if (L.transport == TR_LAN && g_pcfg.link_delay < 1)
         {
            lan_transport_echo_reset(L.lan);
            phase_set(PH_MEASURE);
         }
         else
         {
            session_begin();
            return;
         }
      }
      /* PH_MEASURE: an echo a frame, then their answers */
      if (L.echoes < MEASURE_ECHOES)
      {
         uint8_t mac[6];
         if (fe_np_gb_peer_mac(mac) == 0)
            lan_transport_echo(L.lan, mac);
         L.echoes++;
         return;
      }
      if (lan_transport_echo_result(L.lan, NULL, NULL, NULL) < MEASURE_ECHOES
          && phase_age() < MEASURE_FRAMES)
         return;
      session_begin();
      return;
   }

   st = fe_host_gblink_state();
   if (st == FE_GBLINK_RUNNING || st == FE_GBLINK_ENDING)
   {
      unsigned frames = fe_host_frame_count();
      if (L.screen)
      {
         /* the machines have started: the game's own picture and sound */
         L.screen = 0;
         app_audio_flush();
         app_pace_reset();
         if (!L.peer[0])
            peer_name_take();
         fe_evt("link_running player=%d peer=\"%s\"", L.is_host ? 1 : 2,
                L.peer);
         peek_evt("start");
         L.t_peek = L.t_win = L.ticks;
         {
            fe_gblink_window w;      /* the window starts with the machines */
            fe_host_gblink_window(&w);
         }
      }
      else if (L.ticks - L.t_peek >= 600u)
      {
         L.t_peek = L.ticks;
         peek_evt("run");
      }
      win_note(0);
      /* A partner that has stopped sending (its menu is open, its console
       * is busy, its radio is fading) freezes this game too: say what the
       * player is waiting for before he wonders. */
      if (frames == L.last_frames && !ui_active())
      {
         if (!L.stalled)
         {
            L.stalled = 1;
            L.t_stall = L.ticks;
         }
         else if (L.ticks - L.t_stall >= T_STALL_NOTE && !L.stall_told)
         {
            L.stall_told = 1;
            osd_toast("Waiting for the other Vita...");
            fe_evt("link_stall_note frames=%u", frames);
         }
      }
      else
      {
         L.stalled = 0;
         if (frames != L.last_frames)
            L.stall_told = 0;
      }
      L.last_frames = frames;
      return;
   }
   if (!L.screen)
   {
      L.screen = 1;
      app_audio_flush();             /* what was queued would replay stale */
   }
   if (st == FE_GBLINK_SETUP)
      setup_watch(pad_new);
   else
      link_end(st, pad_new);
}

/* The bring-up and the session, one step. */
static void link_service(unsigned pad_new)
{
   if (L.phase == PH_DIALOG)
   {
      /* Once per rendered frame while the system's screen is up. */
      int st = vadhoc_dialog_poll();
      if (st == VADHOC_DLG_RUNNING)
      {
         if (phase_age() > T_DIALOG && !L.dlg_abort)
         {
            L.dlg_abort = 2;
            vadhoc_dialog_abort();
         }
      }
      else
         adhoc_dialog_done(st);
      return;
   }
   if (L.phase == PH_FAILED)
   {
      /* a join that did not link: its reason, until O */
      L.screen = 1;
      if (pad_new & CTL_CIRCLE)
         link_off();
      return;
   }
   if (L.net_up)
   {
      fe_np_pump();
      if (fe_np_failed())
         link_lost(LW_OVERLOAD);
      else if (L.ticks - L.t_watch >= WATCH_EVERY)
      {
         L.t_watch = L.ticks;
         radio_watch();
      }
   }
   if (L.phase == PH_OFF && !fe_host_gblink_active())
      return;
   if (L.kind == LK_ADAPTER)
      adapter_service(pad_new);
   else
      mirror_service(pad_new);
}

void link_frame(void)
{
   unsigned pad = g_app_pad, pad_new;

   L.ticks = (unsigned)(link_now_us() * 3u / 50000u);
   /* The LINKING screen's footer is a touch target too. */
   if (L.screen_drew)
      pad |= ui_linking_tapped();
   L.screen_drew = 0;
   pad_new = pad & ~L.prev_pad;
   L.prev_pad = pad;
   /* While a menu is up the pad is the menu's: its O is "back", not this
    * file's "cancel the link". */
   if (ui_active() || ui_typing())
      pad_new = 0;

   igd_service();                    /* also the last room's goodbye */
   if (L.phase != PH_OFF || fe_host_gblink_active())
      link_service(pad_new);
   /* A game nobody can see does not get the pad. */
   if (L.screen)
      g_app_keys = 0;
   chip_update();
}

void link_idle_pump(void)
{
   /* The pacer is about to sleep until the refresh: whatever has arrived
    * meanwhile is delivered now rather than at the top of the next frame
    * (an acknowledgement goes out a refresh sooner). */
   if (L.net_up)
      fe_np_pump();
}

/* ---- the LINKING screen --------------------------------------------------------------------- */

/* "x.x / y.y MB" or "x / y KB". */
static void amount_text(char *out, size_t cap, uint32_t done, uint32_t total)
{
   if (total >= 1024u * 1024u)
   {
      unsigned d = (unsigned)(((uint64_t)done * 10u + 524288u) >> 20);
      unsigned t = (unsigned)(((uint64_t)total * 10u + 524288u) >> 20);
      snprintf(out, cap, "%u.%u / %u.%u MB", d / 10u, d % 10u, t / 10u,
               t % 10u);
   }
   else
      snprintf(out, cap, "%u / %u KB", (unsigned)((done + 512u) >> 10),
               (unsigned)((total + 512u) >> 10));
}

/* What is happening, the step and its amount, the bar: the same screen on
 * both consoles and for every kind (psp/main_psp.c link_draw_facts). */
static void linking_view(ui_linking_view *v, char *l1, size_t c1, char *l2,
                         size_t c2, char *l3, size_t c3, char *amt, size_t ca)
{
   fe_gblink_progress pr;
   const char *a, *b;
   int gb = !is_gba();

   memset(v, 0, sizeof(*v));
   l1[0] = l2[0] = l3[0] = amt[0] = '\0';
   v->room = L.room;
   v->frac = -1;
   v->busy = 1;
   v->l1 = l1;
   v->l2 = l2;

   if (L.phase == PH_DIALOG)
   {
      snprintf(l1, c1, "Connecting to the other Vita");
      snprintf(l2, c2, "Follow the Vita's connection screen.");
      v->footer = "";               /* the dialog has its own Cancel */
      return;
   }
   if (L.phase == PH_FAILED || L.fail_hold)
   {
      ui_link_fail_text(L.fail_code, &a, &b);
      snprintf(l1, c1, "%s", a);
      snprintf(l2, c2, "%s", b);
      v->fail = 1;
      v->busy = 0;
      return;
   }
   if (L.phase == PH_JOINING)
   {
      snprintf(l1, c1, "Joining %s by Wireless Adapter", L.room);
      snprintf(l2, c2, "Finding the room...");
      return;
   }

   fe_host_gblink_facts(&pr);
   snprintf(l1, c1, "Linking with %s", L.peer[0] ? L.peer : "the other Vita");
   switch (pr.phase)
   {
   case FE_GBLINK_PH_CHECK:
      snprintf(l2, c2, "Checking the game...");
      if (pr.total)
         v->frac = (int)((uint64_t)pr.done * 100u / pr.total);
      break;
   case FE_GBLINK_PH_WAIT_PEER:
      snprintf(l2, c2, L.is_host ? "Waiting for the other Vita..."
                                 : "Finding the room...");
      break;
   case FE_GBLINK_PH_HELLO:
      snprintf(l2, c2, "Checking the game...");
      v->frac = 100;
      break;
   case FE_GBLINK_PH_ROM_RX:
   case FE_GBLINK_PH_ROM_TX:
      snprintf(l2, c2, "%s %s...",
               pr.phase == FE_GBLINK_PH_ROM_RX ? "Receiving" : "Sending",
               pr.phase == FE_GBLINK_PH_ROM_RX
                  ? (pr.peer_title[0] ? pr.peer_title : "the game")
                  : (g_game.gb_title[0] ? g_game.gb_title : "your game"));
      amount_text(amt, ca, pr.done, pr.total);
      v->amount = amt;
      v->frac = pr.total ? (int)((uint64_t)pr.done * 1000u / pr.total) : 0;
      snprintf(l3, c3, "The game is kept in memory, not on the memory "
               "card.");
      v->l3 = l3;
      break;
   case FE_GBLINK_PH_VERIFY:
      snprintf(l2, c2, "Checking %s...",
               pr.peer_title[0] ? pr.peer_title : "the other game");
      v->frac = pr.total ? (int)((uint64_t)pr.done * 1000u / pr.total) : 0;
      break;
   case FE_GBLINK_PH_SHARE:
      snprintf(l2, c2, "%s", gb ? "Exchanging saves..."
                                : "Sharing where you are...");
      amount_text(amt, ca, pr.done, pr.total);
      v->amount = amt;
      v->frac = pr.total ? (int)((uint64_t)pr.done * 1000u / pr.total) : 0;
      if (!gb)
      {
         snprintf(l3, c3, "Both games carry on from where they are.");
         v->l3 = l3;
      }
      break;
   case FE_GBLINK_PH_START:
   case FE_GBLINK_PH_RUNNING:
      snprintf(l2, c2, "Starting together...");
      v->frac = 1000;
      break;
   case FE_GBLINK_PH_ENDING:
   case FE_GBLINK_PH_DONE:
      snprintf(l1, c1, "Ending the link...");
      snprintf(l2, c2, "%s", pr.phase == FE_GBLINK_PH_DONE
               ? "Link ended. Game saved."
               : "Both Vitas agree where to stop.");
      v->footer = "";
      break;
   default:
      /* failed, and not (yet) a failure screen: a cancel, a host whose
       * visitor left, the wait for the last acknowledgement */
      snprintf(l1, c1, "%s", L.why == LW_CANCEL ? "Link cancelled."
                                                 : "Ending the link...");
      v->footer = "";
      break;
   }
}

int link_draw(void)
{
   static char l1[96], l2[96], l3[96], amt[32];
   ui_linking_view v;

   /* Stall absorption's estimate of a frame's work: the loop's time so far
    * this iteration, the absorbed waiting taken out; 1/8 new. */
   {
      uint32_t work = (uint32_t)(link_now_us() - g_app_frame_t0);
      work = work > L.frame_wait_us ? work - L.frame_wait_us : 0;
      if (work > 50000)
         work = 50000;
      L.work_ema = L.work_ema - L.work_ema / 8u + work / 8u;
      L.frame_wait_us = 0;
      L.wait_prev = 0;
   }
   if (!L.screen)
      return 0;
   linking_view(&v, l1, sizeof(l1), l2, sizeof(l2), l3, sizeof(l3), amt,
                sizeof(amt));
   ui_linking_draw(&v);
   L.screen_drew = 1;
   return 1;
}

/* ---- what main_vita.c and the menus ask ------------------------------------------------------- */

int link_owns_picture(void)
{
   return L.screen;
}

int link_session_up(void)
{
   /* Fast forward and the pause overlay are off: a session's machines run
    * or are being set up, or the adapter is in a room. */
   return L.screen || fe_host_gblink_active() ||
          (L.kind == LK_ADAPTER && L.net_up);
}

int link_net_busy(void)
{
   return L.phase != PH_OFF || fe_host_gblink_active();
}

int link_locks_states(void)
{
   /* Not while a cable host only waits: the link starts from the live
    * game whenever the other console arrives. */
   return fe_host_gblink_active() || L.screen ||
          (L.kind == LK_ADAPTER && L.net_up);
}

/* The session chip and the menu's status line.  A cable session names its
 * mode, the room and, once linked, the player number (the game itself says
 * Player 1 / Player 2); nothing while the LINKING screen says it all
 * (psp/main_psp.c link_chip). */
static void chip_update(void)
{
   char s[sizeof(L.info)];
   int st = fe_host_gblink_active() ? fe_host_gblink_state() : -1;

   s[0] = '\0';
   if (L.screen || L.phase == PH_OFF || L.phase == PH_DIALOG)
      ;
   else if (L.kind == LK_ADAPTER)
      snprintf(s, sizeof(s), "Wireless Adapter  %s%s", L.room,
               L.phase == PH_HOSTING ? "  waiting" : "");
   else if (L.phase == PH_HOSTING || L.phase == PH_MEASURE)
      snprintf(s, sizeof(s), "Link Cable  %s  waiting", L.room);
   else if (st == FE_GBLINK_RUNNING || st == FE_GBLINK_ENDING)
      snprintf(s, sizeof(s), "Link Cable  %s  P%d", L.room,
               L.is_host ? 1 : 2);
   if (strcmp(s, L.info) != 0)
   {
      snprintf(L.info, sizeof(L.info), "%s", s);
      osd_session_chip_refresh();
   }
}

const char *link_session_info(void)
{
   return L.info[0] ? L.info : NULL;
}

void link_ui_view(ui_link_view *v)
{
   int st = fe_host_gblink_active() ? fe_host_gblink_state() : -1;

   v->conn = L.phase != PH_OFF ? L.conn : g_pcfg.net_conn;
   if (L.phase == PH_HOSTING || L.phase == PH_MEASURE)
      v->state = UI_LINK_HOSTING;
   else if (L.phase == PH_LINKED || L.phase == PH_JOINING ||
            fe_host_gblink_active())
      v->state = UI_LINK_LINKED;
   else
   {
      /* nothing up (or the ad hoc screen: the waiting page says "opening") */
      v->state = UI_LINK_OFF;
      v->error = L.error;
      return;
   }
   v->cable = L.kind != LK_ADAPTER;
   v->player = L.kind == LK_ADAPTER ? (L.is_host ? 1 : 2)
             : (st == FE_GBLINK_RUNNING || st == FE_GBLINK_ENDING)
                  ? (L.is_host ? 1 : 2) : 0;
   v->room = L.room;
   v->game = g_game.name;
   v->peer = L.peer;
   if (L.net_up)
   {
      uint32_t srtt = fe_np_srtt_us();
      v->rtt_ms = srtt ? (int)((srtt + 500u) / 1000u) : -1;
   }
   v->signal_pct = L.transport == TR_LAN ? L.signal_pct : -1;
}

/* ---- ui_host_hooks.h section 3: the network's state ------------------------------------------- */

void ui_host_net_status(ui_net_status *out)
{
   net_vita_wifi w;
   int use;

   if (!out)
      return;
   memset(out, 0, sizeof(*out));
   out->signal_pct = -1;
   use = net_vita_use();
   out->radio = use == NET_VITA_ADHOC ? UI_RADIO_ADHOC
              : use == NET_VITA_INFRA ? UI_RADIO_INFRA : UI_RADIO_FREE;
   if (!S.up)
      return;
   /* A few small system calls, twice a second while a page shows. */
   if (net_vita_wifi_status(&w))
   {
      out->wifi = UI_WIFI_CONNECTED;
      snprintf(out->ssid, sizeof(out->ssid), "%s", w.ssid);
      snprintf(out->ip, sizeof(out->ip), "%s", w.ip);
      out->signal_pct = w.rssi_pct;
   }
   else
      /* SceNetCtlState: 1 connecting, 2 waiting for an address */
      out->wifi = (w.state == 1 || w.state == 2) ? UI_WIFI_CONNECTING
                                                 : UI_WIFI_OFF;
   if (L.is_host && L.transport == TR_LAN && L.phase != PH_OFF)
   {
      snprintf(out->host_addr, sizeof(out->host_addr), "%s", L.host_addr);
      if (L.conn == LINK_CONN_INTERNET)
      {
         snprintf(out->public_addr, sizeof(out->public_addr), "%s",
                  L.pub_addr);
         out->public_state = L.pub_state;
      }
   }
}

/* ---- ui_host_hooks.h section 4: rooms ---------------------------------------------------------- */

void ui_host_rooms_end(void)
{
   if (R.scan)
   {
      lan_scan_close(R.scan);
      R.scan = NULL;
      fe_evt("link_scan close");
   }
   if (R.claimed)
   {
      net_vita_release(NET_VITA_INFRA);
      R.claimed = 0;
   }
}

int ui_host_rooms_begin(const char *addr)
{
   net_vita_wifi w;
   lan_scan_config sc;
   uint32_t ip = 0;
   uint16_t port = 0;

   ui_host_rooms_end();              /* begin while open replaces it */
   if (L.phase != PH_OFF || fe_host_gblink_active() ||
       net_vita_use() == NET_VITA_ADHOC)
      return UI_ROOMS_E_BUSY;
   if (addr && addr[0] &&
       lan_parse_addr(addr, &ip, &port,
                      (uint16_t)(S.port ? S.port : LAN_PORT_DEFAULT)) != 0)
      return UI_ROOMS_E_ADDRESS;
   if (!S.up || !net_vita_wifi_status(&w))
      return UI_ROOMS_E_NO_WIFI;
   if (net_vita_claim(NET_VITA_INFRA) != 0)
      return UI_ROOMS_E_BUSY;
   R.claimed = 1;
   memset(&sc, 0, sizeof(sc));
   sc.disc_port = (uint16_t)S.port;
   sc.local_ip_be = w.ip_be;
   sc.netmask_be = w.netmask_be;
   sc.flags = lan_flags(w.ip_be);
   /* Which word a room must match depends on its kind (see the top of this
    * file): decided per room in the poll, not by the scan. */
   sc.compat = S.compat[LAN_KIND_GBA_CABLE];
   sc.target_ip_be = ip;
   sc.target_port = port;
   R.scan = lan_scan_open(&sc);
   if (!R.scan)
   {
      fe_evt("link_error stage=scan_open why=\"%s\"", lan_last_error());
      ui_host_rooms_end();
      return UI_ROOMS_E_FAILED;
   }
   fe_evt("link_scan open target=%s", addr && addr[0] ? addr : "-");
   return 0;
}

int ui_host_rooms_poll(ui_room *out, int max)
{
   static lan_room rooms[UI_ROOMS_MAX];
   static int last_n = -1;
   int n, i;

   if (!R.scan)
      return UI_ROOMS_E_FAILED;
   if (max > UI_ROOMS_MAX)
      max = UI_ROOMS_MAX;
   lan_scan_poll(R.scan);            /* a probe round every 500 ms; drains */
   n = lan_scan_rooms(R.scan, rooms, max);
   {
      /* A Share game data room (share_vita.c) is not a link: it is joined
       * from Settings > Join a share, not from this list. */
      int k = 0;
      for (i = 0; i < n; i++)
         if (rooms[i].info.kind != LAN_KIND_SHARE)
            rooms[k++] = rooms[i];
      n = k;
   }
   for (i = 0; i < n; i++)
   {
      const lan_room *r = &rooms[i];
      ui_room *o = &out[i];
      int kind = r->info.kind;
      memset(o, 0, sizeof(*o));
      o->id = r->room_id;
      snprintf(o->title, sizeof(o->title), "%s", r->info.title);
      snprintf(o->nick, sizeof(o->nick), "%s", r->info.nick);
      snprintf(o->code, sizeof(o->code), "%.4s", r->info.code);
      o->number = r->info.number;
      o->kind = kind;
      o->players = r->info.players;
      o->max_players = r->info.max_players;
      o->open = (r->info.flags & LAN_ROOM_F_OPEN) != 0;
      o->compatible = kind >= 0 && kind <= LAN_KIND_GB_CABLE &&
                      r->info.compat == S.compat[kind];
      o->rtt_ms = r->rtt_ms;
      snprintf(o->addr, sizeof(o->addr), "%s", r->addr);
   }
   if (n != last_n)
   {
      last_n = n;
      fe_evt("link_scan rooms=%d", n);
      for (i = 0; i < n; i++)
         fe_evt("link_scan_room id=%08x addr=%s kind=%d number=%d "
                "players=%d/%d open=%d compatible=%d rtt_ms=%d title=\"%s\" "
                "nick=\"%s\"", out[i].id, out[i].addr, out[i].kind,
                out[i].number, out[i].players, out[i].max_players,
                out[i].open, out[i].compatible, out[i].rtt_ms, out[i].title,
                out[i].nick);
   }
   return n;
}
