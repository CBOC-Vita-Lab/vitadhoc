/* ui_host_test.c -- the Vita UI, run and photographed on Linux.
 *
 * Links the real UI (psp/ui_psp.c, osd_psp.c, config_psp.c, ctl_map.c, all
 * built -DVITA) with vita/video_vita.c on the software backend
 * (vita/vback_soft.c) and vita/pspcompat.c's POSIX body, points it at a fake
 * data root (make_fixture.py), feeds it scripted pad input and writes a PNG
 * of every screen at the Vita's native 960x544:
 *
 *     ui_host_test <data root> <output dir> [groups]
 *
 * `groups` names the test groups to run ("play cheats touch"; every group
 * when absent: text browser loading osd menu play cheats touch light wake
 * pictures), or is `measure` to print the width of each line of stdin.
 *
 * WHAT IS REAL: every pixel is produced by the code the Vita runs, through
 * the backend interface the Vita backend implements; the file I/O is the
 * UI's own (directory scan, art decode and cache, .thumb/.shot, config.ini,
 * favourites, link modes); input goes through the same edge/repeat filter.
 * WHAT IS NOT: the GXM backend itself (vback_vita2d.c cannot run here), the
 * emulator core (the "game" is a drawn test frame), the network and the
 * local link (the hooks of vita/ui_host_hooks.h are SIMULATED below: Wi-Fi
 * on and off, rooms that appear over time, a join that connects and one
 * that fails, a host waiting for player 2), the cheat engine (a fake list
 * of 300 entries behind vita/cheats_vita.h), the touch panel (scripted
 * finger positions through pspcompat_host_touch) and the system keyboard
 * (scripted through pspcompat_host_ime; a grey box stands where it would
 * be).
 *
 * A screen is captured from the vblank hook: every loop in ui_psp.c draws,
 * waits for vblank, then swaps, so at the wait the frame is complete and not
 * yet presented -- exactly when the PSP harness takes its GE dumps.
 */
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

#define PSPCOMPAT_IMPL            /* the declarations, not the name remapping */
#include "pspcompat.h"
#include "ui_psp.h"
#include "osd_psp.h"
#include "config_psp.h"
#include "video_vita.h"
#include "ui_host_hooks.h"
#include "state_slots.h"
#include "fe_evt.h"
#include "fe_host.h"
#include "font_8x16.h"
#include "cheats_vita.h"
#include "room_label.h"

static int g_fail;               /* expectations that did not hold */
#define EXPECT(cond, ...) \
   do { if (!(cond)) { g_fail++; printf("  EXPECT FAILED: " __VA_ARGS__); \
                       printf("\n"); } } while (0)

/* ---- what main_vita.c provides on the console (ui_host_hooks.h) --------- */

volatile int g_running = 1;

void osd_session_chip_refresh(void) { }

static int         g_mg_on;
int         mgift_ui_active(void) { return g_mg_on; }
const char *mgift_ui_line1(void)
{
   return g_mg_on ? "Listening for a Mystery Gift station" : "";
}
const char *mgift_ui_line2(void)
{
   return g_mg_on ? "192.168.43.17  -  nothing received yet" : "";
}
static int g_mg_have = 1;
int         ui_host_mgift_available(void) { return g_mg_have; }

/* ---- the simulated link layer (vita/ui_host_hooks.h sections 3..5) -------
 * What link_vita.c / local_vita.c will answer on a console, reduced to a
 * script: the tests set g_sim and the hooks play it back. */
typedef struct { int at; ui_room r; } sim_room;

static const sim_room SIM_WIFI[] = {
   { 25,  { 0x1001, "POKEMON EMER", "Ana",   "BPEE", 7,  1, 1, 2, 1, 1, 4,
            "192.168.1.31:19019" } },
   { 60,  { 0x1002, "POKEMON FIRE", "Ben",   "BPRE", 3,  1, 1, 2, 1, 1, 6,
            "192.168.1.40:19019" } },
   { 85,  { 0x1003, "POKEMON EMER", "Chris", "BPEE", 12, 1, 2, 2, 0, 1, 5,
            "192.168.1.52:19019" } },
   { 100, { 0x1004, "POKEMON EMER", "Dana",  "BPEE", 5,  1, 1, 2, 1, 0, 9,
            "192.168.1.60:19019" } },
   { 115, { 0x1005, "POKEMON EMER", "Eli",   "BPEE", 9,  0, 1, 4, 1, 1, -1,
            "192.168.1.61:19020" } },
   { 130, { 0x1006, "POKEMON RED",  "Flo",   "POKE", 2,  2, 1, 2, 1, 1, 7,
            "192.168.1.70:19019" } },
   { 145, { 0x1007, "MARIOKART",    "Maximilian-Otto", "AMKE", 21, 1, 1,
            2, 1, 1, 12, "192.168.1.71:19019" } },
};
static const sim_room SIM_INET[] = {
   { 45,  { 0x2001, "POKEMON EMER", "Ana",   "BPEE", 7,  1, 1, 2, 1, 1, 86,
            "203.0.113.9:19019" } },
};

static struct
{
   ui_net_status net;
   /* rooms */
   const sim_room *rooms;
   int  rooms_n, rooms_open, rooms_frames, rooms_err;
   char rooms_addr[24];
   int  begins, ends;
   /* the session, as ui_set_link is told every frame */
   ui_link_view lv;
   int  host_in;                /* frames until a Host action answers; 0 off */
   int  host_fail;              /* it answers with an error                  */
   /* the local link */
   ui_local_state local;
   int  local_in;
   int  local_fail;
   const char *err;
} g_sim;

static void sim_wifi(int state)
{
   memset(&g_sim.net, 0, sizeof(g_sim.net));
   g_sim.net.wifi = state;
   g_sim.net.signal_pct = -1;
   if (state == UI_WIFI_CONNECTED)
   {
      snprintf(g_sim.net.ssid, sizeof(g_sim.net.ssid), "HomeNet-5G");
      snprintf(g_sim.net.ip, sizeof(g_sim.net.ip), "192.168.1.23");
      g_sim.net.signal_pct = 82;
   }
}

static void sim_reset(void)
{
   memset(&g_sim, 0, sizeof(g_sim));
   sim_wifi(UI_WIFI_CONNECTED);
   g_sim.rooms = SIM_WIFI;
   g_sim.rooms_n = (int)(sizeof(SIM_WIFI) / sizeof(SIM_WIFI[0]));
   g_sim.local.available = 1;
   g_sim.lv.rtt_ms = g_sim.lv.signal_pct = -1;
}

void ui_host_net_status(ui_net_status *out) { *out = g_sim.net; }

int ui_host_rooms_begin(const char *addr)
{
   g_sim.begins++;
   if (g_sim.net.wifi != UI_WIFI_CONNECTED)
      return UI_ROOMS_E_NO_WIFI;
   if (g_sim.rooms_err)
      return g_sim.rooms_err;
   snprintf(g_sim.rooms_addr, sizeof(g_sim.rooms_addr), "%s", addr ? addr : "");
   g_sim.rooms_open = 1;
   g_sim.rooms_frames = 0;
   return 0;
}

int ui_host_rooms_poll(ui_room *out, int max)
{
   const sim_room *list = g_sim.rooms_addr[0] ? SIM_INET : g_sim.rooms;
   int n = g_sim.rooms_addr[0] ? (int)(sizeof(SIM_INET) / sizeof(SIM_INET[0]))
                               : g_sim.rooms_n;
   int i, k = 0;
   if (!g_sim.rooms_open)
      return UI_ROOMS_E_FAILED;
   if (g_sim.net.wifi != UI_WIFI_CONNECTED)
      return UI_ROOMS_E_NO_WIFI;
   g_sim.rooms_frames++;
   for (i = 0; i < n && k < max; i++)
      if (g_sim.rooms_frames >= list[i].at &&
          (!g_sim.rooms_addr[0] ||
           !strcmp(g_sim.rooms_addr, list[i].r.addr)))
         out[k++] = list[i].r;
   return k;
}

void ui_host_rooms_end(void)
{
   g_sim.ends++;
   g_sim.rooms_open = 0;
}

void ui_host_local_state(ui_local_state *out) { *out = g_sim.local; }

/* The partner's game: the other cartridges of the console (local_vita.c
 * lists roms/; here, the versions a player would have beside his own). */
static const char *const SIM_PARTNERS_GBA[] = {
   "Mario Kart - Super Circuit (USA)",
   "Pokemon - FireRed Version (USA)",
   "Pokemon - LeafGreen Version (USA)",
   "Pokemon - Ruby Version (USA)",
   "Pokemon - Sapphire Version (USA)",
   "The Legend of Zelda - Four Swords (USA)",
   "Wario Land 4 (USA, Europe)",
};
static const char *const SIM_PARTNERS_GB[] = {
   "Pokemon - Blue Version (USA, Europe)",
   "Pokemon - Yellow Version (USA, Europe)",
   "Tetris (World) (Rev 1)",
};
static const char *const *g_sim_partners = SIM_PARTNERS_GBA;
static int g_sim_partners_n = 7;

int ui_host_local_roms(void) { return g_sim_partners_n; }

const char *ui_host_local_rom_name(int i)
{
   return i >= 0 && i < g_sim_partners_n ? g_sim_partners[i] : "";
}

int ui_host_local_rom_pick(int i)
{
   if (g_sim.local.active)
      return -1;
   g_sim.local.partner_game = i >= 0 && i < g_sim_partners_n
                              ? g_sim_partners[i] : NULL;
   /* another game brings its own save */
   g_sim.local.partner_save = g_sim.local.partner_game != NULL;
   return 0;
}

/* ---- the fake cheat engine (vita/cheats_vita.h) ---------------------------
 * 300 database entries for the test game plus whatever the page adds: the
 * list's real size on a well-known game, with every kind of row in it. */
#define FAKE_CH_MAX 320
static struct
{
   char name[FAKE_CH_MAX][CHEATS_NAME_CAP];
   char code[FAKE_CH_MAX][96];
   cheat_entry e[FAKE_CH_MAX];
   int  n, suspended, added;
} g_fch;

static void fake_cheat_put(const char *name, const char *code, int on,
                           int kind, int usable, int builtin)
{
   int i = g_fch.n++;
   snprintf(g_fch.name[i], sizeof(g_fch.name[i]), "%s", name);
   snprintf(g_fch.code[i], sizeof(g_fch.code[i]), "%s", code);
   g_fch.e[i].name = g_fch.name[i];
   g_fch.e[i].code = g_fch.code[i];
   g_fch.e[i].enabled = on;
   g_fch.e[i].kind = kind;
   g_fch.e[i].usable = usable;
   g_fch.e[i].builtin = builtin;
}

static void fake_cheats_fill(int n)
{
   static const char *const first[] = {
      "Infinite Money", "Master Code (must be on)", "Walk Through Walls",
      "No Random Battles", "Infinite HP In Battle", "Infinite PP",
      "All Badges", "Max Stats For The First Pokemon In The Party After A "
      "Battle", "Rare Candy x99 In PC Slot 1", "Catch Trainer's Pokemon",
      "Shiny Wild Pokemon", "Fast Egg Hatch", "100% Catch Rate",
      "Infinite Safari Balls", "Unlock All Pokedex Entries",
   };
   int i;
   g_fch.n = g_fch.suspended = g_fch.added = 0;
   for (i = 0; i < n; i++)
   {
      char nm[CHEATS_NAME_CAP], cd[96];
      if (i < (int)(sizeof(first) / sizeof(first[0])))
         snprintf(nm, sizeof(nm), "%s", first[i]);
      else if (i < 215)
         snprintf(nm, sizeof(nm), "Wild Pokemon Modifier: %03d", i - 14);
      else
         snprintf(nm, sizeof(nm), "Item Modifier Slot 1: item %03d", i - 214);
      snprintf(cd, sizeof(cd), i % 7 == 3
               ? "%08X %04X+%08X %04X+%08X %04X" : "%08X %04X",
               0x82025838u + (unsigned)i * 4u, 0xE0FFu - (unsigned)i,
               0x82025840u + (unsigned)i * 4u, 0x03E7u,
               0x82025848u + (unsigned)i * 4u, 0x0001u);
      fake_cheat_put(nm, cd, 0, i % 11 == 6 ? CHEAT_KIND_GBA_AR3
                     : CHEAT_KIND_GBA_CODEBREAKER, i % 11 != 6, 1);
   }
}

int cheats_count(void) { return g_fch.n; }
const cheat_entry *cheats_get(int i)
{
   return i >= 0 && i < g_fch.n ? &g_fch.e[i] : NULL;
}
int cheats_enabled_count(void)
{
   int i, k = 0;
   for (i = 0; i < g_fch.n; i++)
      k += g_fch.e[i].enabled != 0;
   return k;
}
int cheats_set_enabled(int i, int on)
{
   if (i < 0 || i >= g_fch.n)
      return CHEATS_E_RANGE;
   if (!g_fch.e[i].usable)
      return CHEATS_E_UNSUPPORTED;
   g_fch.e[i].enabled = on ? 1 : 0;
   return CHEATS_OK;
}
static int hexrun(const char **p)
{
   int n = 0;
   while ((**p >= '0' && **p <= '9') || (**p >= 'A' && **p <= 'F') ||
          (**p >= 'a' && **p <= 'f'))
      (*p)++, n++;
   return n;
}
int cheats_validate(const char *code, fe_console_t console)
{
   const char *p = code;
   int kind = 0;
   for (;;)
   {
      int a, b, k;
      while (*p == ' ')
         p++;
      a = hexrun(&p);
      if (console == FE_CONSOLE_GBA)
      {
         if (a != 8 || *p != ' ')
            return CHEATS_E_FORMAT;
         p++;
         b = hexrun(&p);
         k = b == 4 ? CHEAT_KIND_GBA_CODEBREAKER
           : b == 8 ? CHEAT_KIND_GBA_GAMESHARK : 0;
      }
      else if (a == 8)
         k = CHEAT_KIND_GB_GAMESHARK;
      else if (a == 3 && *p == '-')
      {
         p++;
         if (hexrun(&p) != 3)
            return CHEATS_E_FORMAT;
         if (*p == '-')
         {
            p++;
            if (hexrun(&p) != 3)
               return CHEATS_E_FORMAT;
         }
         k = CHEAT_KIND_GB_GAMEGENIE;
      }
      else
         k = 0;
      if (!k || (kind && k != kind))
         return CHEATS_E_FORMAT;
      kind = k;
      while (*p == ' ')
         p++;
      if (!*p)
         return kind;
      if (*p != '+' && *p != '\n')
         return CHEATS_E_FORMAT;
      p++;
   }
}
int cheats_add(const char *name, const char *code)
{
   char nm[CHEATS_NAME_CAP];
   int kind = cheats_validate(code, FE_CONSOLE_GBA);
   if (g_fch.n >= FAKE_CH_MAX)
      return CHEATS_E_FULL;
   if (kind <= 0)
      return kind;
   if (name && name[0])
      snprintf(nm, sizeof(nm), "%s", name);
   else
      snprintf(nm, sizeof(nm), "Cheat %d", ++g_fch.added);
   fake_cheat_put(nm, code, 1, kind, 1, 0);
   return g_fch.n - 1;
}
int cheats_remove(int i)
{
   int k;
   if (i < 0 || i >= g_fch.n)
      return CHEATS_E_RANGE;
   for (k = i; k < g_fch.n - 1; k++)
   {
      memcpy(g_fch.name[k], g_fch.name[k + 1], sizeof(g_fch.name[k]));
      memcpy(g_fch.code[k], g_fch.code[k + 1], sizeof(g_fch.code[k]));
      g_fch.e[k] = g_fch.e[k + 1];
      g_fch.e[k].name = g_fch.name[k];
      g_fch.e[k].code = g_fch.code[k];
   }
   g_fch.n--;
   return CHEATS_OK;
}
int cheats_rename(int i, const char *name)
{
   if (i < 0 || i >= g_fch.n)
      return CHEATS_E_RANGE;
   snprintf(g_fch.name[i], sizeof(g_fch.name[i]), "%s", name);
   return CHEATS_OK;
}
int cheats_suspended(void) { return g_fch.suspended; }
static int g_fch_match;
int cheats_db_match(void) { return g_fch_match; }
const char *cheats_note(int i)
{
   if (i < 0 || i >= g_fch.n)
      return "";
   if (!g_fch.e[i].usable)
      return "Action Replay v3 codes are encrypted: this one cannot be run.";
   if (strstr(g_fch.name[i], "Master Code"))
      return "An enable code: others need it on.";
   return "";
}
const char *cheats_db_game(void)
{
   return g_fch.n >= 100 ? "Pokemon - Emerald Version (USA, Europe)" : "";
}
const char *cheats_error_text(int rc)
{
   switch (rc)
   {
   case CHEATS_E_FULL:        return "The cheat list is full.";
   case CHEATS_E_FORMAT:      return "That is not a code this console "
                                     "understands.";
   case CHEATS_E_UNSUPPORTED: return "This kind of code cannot be run.";
   case CHEATS_E_TOO_LONG:    return "The code is too long.";
   case CHEATS_E_IO:          return "The cheat file could not be written.";
   default:                   return "That did not work.";
   }
}
const char *cheats_kind_name(int kind)
{
   static const char *const n[] = { "unknown", "raw", "CodeBreaker",
      "GameShark", "Action Replay v3", "GameShark", "Game Genie" };
   return n[kind >= 0 && kind <= 6 ? kind : 0];
}
const char *cheats_format_hint(fe_console_t console)
{
   return console == FE_CONSOLE_GBA
      ? "8 digits, space, 4 or 8 digits"
      : "8 digits, or 3-3-3 with dashes (Game Genie)";
}

/* frontend-common/fe_host.c needs the core; the UI needs three names. */
static int g_gbpal;
int fe_host_gb_palette_count(void) { return 4; }
const char *fe_host_gb_palette_name(int id)
{
   static const char *const n[4] = { "Auto", "Grey", "DMG green", "Pocket" };
   return n[id >= 0 && id < 4 ? id : 0];
}
void fe_host_gb_palette_set(int id) { g_gbpal = id; }

/* ---- the scripted timeline ----------------------------------------------- */

#define TL_MAX 6000
typedef struct
{
   unsigned pad;
   char shot[64];
   unsigned char quit;
   unsigned char touch;          /* a finger is down, at tx,ty (LOGICAL px) */
   short tx, ty;
   unsigned char ev;             /* a SIM_EV_* that happens on this frame   */
} tl_frame;
enum { SIM_EV_NONE = 0, SIM_EV_WIFI_OFF, SIM_EV_WIFI_ON, SIM_EV_SWAP_FAIL };
   /* (SIM_EV_SWAP_FAIL: a ROM file appears in the folder being browsed) */
static void sim_event(int ev);
static tl_frame g_tl[TL_MAX];
static int g_t;            /* the script's cursor while it is being written */
static int g_frame;        /* the frame being played */
static int g_tl_end;
static char g_out[512];
static int g_shots;

static void tl_reset(void)
{
   memset(g_tl, 0, sizeof(g_tl));
   g_t = g_frame = g_tl_end = 0;
}

static void tl_wait(int n)
{
   g_t += n;
   if (g_t > g_tl_end) g_tl_end = g_t;
}

/* A press: down for two frames, then released for six -- one edge, and
 * never a repeat (the UI repeats a held direction after 18 frames). */
static void tl_press(unsigned b)
{
   g_tl[g_t].pad |= b;
   g_tl[g_t + 1].pad |= b;
   tl_wait(8);
}

static void tl_down(int n)
{
   while (n-- > 0)
      tl_press(PSP_CTRL_DOWN);
}

static void tl_up(int n)
{
   while (n-- > 0)
      tl_press(PSP_CTRL_UP);
}

static void tl_hold(unsigned b, int n)
{
   int i;
   for (i = 0; i < n; i++)
      g_tl[g_t + i].pad |= b;
   tl_wait(n + 6);
}

static void tl_shot(const char *name)
{
   snprintf(g_tl[g_t].shot, sizeof(g_tl[g_t].shot), "%s", name);
   tl_wait(1);
}

static void tl_quit(void)
{
   g_tl[g_t].quit = 1;
   tl_wait(1);
}

/* A finger down at x,y (logical 480x272) for n frames; the script's cursor
 * moves past them.  No pause after: chain these for a drag. */
static void tl_finger(int x, int y, int n)
{
   int i;
   for (i = 0; i < n && g_t + i < TL_MAX; i++)
   {
      g_tl[g_t + i].touch = 1;
      g_tl[g_t + i].tx = (short)x;
      g_tl[g_t + i].ty = (short)y;
   }
   tl_wait(n);
}

/* A tap on one hint of a footer: the footer's whole text as the page draws
 * it (centred), and the hint inside it.  The real words, so a hint that
 * moves is still hit and one that is gone fails the run. */
static void tl_tap(int x, int y);
static void tl_tap_x(int x0, const char *all, const char *hint)
{
   char pre[200];
   const char *at = strstr(all, hint);
   if (!at)
   {
      g_fail++;
      printf("  EXPECT FAILED: no hint \"%s\" in \"%s\"\n", hint, all);
      return;
   }
   snprintf(pre, sizeof(pre), "%.*s", (int)(at - all), all);
   tl_tap(x0 + vid_text_w(pre) + vid_text_w(hint) / 2, 262);
}
static void tl_tap_footer(const char *all, const char *hint)
{
   tl_tap_x((480 - vid_text_w(all)) / 2, all, hint);
}
/* The game list's footer is laid out from x = 20, 18 px between hints. */
static void tl_tap_list(const char *const *hints, int n, const char *hint)
{
   int i, x = 20;
   for (i = 0; i < n; i++)
   {
      if (!strcmp(hints[i], hint))
      {
         tl_tap(x + vid_text_w(hint) / 2, 262);
         return;
      }
      x += vid_text_w(hints[i]) + 18;
   }
   g_fail++;
   printf("  EXPECT FAILED: no list hint \"%s\"\n", hint);
}

/* A tap: down three frames, then up, and a pause. */
static void tl_tap(int x, int y)
{
   tl_finger(x, y, 3);
   tl_wait(6);
}

/* A drag from one point to another over n frames, the finger staying down
 * until the last; `lift_pause` frames follow with it up. */
static void tl_drag(int x0, int y0, int x1, int y1, int n, int lift_pause)
{
   int i;
   for (i = 0; i < n; i++)
      tl_finger(x0 + (x1 - x0) * i / (n - 1), y0 + (y1 - y0) * i / (n - 1), 1);
   tl_wait(lift_pause);
}

static int tl_touch(int *nx, int *ny)
{
   if (g_frame >= TL_MAX || !g_tl[g_frame].touch)
      return 0;
   *nx = g_tl[g_frame].tx * 2 + 1;
   *ny = g_tl[g_frame].ty * 2 + 1;
   return 1;
}

/* The system keyboard, scripted: `g_ime_reply` after g_ime_frames polls
 * (NULL = the player closes it without accepting). */
static const char *g_ime_reply = "";
static int g_ime_frames = 20, g_ime_up;
static const char *ime_script(const char *title, const char *initial,
                              int frames)
{
   (void)title; (void)initial;
   g_ime_up = frames < g_ime_frames;
   if (g_ime_up)
      return NULL;
   return g_ime_reply ? g_ime_reply : PSPCOMPAT_HOST_IME_CANCEL;
}

static unsigned tl_pad(void)
{
   return g_frame < TL_MAX ? g_tl[g_frame].pad : 0;
}

/* What a still picture cannot show: where the finger is (a ring), and that
 * the system's keyboard is up (it draws itself on the console; here a
 * labelled box stands in for it). */
static void shot_marks(void)
{
   int f = g_frame < TL_MAX ? g_frame : TL_MAX - 1;
   if (!g_tl[f].touch && !(g_ime_up && ui_typing()))
      return;
   vid_overlay_begin(0);
   if (g_ime_up && ui_typing())
   {
      vid_rect(0, 150, 480, 122, 0x2104, 245);
      vid_rect(0, 150, 480, 1, 0xFFFF, 255);
      vid_text_center(196, "[ the Vita's system keyboard is shown here ]",
                      0xFFFF);
      vid_text_center(216, "sceImeDialog, composited by the system over "
                      "this frame", 0x8C51);
   }
   if (g_tl[f].touch)
   {
      int x = g_tl[f].tx, y = g_tl[f].ty, i;
      static const signed char ring[12][2] = {
         {0,-9},{5,-8},{8,-5},{9,0},{8,5},{5,8},{0,9},{-5,8},{-8,5},{-9,0},
         {-8,-5},{-5,-8} };
      vid_rect(x - 2, y - 2, 4, 4, 0xFFE0, 255);
      for (i = 0; i < 12; i++)
         vid_rect(x + ring[i][0] - 1, y + ring[i][1] - 1, 3, 3, 0xFFE0, 255);
   }
   vid_overlay_end();
}

static void shot_now(const char *name)
{
   char path[700];
   shot_marks();
   snprintf(path, sizeof(path), "%s/%s.png", g_out, name);
   if (vid_dump_ge(path) == 0)
      g_shots++;
   else
      fprintf(stderr, "shot FAILED: %s\n", path);
}

static void tl_event(int ev)
{
   g_tl[g_t].ev = (unsigned char)ev;
}

static void tl_vblank(void)
{
   if (!g_tl_end)            /* no script: a test drawing frames itself */
      return;
   if (g_frame < TL_MAX)
   {
      if (g_tl[g_frame].shot[0])
         shot_now(g_tl[g_frame].shot);
      if (g_tl[g_frame].quit)
         g_running = 0;
      if (g_tl[g_frame].ev)
         sim_event(g_tl[g_frame].ev);
   }
   g_frame++;
   /* A script that forgot to quit must not hang the test. */
   if (g_frame > g_tl_end + 900)
   {
      fprintf(stderr, "timeline overran at frame %d\n", g_frame);
      g_running = 0;
   }
}

/* ---- drawn "game" frames -------------------------------------------------- */

#define RGB(r, g, b) \
   ((uint16_t)((((r) >> 3) << 11) | (((g) >> 2) << 5) | ((b) >> 3)))

static void put(uint16_t *f, int w, int h, int x, int y, uint16_t c)
{
   if (x >= 0 && y >= 0 && x < w && y < h)
      f[y * w + x] = c;
}

static void fill(uint16_t *f, int w, int h, int x0, int y0, int x1, int y1,
                 uint16_t c)
{
   int x, y;
   for (y = y0; y < y1; y++)
      for (x = x0; x < x1; x++)
         put(f, w, h, x, y, c);
}

/* The classic 8x16 VGA face (psp/font_8x16.c): game-like 1-pixel text. */
static void text8(uint16_t *f, int w, int h, int x, int y, const char *s,
                  uint16_t c)
{
   for (; *s; s++, x += 8)
   {
      int r, b;
      for (r = 0; r < 16; r++)
      {
         unsigned char bits = fe_font8x16[(unsigned char)*s * 16 + r];
         for (b = 0; b < 8; b++)
            if (bits & (0x80 >> b))
               put(f, w, h, x + b, y + r, c);
      }
   }
}

/* THE TEST PATTERN: everything a scaler can get wrong, in one frame.
 * 1-pixel checkerboards and line gratings (shimmer, uneven columns), colour
 * bars (channel order), a 1-pixel white frame and corner marks (cropping,
 * edge bleed), diagonals (stair-stepping) and 8x16 text. */
static void pattern(uint16_t *f, int w, int h, const char *label)
{
   static const uint16_t bars[8] = {
      RGB(255, 255, 255), RGB(255, 255, 0), RGB(0, 255, 255), RGB(0, 255, 0),
      RGB(255, 0, 255), RGB(255, 0, 0), RGB(0, 0, 255), RGB(0, 0, 0)
   };
   int x, y, i;
   fill(f, w, h, 0, 0, w, h, RGB(56, 64, 88));
   for (y = 4; y < 40; y++)
      for (x = 4; x < w - 4; x++)
      {
         int zone = (x - 4) / ((w - 8) / 4), on;
         switch (zone)
         {
         case 0:  on = (x + y) & 1; break;            /* 1 px checker    */
         case 1:  on = x & 1; break;                  /* vertical lines  */
         case 2:  on = y & 1; break;                  /* horizontal      */
         default: on = ((x >> 1) + (y >> 1)) & 1; break;   /* 2 px checker */
         }
         f[y * w + x] = on ? RGB(255, 255, 255) : RGB(0, 0, 0);
      }
   for (i = 0; i < 8; i++)
      fill(f, w, h, 4 + i * (w - 8) / 8, 44, 4 + (i + 1) * (w - 8) / 8, 60,
           bars[i]);
   for (x = 4; x < w - 4; x++)        /* a grey ramp: banding, order      */
   {
      int v = (x - 4) * 255 / (w - 9);
      fill(f, w, h, x, 62, x + 1, 70, RGB(v, v, v));
   }
   text8(f, w, h, 6, 74, label, RGB(255, 255, 255));
   text8(f, w, h, 6, 92, w >= 240 ? "The quick brown fox, 0123456"
                                  : "Quick fox 0123456", RGB(255, 220, 96));
   for (i = 0; i < 40; i++)           /* diagonals                        */
   {
      put(f, w, h, 8 + i, 112 + i * 3 / 4, RGB(255, 255, 255));
      put(f, w, h, 60 + i, 112 + 30 - i * 3 / 4, RGB(96, 255, 160));
   }
   for (i = 0; i < 360; i++)          /* a circle, coarse                 */
   {
      float a = (float)i * 3.14159265f / 180.0f;
      float sx = (float)(w - 40) + 14.0f * cosf(a);
      float sy = 128.0f + 14.0f * sinf(a);
      put(f, w, h, (int)(sx + 0.5f), (int)(sy + 0.5f), RGB(255, 96, 96));
   }
   for (x = 0; x < w; x++)            /* the 1 px frame                   */
   {
      f[x] = RGB(255, 255, 255);
      f[(h - 1) * w + x] = RGB(255, 255, 255);
   }
   for (y = 0; y < h; y++)
   {
      f[y * w] = RGB(255, 255, 255);
      f[y * w + w - 1] = RGB(255, 255, 255);
   }
   fill(f, w, h, 0, 0, 3, 3, RGB(255, 0, 0));
   fill(f, w, h, w - 3, 0, w, 3, RGB(0, 255, 0));
   fill(f, w, h, 0, h - 3, 3, h, RGB(0, 0, 255));
   fill(f, w, h, w - 3, h - 3, w, h, RGB(255, 255, 0));
}

/* A made-up overworld, so the menu has a picture to sit on. */
static void scene(uint16_t *f, int w, int h, int variant)
{
   int x, y, i;
   for (y = 0; y < h; y++)
      for (x = 0; x < w; x++)
      {
         int tile = ((x / 16) + (y / 16)) & 1;
         f[y * w + x] = y < h * 5 / 8
            ? RGB(96 + y / 2, 168 + y / 4, 232)
            : (tile ? RGB(88, 168, 80) : RGB(104, 184, 88));
      }
   for (i = 0; i < 7; i++)
   {
      int bx = (i * 37 + variant * 23) % (w - 24), by = h * 5 / 8 - 22 - (i % 3) * 6;
      fill(f, w, h, bx, by, bx + 20, h * 5 / 8, RGB(168, 104, 64));
      fill(f, w, h, bx - 3, by - 8, bx + 23, by, RGB(184, 56, 56));
      fill(f, w, h, bx + 7, by + 10, bx + 13, h * 5 / 8, RGB(72, 48, 32));
   }
   fill(f, w, h, w / 2 - 5, h * 5 / 8 + 14, w / 2 + 5, h * 5 / 8 + 28,
        RGB(232, 64, 64));
   fill(f, w, h, w / 2 - 4, h * 5 / 8 + 8, w / 2 + 4, h * 5 / 8 + 15,
        RGB(248, 208, 168));
   fill(f, w, h, 6, h - 40, w - 6, h - 6, RGB(248, 248, 248));
   fill(f, w, h, 8, h - 38, w - 8, h - 8, RGB(40, 56, 104));
   text8(f, w, h, 14, h - 36, w >= 240 ? "A test frame, not a game." : "A test frame.",
         RGB(255, 255, 255));
   text8(f, w, h, 14, h - 22, w >= 240 ? "Every pixel here is 1:1." : "1:1 pixels.",
         RGB(200, 216, 255));
}

/* ---- helpers -------------------------------------------------------------- */

static char g_root[256], g_roms[300];
static uint16_t g_game[240 * 160];          /* the "core's" frame, RGB565  */
static uint16_t g_game_b[240 * 160];
static uint16_t g_snap[256 * 256];          /* PSP-order snapshot          */
static uint16_t g_thumbs[5 * 64 * 64];
static int g_gw = 240, g_gh = 160;

static void game_frame_take(int console)
{
   g_gw = console == FE_CONSOLE_GBA ? 240 : 160;
   g_gh = console == FE_CONSOLE_GBA ? 160 : 144;
   scene(g_game, g_gw, g_gh, 1);
   memset(g_snap, 0, sizeof(g_snap));
   vid_frame_to_psp565(g_snap, 256, g_game, (unsigned)g_gw, (unsigned)g_gh,
                       (size_t)g_gw * 2);
}

/* What a main loop does on UI_ACT_SAVESTATE: the state, then its previews
 * from the snapshot (psp/state_slots.h formats, PSP-order 565). */
static int fake_save(const char *slot1, unsigned slot)
{
   char st[PSP_FILE_PATH_CAP], th[PSP_STATE_THUMB_PATH_CAP];
   char sh[PSP_STATE_SHOT_PATH_CAP];
   unsigned char hdr[16];
   FILE *f;
   int x, y;
   if (psp_state_path_for_slot(st, sizeof(st), slot1, slot) != 0 ||
       psp_state_thumb_path(th, sizeof(th), st) != 0 ||
       psp_state_shot_path(sh, sizeof(sh), st) != 0)
      return -1;
   f = fopen(st, "wb");
   if (!f)
      return -1;
   fwrite("GBADHOC-TEST-STATE", 1, 18, f);
   fclose(f);
   f = fopen(th, "wb");
   if (f)
   {
      memset(hdr, 0, 16);
      memcpy(hdr, PSP_STATE_THUMB_MAGIC, 4);
      hdr[4] = 64; hdr[6] = 42; hdr[8] = 64;
      fwrite(hdr, 1, 16, f);
      for (y = 0; y < 42; y++)
         for (x = 0; x < 64; x++)
         {
            uint16_t v = g_snap[(y * g_gh / 42) * 256 + x * g_gw / 64];
            fwrite(&v, 2, 1, f);
         }
      fclose(f);
   }
   f = fopen(sh, "wb");
   if (f)
   {
      psp_state_shot_header(hdr, (unsigned)g_gw, (unsigned)g_gh);
      fwrite(hdr, 1, 16, f);
      fwrite(g_snap, 2, (size_t)256 * g_gh, f);
      fclose(f);
   }
   return 0;
}

static char g_state_base[PSP_FILE_PATH_CAP];

/* What a script did not expect to be asked. */
static int g_acts[32];
static ui_join_info g_joined;

static char g_new_rom[400];      /* SIM_EV_SWAP_FAIL: this file appears */
static void sim_event(int ev)
{
   if (ev == SIM_EV_WIFI_OFF)
      sim_wifi(UI_WIFI_OFF);
   else if (ev == SIM_EV_WIFI_ON)
      sim_wifi(UI_WIFI_CONNECTED);
   else if (ev == SIM_EV_SWAP_FAIL && g_new_rom[0])
   {
      FILE *fp = fopen(g_new_rom, "wb");
      if (fp) { fputs("x", fp); fclose(fp); }
   }
}

/* One frame of the pretend link layer: a Host that was asked for answers
 * after a moment (the room opens, or it fails); with Connection: Internet
 * the router is asked and answers a little later. */
static int g_sim_pub_in, g_sim_pub_result = UI_PUBLIC_OPEN;
static void sim_frame(void)
{
   static char room[ROOM_LABEL_CAP];
   if (g_sim.host_in && --g_sim.host_in == 0)
   {
      if (g_sim.host_fail)
         ui_link_fail_text(g_sim.host_fail, &g_sim.err, NULL);
      else
      {
         g_sim.lv.state = UI_LINK_HOSTING;
         g_sim.lv.cable = ui_host_cable();
         g_sim.lv.player = 1;
         g_sim.lv.room = room_label(ui_group(), room, sizeof(room));
         g_sim.lv.game = "Pokemon Emerald";
         g_sim.lv.conn = g_pcfg.net_conn;
         if (g_pcfg.net_conn != UI_CONN_ADHOC)
            snprintf(g_sim.net.host_addr, sizeof(g_sim.net.host_addr),
                     "%s:19019", g_sim.net.ip);
         if (g_pcfg.net_conn == UI_CONN_INTERNET)
         {
            g_sim.net.public_state = UI_PUBLIC_ASKING;
            g_sim_pub_in = 40;
         }
      }
   }
   if (g_sim_pub_in && --g_sim_pub_in == 0)
   {
      g_sim.net.public_state = g_sim_pub_result;
      snprintf(g_sim.net.public_addr, sizeof(g_sim.net.public_addr),
               "203.0.113.9:19019");
   }
   if (g_sim.local_in && --g_sim.local_in == 0)
   {
      if (g_sim.local_fail)
         ui_link_fail_text(UI_FAIL_LOCAL, &g_sim.err, NULL);
      else
         g_sim.local.active = 1;
   }
   g_sim.lv.error = g_sim.err;
   ui_set_link(&g_sim.lv);
}

/* The in-game menu loop, as a main loop runs it. */
static void run_menu(int session, const char *info)
{
   memset(g_acts, 0, sizeof(g_acts));
   while (g_frame <= g_tl_end && ui_active() && g_running)
   {
      ui_action act;
      sim_frame();
      act = ui_frame(tl_pad(), session, info);
      if ((int)act >= 0 && (int)act < 32)
         g_acts[act]++;
      switch (act)
      {
      case UI_ACT_SAVESTATE:
         ui_state_save_done(fake_save(g_state_base, (unsigned)ui_state_slot()));
         break;
      case UI_ACT_LOADSTATE:
         if (!ui_state_load_done(0))
            osd_toast("State %d loaded", ui_state_slot());
         break;
      case UI_ACT_NET_MGIFT:      g_mg_on = 1; break;
      case UI_ACT_NET_MGIFT_STOP: g_mg_on = 0; break;
      case UI_ACT_NET_HOST:
         g_sim.err = NULL;
         g_sim.host_in = 20;
         break;
      case UI_ACT_NET_JOIN:
         g_joined = *ui_join_target();
         break;
      case UI_ACT_NET_DISCONNECT:
         osd_toast(g_sim.lv.state == UI_LINK_HOSTING ? "Stopped hosting %s"
                   : "Link ended. Game saved.", g_sim.lv.room);
         memset(&g_sim.lv, 0, sizeof(g_sim.lv));
         g_sim.lv.rtt_ms = g_sim.lv.signal_pct = -1;
         g_sim.host_in = 0;
         g_sim.net.host_addr[0] = g_sim.net.public_addr[0] = '\0';
         g_sim.net.public_state = UI_PUBLIC_UNKNOWN;
         session = 0;
         break;
      case UI_ACT_NET_LOCAL:
         g_sim.err = NULL;
         g_sim.local_in = 12;
         break;
      case UI_ACT_NET_LOCAL_STOP:
         g_sim.local.active = 0;
         osd_toast("The second game has stopped. Its save is kept.");
         break;
      case UI_ACT_NET_LOCAL_SWAP:
         g_sim.local.controlled ^= 1;
         break;
      default:
         break;
      }
      osd_draw();
      pspcompat_wait_vblank();
      vid_swap();
   }
   if (ui_active())
      ui_close();
   g_running = 1;
}

/* The LINKING screen after a Join, as link_vita.c will draw it: looking,
 * then matched and starting -- or the failure, in ui_link_fail_text's
 * words. */
static void sim_linking(const char *tag, int fail_why)
{
   ui_linking_view v;
   char room[ROOM_LABEL_CAP + 8], l1[96], n[64];
   const char *a, *b;
   ui_set_backdrop(g_snap, g_gw, g_gh);
   memset(&v, 0, sizeof(v));
   snprintf(room, sizeof(room), "Room %d", g_joined.number);
   v.game = "Pokemon Emerald";
   v.room = room;
   snprintf(l1, sizeof(l1), g_joined.listed ? "Joining %s's game"
            : "Waiting for the connection screen", g_joined.nick);
   v.l1 = l1;
   v.l2 = g_joined.listed ? g_joined.addr : "";
   v.frac = -1; v.busy = 1;
   ui_linking_draw(&v);
   snprintf(n, sizeof(n), "35_linking_%s_joining", tag);
   shot_now(n);
   vid_swap();
   if (fail_why >= 0)
   {
      ui_link_fail_text(fail_why, &a, &b);
      memset(&v, 0, sizeof(v));
      v.game = "Pokemon Emerald"; v.room = room;
      v.l1 = a; v.l2 = b; v.frac = -1; v.fail = 1;
      ui_linking_draw(&v);
      snprintf(n, sizeof(n), "35_linking_%s_failed", tag);
      shot_now(n);
      vid_swap();
      return;
   }
   v.l1 = "Linked. Matching the two games";
   v.l2 = "Sharing where you are";
   v.amount = "84 / 128 KiB";
   v.l3 = "Both games carry on from where they are.";
   v.frac = 656;
   ui_linking_draw(&v);
   snprintf(n, sizeof(n), "35_linking_%s_progress", tag);
   shot_now(n);
   vid_swap();
}

static void menu_open(void)
{
   g_frame = 0;
   ui_set_backdrop(g_snap, g_gw, g_gh);
   ui_set_thumb_buffer(g_thumbs);
   ui_set_state_base(g_state_base);
   ui_open();
   tl_reset();
   tl_wait(14);                      /* the opening transition */
}

static void game_begin(const char *rom, const char *code, int console,
                       int cable_ready)
{
   char path[PSP_FILE_PATH_CAP];
   g_pcfg.console = console;
   pcfg_display_select(console);
   vid_set_mode(g_pcfg.scale, g_pcfg.filter);
   vid_ambient_mode(g_pcfg.ambient);
   snprintf(path, sizeof(path), "%s/%s", g_roms, rom);
   psp_rom_path_suffix(g_state_base, sizeof(g_state_base), path,
                       psp_state_slot1_suffix((fe_console_t)console));
   snprintf(g_pcfg.last_rom, sizeof(g_pcfg.last_rom), "%s", rom);
   ui_linkmode_load(g_roms);
   ui_set_state_base(g_state_base);
   ui_set_game(console == FE_CONSOLE_GBA ? code : NULL,
               console == FE_CONSOLE_GBA ? NULL : code, rom, cable_ready);
   ui_set_link(NULL);
   sim_reset();
   /* Two machines of any of the three consoles run on one Vita. */
   g_sim.local.available = 1;
   g_sim_partners = console == FE_CONSOLE_GBA ? SIM_PARTNERS_GBA
                                              : SIM_PARTNERS_GB;
   g_sim_partners_n = console == FE_CONSOLE_GBA ? 7 : 3;
   game_frame_take(console);
}

/* Every browser run starts from the fixture's favourites, whatever the
 * previous run starred. */
static void favourites_reset(void)
{
   char path[400];
   FILE *f;
   snprintf(path, sizeof(path), "%s/favourites.txt", g_roms);
   f = fopen(path, "w");
   if (f)
   {
      fputs("Pokemon - Emerald Version (USA, Europe).gba\n"
            "Golden Sun (USA, Europe).gba\n"
            "Tetris (World) (Rev 1).gb\n", f);
      fclose(f);
   }
}

static void browser_run(void)
{
   char out[PSP_FILE_PATH_CAP];
   int slot = 0, rc;
   fe_console_t con = FE_CONSOLE_GBA;
   favourites_reset();
   g_running = 1;
   g_frame = 0;
   rc = ui_browser(g_roms, out, sizeof(out), &slot, &con);
   printf("  ui_browser rc=%d frames=%d%s%s\n", rc, g_frame,
          rc == 0 ? " picked=" : "", rc == 0 ? out : "");
   g_running = 1;
}

/* The game picture, as the main loop presents it: blit, OSD, swap. */
static void picture_shot(const char *name, const uint16_t *f, int w, int h)
{
   vid_draw_frame(f, (unsigned)w, (unsigned)h, (size_t)w * 2);
   osd_draw();
   shot_now(name);
   pspcompat_wait_vblank();
   vid_swap();
}

/* ---- the scenarios -------------------------------------------------------- */

static void test_text(void)
{
   static const char *const probe[] = {
      VID_GLYPH_X " play", "L/R page", "Left: states", VID_GLYPH_SQ " unstar",
      "START settings", "Pokemon - Emerald Version", "Wireless Adapter",
      "The quick brown fox jumps over the lazy dog 0123456789",
      VID_GLYPH_X " rebind   " VID_GLYPH_SQ " none   " VID_GLYPH_TRI
      " default   " VID_GLYPH_O " back      blank = disabled",
   };
   unsigned i;
   int ch;
   float mean, w;
   w = vid_text_advance_dev(0, &ch, &mean);
   printf("text ui (Inter Regular, 30 px native): natural vs used advance, "
          "worst %+.2f native px ('%c'), mean |dev| %.3f native px\n",
          w, ch, mean);
   w = vid_text_advance_dev(1, &ch, &mean);
   printf("text hd (Inter SemiBold, 38 px native): natural vs used advance, "
          "worst %+.2f native px ('%c'), mean |dev| %.3f native px\n",
          w, ch, mean);
   printf("vid_text_w == the PSP's baked advances (logical px), by "
          "construction; samples:\n");
   for (i = 0; i < sizeof(probe) / sizeof(probe[0]); i++)
      printf("  %4d  hd %4d  \"%s\"\n", vid_text_w(probe[i]),
             vid_text_hd_w(probe[i]), probe[i]);
   printf("vid_band_y(100,24)=%d vid_text_y_in(100,24)=%d (PSP: 98, 102)\n",
          vid_band_y(100, 24), vid_text_y_in(100, 24));

   /* A type specimen: both faces, the glyphs, the wordmark, on both pages. */
   {
      int t;
      for (t = 0; t < 2; t++)
      {
         uint16_t bg = t ? 0xF79E : 0x0000, fg = t ? 0x0000 : 0xFFFF;
         uint16_t dim = t ? 0x8C71 : 0x6B4D;
         vid_overlay_begin(1);
         vid_rect(0, 0, 480, 272, bg, 255);
         vid_logo(20, 12, fg, 255);
         vid_text_hd(20 + VID_LOGO_W + 14, 11, "TYPE SPECIMEN", fg);
         vid_text(20, 48, "Inter Regular, the PSP's 15 px face at 30 px: "
                  "AaBbCcGgQqRr 0123456789", fg);
         vid_text(20, 66, "The quick brown fox jumps over the lazy dog. "
                  "il1I| O0o rn m WAVE Ty", fg);
         vid_text_hd(20, 88, "Inter SemiBold 19 -> 38 px: Emerald Version",
                     fg);
         vid_text_hd(20, 114, "ABCDEFGHIJKLMNOPQRSTUVWXYZ 0123456789", fg);
         vid_text(20, 146, VID_GLYPH_TRI "  " VID_GLYPH_O "  " VID_GLYPH_X
                  "  " VID_GLYPH_SQ "  " VID_GLYPH_STAR "  " VID_GLYPH_STAR_O
                  "    " VID_GLYPH_X " play   " VID_GLYPH_O " back   "
                  VID_GLYPH_SQ " star   " VID_GLYPH_TRI " console", fg);
         vid_text_hd(20, 168, VID_GLYPH_TRI " " VID_GLYPH_O " " VID_GLYPH_X
                     " " VID_GLYPH_SQ " " VID_GLYPH_STAR " "
                     VID_GLYPH_STAR_O, fg);
         vid_text(20, 200, "dim text: #6B6B6B on the page, alpha text below",
                  dim);
         vid_text_a(20, 218, "vid_text_a at alpha 128: Returning to game "
                    "list...", fg, 128);
         /* A 1-logical-pixel ruler: rects must land on whole 2x2 blocks. */
         {
            int i2;
            for (i2 = 0; i2 < 60; i2++)
               vid_rect(20 + i2 * 2, 244, 1, i2 % 5 ? 4 : 8, fg, 255);
            vid_gradient(160, 244, 140, 8, fg, bg, 255);
            vid_gradient_a(320, 244, 140, 8, fg, 0, 255);
         }
         vid_overlay_end();
         shot_now(t ? "00_type_light" : "00_type_dark");
         vid_swap();
      }
   }
}

static void test_browser(void)
{
   int theme, shell;

   pspcompat_host_pad(tl_pad);
   pspcompat_host_vblank_hook(tl_vblank);

   for (shell = 1; shell >= 0; shell--)
      for (theme = 0; theme < 2; theme++)
      {
         char n[64];
         const char *sn = shell ? "marquee" : "shelf";
         const char *tn = theme ? "light" : "dark";
         g_pcfg.ui_shell = shell;
         g_pcfg.theme = theme;
         g_pcfg.console = FE_CONSOLE_GBA;
         pcfg_display_select(FE_CONSOLE_GBA);
         tl_reset();
         tl_wait(70);                      /* art decodes, fades in */
         snprintf(n, sizeof(n), "01_browser_%s_%s", sn, tn);
         tl_shot(n);
         if (theme == 0)
         {
            /* The other art paths, one game each. */
            tl_press(PSP_CTRL_UP);          /* Metroid: PSP-pack .565 cover */
            tl_wait(60);
            snprintf(n, sizeof(n), "02_browser_%s_psp565_cover", sn);
            tl_shot(n);
            tl_press(PSP_CTRL_UP);          /* Mario Kart: a small PNG */
            tl_press(PSP_CTRL_UP);          /* Minish Cap: PSP-size hero */
            tl_wait(60);
            snprintf(n, sizeof(n), "02_browser_%s_psp_hero", sn);
            tl_shot(n);
            tl_press(PSP_CTRL_UP);          /* Golden Sun: square cover */
            tl_wait(60);
            snprintf(n, sizeof(n), "02_browser_%s_square_cover", sn);
            tl_shot(n);
            tl_press(PSP_CTRL_UP);          /* FF Tactics: no art at all */
            tl_wait(60);
            snprintf(n, sizeof(n), "02_browser_%s_no_art", sn);
            tl_shot(n);
            tl_press(PSP_CTRL_UP);          /* Castlevania: PSP-pack hero */
            tl_wait(60);
            snprintf(n, sizeof(n), "02_browser_%s_psp565_hero", sn);
            tl_shot(n);
            tl_press(PSP_CTRL_DOWN); tl_press(PSP_CTRL_DOWN);
            tl_press(PSP_CTRL_DOWN); tl_press(PSP_CTRL_DOWN);
            tl_press(PSP_CTRL_DOWN); tl_press(PSP_CTRL_DOWN);
                                            /* back on Emerald */
            tl_press(PSP_CTRL_DOWN); tl_press(PSP_CTRL_DOWN);
            tl_wait(150);                   /* Mystery Dungeon: the long
                                             * title is mid-scroll */
            snprintf(n, sizeof(n), "02_browser_%s_long_title", sn);
            tl_shot(n);
            tl_press(PSP_CTRL_UP); tl_press(PSP_CTRL_UP);
            tl_wait(40);
         }
         /* The save-state shelf of Emerald. */
         tl_press(PSP_CTRL_LEFT);
         tl_wait(40);
         snprintf(n, sizeof(n), "03_browser_%s_%s_states", sn, tn);
         tl_shot(n);
         if (theme == 0 && shell == 1)
         {
            tl_press(PSP_CTRL_DOWN);
            tl_press(PSP_CTRL_SQUARE);
            tl_wait(4);
            tl_shot("03_browser_states_delete_ask");
            tl_press(PSP_CTRL_CIRCLE);
         }
         tl_press(PSP_CTRL_CIRCLE);
         tl_wait(20);
         /* Star a game, the favourites view. */
         tl_press(PSP_CTRL_DOWN);
         tl_wait(30);
         tl_press(PSP_CTRL_SQUARE);
         tl_wait(5);
         if (theme == 0 && shell == 1)
            tl_shot("04_browser_star_pop");
         tl_wait(30);
         tl_press(PSP_CTRL_SELECT);
         tl_wait(60);
         snprintf(n, sizeof(n), "04_browser_%s_%s_favs", sn, tn);
         tl_shot(n);
         tl_press(PSP_CTRL_SELECT);
         tl_wait(40);
         /* START: Settings over the game's blurred art, then Controls. */
         tl_press(PSP_CTRL_UP);
         tl_wait(50);
         tl_press(PSP_CTRL_START);
         tl_wait(10);
         snprintf(n, sizeof(n), "05_browser_%s_%s_settings", sn, tn);
         tl_shot(n);
         tl_press(PSP_CTRL_UP);            /* wraps to Button mapping */
         tl_wait(6);
         snprintf(n, sizeof(n), "05_browser_%s_%s_settings_end", sn, tn);
         tl_shot(n);
         tl_press(PSP_CTRL_CROSS);
         tl_wait(6);
         snprintf(n, sizeof(n), "05_browser_%s_%s_controls", sn, tn);
         tl_shot(n);
         tl_press(PSP_CTRL_CIRCLE);
         tl_press(PSP_CTRL_CIRCLE);
         tl_wait(20);
         /* TRIANGLE: the console flare, GB, GBC. */
         tl_press(PSP_CTRL_TRIANGLE);
         g_t -= 8; tl_wait(11);
         if (theme == 0 && shell == 1)
            tl_shot("06_browser_flare_gb");
         tl_wait(11);
         if (theme == 0 && shell == 1)
            tl_shot("06_browser_flare_gb_caption");
         tl_wait(100);
         snprintf(n, sizeof(n), "07_browser_%s_%s_gb", sn, tn);
         tl_shot(n);
         tl_press(PSP_CTRL_DOWN);
         tl_wait(30);
         tl_press(PSP_CTRL_LEFT);          /* Pokemon Red's one state */
         tl_wait(40);
         if (theme == 0)
         {
            snprintf(n, sizeof(n), "07_browser_%s_gb_states", sn);
            tl_shot(n);
         }
         tl_press(PSP_CTRL_CIRCLE);
         tl_wait(20);
         tl_press(PSP_CTRL_TRIANGLE);
         tl_wait(130);
         snprintf(n, sizeof(n), "08_browser_%s_%s_gbc", sn, tn);
         tl_shot(n);
         tl_press(PSP_CTRL_SELECT);        /* no GBC favourites */
         tl_wait(40);
         if (theme == 0)
         {
            snprintf(n, sizeof(n), "08_browser_%s_gbc_no_favs", sn);
            tl_shot(n);
         }
         tl_press(PSP_CTRL_SELECT);
         tl_wait(30);
         tl_press(PSP_CTRL_TRIANGLE);      /* back to GBA */
         tl_wait(130);
         tl_quit();
         printf("browser %s %s\n", sn, tn);
         browser_run();
      }

   /* A first run: no games anywhere.  The page names the folder -- on a
    * console pspcompat's root, ux0:data/GBAdhoc/roms, which is what the
    * shot uses (nothing is read from it: it does not exist here). */
   {
      char out[PSP_FILE_PATH_CAP], empty[300], f[400];
      int slot, theme;
      fe_console_t con;
      FILE *fp;
      for (theme = 0; theme < 2; theme++)
      {
         g_pcfg.theme = theme;
         g_pcfg.ui_shell = 1;
         g_pcfg.console = FE_CONSOLE_GBA;
         tl_reset();
         tl_wait(20);
         tl_shot(theme ? "09_browser_first_run_light" : "09_browser_no_roms");
         tl_press(PSP_CTRL_CROSS);          /* look again: still nothing */
         tl_wait(4);
         if (!theme)
            tl_shot("09_browser_no_roms_looked");
         tl_quit();
         g_running = 1; g_frame = 0;
         ui_browser(PSPCOMPAT_ROOT_VITA "/roms", out, sizeof(out), &slot,
                    &con);
         g_running = 1;
         osd_reset();
      }
      g_pcfg.theme = 0;
      /* This console empty, another one not: the triangle says so.  Then a
       * game is copied in while the page is open, and X finds it. */
      snprintf(empty, sizeof(empty), "%s/empty", g_root);
      mkdir(empty, 0755);
      snprintf(f, sizeof(f), "%s/Tetris (World) (Rev 1).gb", empty);
      fp = fopen(f, "wb");
      if (fp) { fputs("x", fp); fclose(fp); }
      snprintf(f, sizeof(f), "%s/Golden Sun (USA, Europe).gba", empty);
      remove(f);
      tl_reset();
      tl_wait(20);
      tl_shot("09_browser_empty_console");
      tl_wait(4);
      tl_event(SIM_EV_SWAP_FAIL);           /* = "the file arrives now" */
      tl_wait(2);
      tl_press(PSP_CTRL_CROSS);
      tl_wait(30);
      tl_shot("09_browser_rescan_found");
      tl_quit();
      snprintf(g_new_rom, sizeof(g_new_rom), "%s", f);
      g_running = 1; g_frame = 0;
      ui_browser(empty, out, sizeof(out), &slot, &con);
      g_running = 1;
      g_new_rom[0] = '\0';
      remove(f);
      osd_reset();
   }
}

static void test_loading(void)
{
   int theme;
   for (theme = 0; theme < 2; theme++)
   {
      char src[700], dst[700];
      /* Pick Emerald from the browser: its hero becomes the loading art. */
      g_pcfg.theme = theme;
      g_pcfg.ui_shell = 1;
      g_pcfg.console = FE_CONSOLE_GBA;
      pcfg_display_select(FE_CONSOLE_GBA);
      snprintf(g_pcfg.last_rom, sizeof(g_pcfg.last_rom), "%s",
               "Pokemon - Emerald Version (USA, Europe).gba");
      tl_reset();
      tl_wait(60);
      tl_press(PSP_CTRL_CROSS);
      ui_loading_dump_shots(4);
      browser_run();
      ui_loading_update("Reading ROM", 3u << 20, 16u << 20);
      pspcompat_host_advance(200000);
      ui_loading_update("Reading ROM", 11u << 20, 16u << 20);
      pspcompat_host_advance(200000);
      ui_loading_update("Starting the core", 0, 0);
      ui_loading_finish(1);
      snprintf(src, sizeof(src), "%s/log/ge_loading_%d.png", g_root,
               theme * 4 + 2);
      snprintf(dst, sizeof(dst), "%s/10_loading_art_%s.png", g_out,
               theme ? "light" : "dark");
      if (rename(src, dst) == 0)
         g_shots++;
      vid_ambient_drop();
   }
   /* A game with no art: the console's palette and the cartridge. */
   {
      char src[700], dst[700];
      g_pcfg.theme = 0;
      snprintf(g_pcfg.last_rom, sizeof(g_pcfg.last_rom), "%s",
               "Final Fantasy Tactics Advance (USA).gba");
      tl_reset();
      tl_wait(60);
      tl_press(PSP_CTRL_CROSS);
      ui_loading_dump_shots(3);
      browser_run();
      ui_loading_update("Reading ROM", 9u << 20, 16u << 20);
      ui_loading_finish(1);
      snprintf(src, sizeof(src), "%s/log/ge_loading_9.png", g_root);
      snprintf(dst, sizeof(dst), "%s/10_loading_no_art.png", g_out);
      if (rename(src, dst) == 0)
         g_shots++;
   }
}

static void test_game_osd(void)
{
   tl_reset();
   game_begin("Pokemon - Emerald Version (USA, Europe).gba", "BPEE",
              FE_CONSOLE_GBA, 1);
   vid_set_mode(VID_SCALE_INT3, VID_FILTER_NEAREST);
   osd_reset();
   osd_toast("Hosting Room 7 by Link Cable. Waiting for the other Vita.");
   osd_chip_session("Room 7  hosting");
   picture_shot("20_game_osd_toast", g_game, g_gw, g_gh);
   osd_reset();
   osd_chip_fps("59.7 fps (60)");
   osd_chip_ff("Unlimited Smooth");
   osd_chip_session("Room 7  P1  linked");
   vid_set_mode(VID_SCALE_FIT, VID_FILTER_SHARP);
   picture_shot("20_game_osd_chips", g_game, g_gw, g_gh);
   vid_set_mode(VID_SCALE_INT3, VID_FILTER_NEAREST);
   osd_reset();
}

static void test_menu(void)
{
   int theme;
   for (theme = 0; theme < 2; theme++)
   {
      const char *tn = theme ? "light" : "dark";
      char n[64];
      g_pcfg.theme = theme;
      game_begin("Pokemon - Emerald Version (USA, Europe).gba", "BPEE",
                 FE_CONSOLE_GBA, 1);
      vid_set_mode(VID_SCALE_INT3, VID_FILTER_NEAREST);

      /* The menu, mid-opening and open. */
      menu_open();
      g_t = 4;
      if (!theme) tl_shot("21_menu_opening");
      g_t = 14;
      snprintf(n, sizeof(n), "21_menu_%s", tn);
      tl_shot(n);
      /* Save states. */
      tl_press(PSP_CTRL_DOWN);
      tl_press(PSP_CTRL_CROSS);
      tl_wait(12);                       /* the dwell reads slot 1's .shot */
      snprintf(n, sizeof(n), "22_states_%s", tn);
      tl_shot(n);
      if (!theme)
      {
         tl_press(PSP_CTRL_DOWN);
         tl_press(PSP_CTRL_DOWN);
         tl_wait(12);
         tl_shot("22_states_thumb_only");     /* slot 3: a 3.1 state */
         tl_press(PSP_CTRL_DOWN);
         tl_wait(12);
         tl_shot("22_states_empty");          /* slot 4 */
         tl_press(PSP_CTRL_SQUARE);           /* save it */
         tl_wait(14);
         tl_shot("22_states_saved");
         tl_press(PSP_CTRL_TRIANGLE);
         tl_shot("22_states_delete_ask");
         tl_press(PSP_CTRL_CROSS);            /* delete: the fixture stays */
         tl_wait(4);
         tl_shot("22_states_deleted");
         tl_press(PSP_CTRL_DOWN);             /* slot 5: no previews */
         tl_wait(12);
         tl_shot("22_states_no_preview");
      }
      tl_press(PSP_CTRL_CIRCLE);
      /* Settings (the menu's fifth row): the top, each section, the end.
       * Rows: Player name, Connection, Room, Link mode, Link delay, Session
       * overlay | Video scale, Video filter, Ambient bars, (GB palette),
       * Colours, Motion blend, Wide view, Wide view: menus | Theme, Menu
       * style | Fast-forward, FF button, Rewind, FPS | Button mapping. */
      tl_down(4);
      tl_press(PSP_CTRL_CROSS);
      tl_wait(4);
      snprintf(n, sizeof(n), "23_settings_%s_1", tn);
      tl_shot(n);
      if (!theme)
      {
         /* Player name: the system keyboard, then the new name. */
         g_ime_reply = "Robin";
         g_ime_frames = 12;
         tl_press(PSP_CTRL_CROSS);
         g_t -= 4;
         tl_shot("23_settings_name_keyboard");
         tl_wait(20);
         tl_press(PSP_CTRL_DOWN);           /* Connection */
         tl_press(PSP_CTRL_RIGHT);          /* -> Internet */
         tl_shot("23_settings_connection");
         tl_press(PSP_CTRL_LEFT);
         tl_down(3);                        /* Link delay */
         tl_press(PSP_CTRL_RIGHT); tl_press(PSP_CTRL_RIGHT);
         tl_shot("23_settings_link_delay");
         tl_press(PSP_CTRL_LEFT); tl_press(PSP_CTRL_LEFT);
         tl_down(3);                        /* Video filter */
      }
      else
         tl_down(7);                        /* Video filter */
      tl_press(PSP_CTRL_RIGHT); tl_press(PSP_CTRL_RIGHT);
      tl_press(PSP_CTRL_RIGHT);             /* -> LCD grid */
      if (!theme) tl_shot("23_settings_lcd");
      tl_press(PSP_CTRL_RIGHT);             /* -> xBRZ smooth */
      if (!theme) tl_shot("23_settings_xbrz");
      tl_press(PSP_CTRL_RIGHT); tl_press(PSP_CTRL_RIGHT);
      tl_press(PSP_CTRL_RIGHT);             /* Scale3x, scanlines, nearest */
      tl_down(2);                           /* Ambient bars, Colours */
      tl_press(PSP_CTRL_RIGHT);             /* -> GBA screen */
      if (!theme) tl_shot("23_settings_colours");
      tl_press(PSP_CTRL_LEFT);
      tl_down(2);                           /* Motion blend, Wide view */
      if (!theme) tl_shot("23_settings_wide");
      tl_down(4);          /* Wide view: menus, Theme, Menu style, Fast-forward */
      snprintf(n, sizeof(n), "23_settings_%s_2", tn);
      tl_shot(n);
      tl_down(2);                           /* Rewind: 1 min -> 3 min -> 1 min */
      if (!theme)
      {
         tl_press(PSP_CTRL_RIGHT);
         tl_shot("23_settings_rewind");
         tl_press(PSP_CTRL_LEFT);
      }
      tl_down(2);
      snprintf(n, sizeof(n), "23_settings_%s_3", tn);
      tl_shot(n);
      /* Controls. */
      tl_press(PSP_CTRL_CROSS);
      tl_wait(4);
      snprintf(n, sizeof(n), "24_controls_%s", tn);
      tl_shot(n);
      if (!theme)
      {
         tl_press(PSP_CTRL_CROSS);            /* capture A */
         tl_wait(10);
         tl_shot("24_controls_capture");
         tl_hold(PSP_CTRL_SQUARE, 4);         /* Square: taken from FF */
         tl_wait(4);
         tl_shot("24_controls_conflict");
         tl_press(PSP_CTRL_UP);               /* Reset */
         tl_press(PSP_CTRL_CROSS);
         tl_shot("24_controls_reset_ask");
         tl_press(PSP_CTRL_CROSS);
         tl_press(PSP_CTRL_UP);               /* the fixed Menu row is not a stop */
         tl_shot("24_controls_shortcuts");
         tl_press(PSP_CTRL_CROSS);
         tl_wait(8);
         tl_hold(PSP_CTRL_LTRIGGER | PSP_CTRL_RTRIGGER | PSP_CTRL_UP, 6);
         tl_wait(4);
         tl_shot("24_controls_chord");
         tl_press(PSP_CTRL_TRIANGLE);         /* back to its default */
      }
      tl_press(PSP_CTRL_CIRCLE);
      tl_press(PSP_CTRL_CIRCLE);
      tl_wait(4);
      run_menu(0, NULL);
      if (theme == 0)
      {
         /* Whatever the script did to the bindings stays out of the file. */
         ctl_reset(&g_pcfg.controls);
         EXPECT(!strcmp(g_pcfg.nick, "Robin"), "Player name: nick is \"%s\"",
                g_pcfg.nick);
         EXPECT(g_pcfg.net_conn == UI_CONN_WIFI && g_pcfg.link_delay == 0,
                "Settings left conn=%d delay=%d", g_pcfg.net_conn,
                g_pcfg.link_delay);
      }
   }
   g_pcfg.theme = 0;

   /* Leaving asks first: X on "Quit to game list" puts the question on the
    * plate, O (or any key but X) stays, X again leaves. */
   menu_open();
   tl_up(2);
   tl_press(PSP_CTRL_CROSS);
   tl_shot("21_menu_leave_ask");
   tl_press(PSP_CTRL_CIRCLE);
   tl_wait(2);
   tl_shot("21_menu_leave_stayed");
   tl_press(PSP_CTRL_CIRCLE);            /* now O is "resume" again */
   tl_wait(4);
   run_menu(0, NULL);
   EXPECT(!g_acts[UI_ACT_GAMELIST] && !g_acts[UI_ACT_EXIT] &&
          g_acts[UI_ACT_RESUME] == 1, "leave asked, O: list=%d exit=%d "
          "resume=%d", g_acts[UI_ACT_GAMELIST], g_acts[UI_ACT_EXIT],
          g_acts[UI_ACT_RESUME]);
   menu_open();
   tl_up(1);                             /* Exit GBAdhoc */
   tl_press(PSP_CTRL_CROSS);
   tl_press(PSP_CTRL_CROSS);
   tl_wait(4);
   run_menu(0, NULL);
   EXPECT(g_acts[UI_ACT_EXIT] == 1, "leave asked, X: exit=%d",
          g_acts[UI_ACT_EXIT]);
   ui_close();
}

/* The menu, then its third row: Play together. */
static void play_open(void)
{
   menu_open();
   tl_down(2);
   tl_press(PSP_CTRL_CROSS);
   tl_wait(4);
}

/* A tap on a keypad key: column, row of the 42 x 36 grid at x0,y0. */
static void tl_key(int x0, int y0, int col, int row)
{
   tl_tap(x0 + col * 42 + 20, y0 + row * 36 + 17);
}

/* Type an address on SCR_ADDR's keypad with the finger. */
static void tl_type_addr(const char *t)
{
   for (; *t; t++)
   {
      int col = 0, row = 0;
      if (*t >= '1' && *t <= '9')
      {
         col = (*t - '1') % 3;
         row = (*t - '1') / 3;
      }
      else if (*t == '0') { col = 0; row = 3; }
      else if (*t == '.') { col = 3; row = 1; }
      else if (*t == ':') { col = 3; row = 2; }
      tl_key(20, 102, col, row);
   }
}

static void test_play(void)
{
   g_pcfg.theme = 0;
   g_pcfg.net_conn = UI_CONN_WIFI;
   g_pcfg.link_delay = 0;
   memset(g_pcfg.net_addr, 0, sizeof(g_pcfg.net_addr));

   /* ---- the page, on Wi-Fi, for a game that links both ways ------------ */
   game_begin("Pokemon - Emerald Version (USA, Europe).gba", "BPEE",
              FE_CONSOLE_GBA, 1);
   play_open();
   tl_shot("30_play_wifi");
   tl_press(PSP_CTRL_RIGHT);                 /* Host row: the room number */
   tl_shot("30_play_room_changed");
   tl_press(PSP_CTRL_LEFT);
   tl_press(PSP_CTRL_CROSS);                 /* Host -> the one question */
   tl_shot("30_play_host_ask");
   tl_press(PSP_CTRL_CIRCLE);
   tl_down(3);                               /* Connection */
   tl_shot("30_play_connection_row");
   tl_press(PSP_CTRL_CROSS);
   tl_wait(2);
   tl_shot("36_connection_wifi");
   tl_press(PSP_CTRL_DOWN);
   tl_shot("36_connection_internet_cursor");
   tl_down(2);                               /* Link delay */
   tl_press(PSP_CTRL_RIGHT); tl_press(PSP_CTRL_RIGHT);
   tl_press(PSP_CTRL_RIGHT);
   tl_shot("36_connection_delay");
   tl_press(PSP_CTRL_LEFT); tl_press(PSP_CTRL_LEFT); tl_press(PSP_CTRL_LEFT);
   tl_press(PSP_CTRL_CIRCLE);
   tl_down(2);                               /* How to play together */
   tl_press(PSP_CTRL_CROSS);
   tl_shot("38_help_steps");
   tl_press(PSP_CTRL_CROSS);
   tl_shot("38_help_connections");
   tl_press(PSP_CTRL_CIRCLE);
   tl_up(1);                                 /* Mystery Gift */
   tl_press(PSP_CTRL_CROSS);
   tl_shot("33_mystery_gift");
   tl_press(PSP_CTRL_DOWN);
   tl_press(PSP_CTRL_CROSS);                 /* start listening */
   tl_wait(4);
   tl_shot("33_mystery_gift_listening");
   tl_press(PSP_CTRL_CROSS);                 /* stop */
   tl_press(PSP_CTRL_CIRCLE);
   tl_press(PSP_CTRL_CIRCLE);
   tl_wait(4);
   run_menu(0, NULL);

   /* ---- Join on Wi-Fi: the list fills by itself ------------------------- */
   play_open();
   tl_press(PSP_CTRL_DOWN);
   tl_press(PSP_CTRL_CROSS);                 /* Join a game */
   tl_wait(4);
   tl_shot("31_join_searching");
   tl_wait(26);
   tl_shot("31_join_first_room");
   tl_wait(140);
   tl_shot("31_join_list");
   tl_down(3);                               /* another game's room */
   tl_press(PSP_CTRL_CROSS);
   tl_shot("31_join_refused");
   tl_down(3);
   tl_shot("31_join_scrolled");
   tl_up(6);
   tl_press(PSP_CTRL_CROSS);                 /* Ana's room: join */
   tl_wait(4);
   run_menu(0, NULL);
   EXPECT(g_acts[UI_ACT_NET_JOIN] == 1 && g_joined.listed &&
          !strcmp(g_joined.addr, "192.168.1.31:19019") &&
          g_joined.id == 0x1001 && !strcmp(ui_group(), "GC07BPEE"),
          "join: acts=%d listed=%d addr=%s id=%x group=%s",
          g_acts[UI_ACT_NET_JOIN], g_joined.listed, g_joined.addr,
          g_joined.id, ui_group());
   EXPECT(g_sim.begins >= 1 && g_sim.ends >= 1 && !g_sim.rooms_open,
          "join: scan begins=%d ends=%d open=%d", g_sim.begins, g_sim.ends,
          g_sim.rooms_open);
   sim_linking("ok", -1);
   sim_linking("full", UI_FAIL_ROOM_FULL);
   sim_linking("version", UI_FAIL_VERSION);
   sim_linking("lost", UI_FAIL_NO_ANSWER);

   /* Nothing hosting anywhere; Wi-Fi dropping and coming back. */
   game_begin("Pokemon - Emerald Version (USA, Europe).gba", "BPEE",
              FE_CONSOLE_GBA, 1);
   g_sim.rooms_n = 0;
   play_open();
   tl_press(PSP_CTRL_DOWN);
   tl_press(PSP_CTRL_CROSS);
   tl_wait(110);
   tl_shot("31_join_empty");
   tl_event(SIM_EV_WIFI_OFF);
   tl_wait(40);
   tl_shot("31_join_wifi_lost");
   tl_event(SIM_EV_WIFI_ON);
   tl_wait(70);
   tl_shot("31_join_wifi_back");
   tl_press(PSP_CTRL_CIRCLE);
   tl_wait(2);
   run_menu(0, NULL);
   EXPECT(g_sim.begins >= 2 && !g_sim.rooms_open,
          "join: the scan restarts after Wi-Fi returns (begins=%d open=%d)",
          g_sim.begins, g_sim.rooms_open);

   /* Wi-Fi off: the page says so, and Host/Join explain instead of acting. */
   game_begin("Pokemon - Emerald Version (USA, Europe).gba", "BPEE",
              FE_CONSOLE_GBA, 1);
   sim_wifi(UI_WIFI_OFF);
   play_open();
   tl_shot("30_play_no_wifi");
   tl_press(PSP_CTRL_DOWN);
   tl_press(PSP_CTRL_CROSS);
   tl_shot("30_play_no_wifi_join");
   tl_wait(2);
   run_menu(0, NULL);
   EXPECT(!g_acts[UI_ACT_NET_JOIN] && !g_acts[UI_ACT_NET_HOST] &&
          g_sim.begins == 0, "no Wi-Fi: nothing may start");

   /* ---- Host on Wi-Fi: the waiting page --------------------------------- */
   game_begin("Advance Wars 2 - Black Hole Rising (USA).gba", "AW2E",
              FE_CONSOLE_GBA, 1);
   play_open();
   tl_shot("30_play_cable");
   tl_press(PSP_CTRL_CROSS);                 /* Host: no question to ask */
   tl_wait(2);
   tl_shot("34_host_opening");
   tl_wait(24);
   tl_shot("34_host_wifi");
   tl_press(PSP_CTRL_CROSS);                 /* Keep playing */
   tl_wait(2);
   run_menu(0, NULL);
   EXPECT(g_acts[UI_ACT_NET_HOST] == 1 && g_acts[UI_ACT_RESUME] == 1 &&
          ui_host_cable() == 1, "host: acts host=%d resume=%d cable=%d",
          g_acts[UI_ACT_NET_HOST], g_acts[UI_ACT_RESUME], ui_host_cable());
   menu_open();                              /* later: the menu says so */
   tl_shot("34_menu_hosting");
   tl_down(2);
   tl_press(PSP_CTRL_CROSS);
   tl_wait(2);
   tl_press(PSP_CTRL_DOWN);
   tl_shot("34_host_stop_cursor");
   tl_press(PSP_CTRL_CROSS);                 /* Stop hosting */
   tl_wait(6);
   tl_shot("34_host_stopped");
   tl_wait(2);
   run_menu(0, NULL);
   EXPECT(g_acts[UI_ACT_NET_DISCONNECT] == 1, "stop hosting: no action");
   osd_reset();

   /* A Host the network refuses. */
   game_begin("Advance Wars 2 - Black Hole Rising (USA).gba", "AW2E",
              FE_CONSOLE_GBA, 1);
   g_sim.host_fail = UI_FAIL_PORT;
   play_open();
   tl_press(PSP_CTRL_CROSS);
   tl_wait(30);
   tl_shot("34_host_failed");
   tl_wait(2);
   run_menu(0, NULL);

   /* ---- Internet: the address page, the probe, the host's address ------- */
   game_begin("Pokemon - Emerald Version (USA, Europe).gba", "BPEE",
              FE_CONSOLE_GBA, 1);
   g_pcfg.net_conn = UI_CONN_INTERNET;
   play_open();
   tl_shot("30_play_internet");
   tl_press(PSP_CTRL_DOWN);
   tl_press(PSP_CTRL_CROSS);                 /* Join -> the address */
   tl_wait(2);
   tl_shot("37_addr_empty");
   tl_type_addr("203.0.113");
   tl_shot("37_addr_typing");
   tl_tap(20 + 2 * 42 + 40, 102 + 3 * 36 + 17);   /* Join, too early */
   tl_shot("37_addr_not_yet");
   tl_type_addr(".9:19019");
   tl_shot("37_addr_ready");
   tl_tap(20 + 2 * 42 + 40, 102 + 3 * 36 + 17);   /* Join */
   tl_wait(10);
   tl_shot("31_join_addr_searching");
   tl_wait(50);
   tl_shot("31_join_addr_found");
   tl_press(PSP_CTRL_CIRCLE);                /* back: it is remembered */
   tl_wait(2);
   tl_shot("37_addr_recent");
   tl_press(PSP_CTRL_SQUARE); tl_press(PSP_CTRL_SQUARE);
   tl_press(PSP_CTRL_SQUARE); tl_press(PSP_CTRL_SQUARE);
   tl_press(PSP_CTRL_SQUARE);
   tl_press(PSP_CTRL_RIGHT); tl_press(PSP_CTRL_RIGHT);
   tl_press(PSP_CTRL_RIGHT);
   tl_press(PSP_CTRL_RIGHT);                 /* the pad: over to RECENT */
   tl_shot("37_addr_recent_cursor");
   tl_press(PSP_CTRL_CROSS);                 /* use it */
   tl_wait(60);
   tl_press(PSP_CTRL_CROSS);                 /* join the room behind it */
   tl_wait(4);
   run_menu(0, NULL);
   EXPECT(g_acts[UI_ACT_NET_JOIN] == 1 &&
          !strcmp(g_joined.addr, "203.0.113.9:19019") &&
          g_joined.conn == UI_CONN_INTERNET,
          "internet join: acts=%d addr=%s conn=%d", g_acts[UI_ACT_NET_JOIN],
          g_joined.addr, g_joined.conn);
   EXPECT(!strcmp(g_pcfg.net_addr[0], "203.0.113.9:19019") &&
          !g_pcfg.net_addr[1][0], "recent addresses: \"%s\" \"%s\"",
          g_pcfg.net_addr[0], g_pcfg.net_addr[1]);
   /* An address nobody answers at; three more remembered. */
   pcfg_net_addr_push("198.51.100.4");
   pcfg_net_addr_push("192.168.1.31:19019");
   pcfg_net_addr_push("203.0.113.77:19020");
   pcfg_net_addr_push("192.0.2.200");
   EXPECT(!strcmp(g_pcfg.net_addr[0], "192.0.2.200") &&
          !strcmp(g_pcfg.net_addr[3], "198.51.100.4"),
          "recent addresses keep the newest four");
   game_begin("Pokemon - Emerald Version (USA, Europe).gba", "BPEE",
              FE_CONSOLE_GBA, 1);
   play_open();
   tl_press(PSP_CTRL_DOWN);
   tl_press(PSP_CTRL_CROSS);
   tl_wait(2);
   tl_shot("37_addr_four_recent");
   tl_press(PSP_CTRL_CROSS);                 /* Join: 192.0.2.200 */
   tl_wait(110);
   tl_shot("31_join_addr_no_answer");
   tl_press(PSP_CTRL_CIRCLE);
   tl_press(PSP_CTRL_CIRCLE);
   tl_wait(2);
   /* Host on the internet: the router is asked, then the address. */
   tl_up(1);
   tl_press(PSP_CTRL_CROSS);                 /* Host -> the question */
   tl_press(PSP_CTRL_CROSS);                 /* Link Cable */
   tl_wait(24);
   tl_shot("34_host_internet_asking");
   tl_wait(75);                              /* the page asks twice a second */
   tl_shot("34_host_internet");
   tl_wait(2);
   run_menu(0, NULL);
   game_begin("Pokemon - Emerald Version (USA, Europe).gba", "BPEE",
              FE_CONSOLE_GBA, 1);
   g_sim_pub_result = UI_PUBLIC_CLOSED;
   play_open();
   tl_press(PSP_CTRL_CROSS);
   tl_press(PSP_CTRL_DOWN);                  /* Wireless Adapter this time */
   tl_press(PSP_CTRL_CROSS);
   tl_wait(100);
   tl_shot("34_host_internet_closed");
   tl_wait(2);
   run_menu(0, NULL);
   g_sim_pub_result = UI_PUBLIC_OPEN;
   EXPECT(ui_host_cable() == 0, "host by adapter: ui_host_cable()=%d",
          ui_host_cable());

   /* ---- Ad hoc: what comes next ----------------------------------------- */
   g_pcfg.net_conn = UI_CONN_ADHOC;
   game_begin("Advance Wars 2 - Black Hole Rising (USA).gba", "AW2E",
              FE_CONSOLE_GBA, 1);
   play_open();
   tl_shot("30_play_adhoc");
   tl_press(PSP_CTRL_CROSS);                 /* Host */
   tl_shot("39_adhoc_host");
   tl_press(PSP_CTRL_CROSS);                 /* Continue */
   tl_wait(6);
   tl_shot("39_adhoc_host_opening");
   tl_wait(20);
   tl_shot("34_host_adhoc");
   tl_press(PSP_CTRL_DOWN);
   tl_press(PSP_CTRL_CROSS);                 /* Stop hosting */
   tl_wait(4);
   run_menu(0, NULL);
   osd_reset();
   game_begin("Advance Wars 2 - Black Hole Rising (USA).gba", "AW2E",
              FE_CONSOLE_GBA, 1);
   play_open();
   tl_press(PSP_CTRL_DOWN);
   tl_press(PSP_CTRL_CROSS);                 /* Join */
   tl_shot("39_adhoc_join");
   tl_press(PSP_CTRL_CROSS);                 /* Continue */
   tl_wait(4);
   run_menu(0, NULL);
   EXPECT(g_acts[UI_ACT_NET_JOIN] == 1 && !g_joined.listed &&
          g_joined.conn == UI_CONN_ADHOC && g_joined.kind == ROOM_CABLE_GBA,
          "ad hoc join: acts=%d listed=%d conn=%d kind=%d",
          g_acts[UI_ACT_NET_JOIN], g_joined.listed, g_joined.conn,
          g_joined.kind);
   sim_linking("adhoc", UI_FAIL_ADHOC_CANCELLED);
   g_pcfg.net_conn = UI_CONN_WIFI;

   /* ---- the other kinds of game ------------------------------------------ */
   game_begin("Homebrew/Anguna - Warriors of Virtue.gba", "BMGE",
              FE_CONSOLE_GBA, 1);
   play_open();
   tl_shot("30_play_adapter");
   tl_wait(2);
   run_menu(0, NULL);
   game_begin("Pokemon - Red Version (USA, Europe).gb", "POKEMON RED",
              FE_CONSOLE_GB, 1);
   vid_set_mode(VID_SCALE_INT3, VID_FILTER_NEAREST);
   play_open();
   tl_shot("30_play_gb");
   tl_down(2);
   tl_press(PSP_CTRL_CROSS);                 /* two players: a GB game too */
   tl_wait(2);
   tl_shot("32_local_gb");
   tl_down(2);
   tl_press(PSP_CTRL_CROSS);                 /* Partner's game: the picker */
   tl_wait(2);
   tl_shot("32_local_gb_picker");
   tl_press(PSP_CTRL_DOWN);
   tl_press(PSP_CTRL_CROSS);                 /* Blue beside Red */
   tl_wait(2);
   tl_shot("32_local_gb_other");
   tl_press(PSP_CTRL_CIRCLE);                /* back to Play together */
   tl_wait(2);
   tl_up(1);
   tl_press(PSP_CTRL_CROSS);                 /* Join: what a GB game sees */
   tl_wait(170);
   tl_shot("31_join_list_gb");
   tl_wait(2);
   run_menu(0, NULL);

   /* ---- linked: who am I, with whom, how good, how to end ---------------- */
   game_begin("Pokemon - Emerald Version (USA, Europe).gba", "BPEE",
              FE_CONSOLE_GBA, 1);
   g_sim.lv.state = UI_LINK_LINKED; g_sim.lv.cable = 1; g_sim.lv.player = 2;
   g_sim.lv.room = "Room 7"; g_sim.lv.game = "Pokemon Emerald";
   g_sim.lv.conn = UI_CONN_WIFI; g_sim.lv.peer = "Ana";
   g_sim.lv.rtt_ms = 12; g_sim.lv.signal_pct = 82;
   g_fch.suspended = 1;
   menu_open();
   tl_shot("34_menu_linked");
   tl_press(PSP_CTRL_DOWN);
   tl_press(PSP_CTRL_CROSS);                 /* save states are locked */
   tl_wait(2);
   tl_shot("34_menu_linked_locked");
   tl_press(PSP_CTRL_DOWN);
   tl_press(PSP_CTRL_CROSS);
   tl_wait(4);
   tl_shot("34_linked_cable");
   tl_wait(2);
   run_menu(1, "linked");
   osd_reset();
   g_sim.lv.cable = 0; g_sim.lv.player = 1; g_sim.lv.conn = UI_CONN_INTERNET;
   g_sim.lv.rtt_ms = 164; g_sim.lv.signal_pct = 40; g_sim.lv.peer = NULL;
   play_open();
   tl_shot("34_linked_adapter_slow");
   tl_press(PSP_CTRL_DOWN);
   tl_press(PSP_CTRL_CROSS);                 /* Disconnect */
   tl_wait(6);
   tl_shot("34_disconnected");
   tl_wait(2);
   run_menu(1, "linked");
   EXPECT(g_acts[UI_ACT_NET_DISCONNECT] == 1, "disconnect: no action");
   g_fch.suspended = 0;
   osd_reset();

   /* ---- two players on this Vita ------------------------------------------ */
   game_begin("Pokemon - Emerald Version (USA, Europe).gba", "BPEE",
              FE_CONSOLE_GBA, 1);
   g_pcfg.local_layout = VID_PAIR_SIDE;
   play_open();
   tl_down(2);
   tl_press(PSP_CTRL_CROSS);
   tl_wait(2);
   tl_shot("32_local_start");
   tl_press(PSP_CTRL_DOWN);
   tl_shot("32_local_own_save");
   tl_press(PSP_CTRL_DOWN);
   tl_shot("32_local_partner_row");
   tl_press(PSP_CTRL_CROSS);                 /* Partner's game: the picker */
   tl_wait(2);
   tl_shot("32_local_picker");
   tl_down(5);
   tl_shot("32_local_picker_scrolled");
   tl_press(PSP_CTRL_CROSS);                 /* Sapphire beside Emerald */
   tl_wait(2);
   tl_shot("32_local_other_game");
   tl_press(PSP_CTRL_UP);                    /* the start that exists */
   tl_shot("32_local_other_start");
   tl_press(PSP_CTRL_DOWN);
   tl_press(PSP_CTRL_CROSS);                 /* the picker again... */
   tl_wait(2);
   tl_up(5);
   tl_press(PSP_CTRL_CROSS);                 /* ...and this game again */
   tl_wait(2);
   EXPECT(g_sim.local.partner_game == NULL, "local: partner game not reset");
   tl_press(PSP_CTRL_DOWN);
   tl_press(PSP_CTRL_CROSS);                 /* Layout */
   tl_shot("32_local_layout");
   tl_up(3);
   tl_press(PSP_CTRL_CROSS);                 /* Start: a copy of my save */
   g_t -= 5;
   tl_shot("32_local_starting");
   tl_wait(20);
   tl_shot("32_local_running");
   tl_press(PSP_CTRL_CROSS);                 /* Swap controlled game */
   tl_shot("32_local_swapped");
   tl_down(2);
   tl_shot("32_local_stop_cursor");
   tl_press(PSP_CTRL_CIRCLE);                /* back to the menu */
   tl_wait(2);
   tl_shot("32_menu_local");
   tl_down(2);
   tl_press(PSP_CTRL_CROSS);                 /* Play together = this page */
   tl_down(2);
   tl_press(PSP_CTRL_CROSS);                 /* Stop */
   tl_wait(6);
   tl_shot("32_local_stopped");
   tl_wait(2);
   run_menu(0, NULL);
   EXPECT(g_acts[UI_ACT_NET_LOCAL] == 1 && g_acts[UI_ACT_NET_LOCAL_SWAP] == 1
          && g_acts[UI_ACT_NET_LOCAL_STOP] == 1 && !g_sim.local.active &&
          g_pcfg.local_partner == 0 && g_pcfg.local_layout == VID_PAIR_INSET,
          "local: start=%d swap=%d stop=%d active=%d partner=%d layout=%d",
          g_acts[UI_ACT_NET_LOCAL], g_acts[UI_ACT_NET_LOCAL_SWAP],
          g_acts[UI_ACT_NET_LOCAL_STOP], g_sim.local.active,
          g_pcfg.local_partner, g_pcfg.local_layout);
   g_pcfg.local_layout = VID_PAIR_SIDE;
   osd_reset();
   /* A PS TV with two pads: no swap row.  And a start that fails. */
   game_begin("Pokemon - Emerald Version (USA, Europe).gba", "BPEE",
              FE_CONSOLE_GBA, 1);
   g_sim.local.active = 1; g_sim.local.have_pad2 = 1;
   play_open();
   tl_shot("32_local_two_pads");
   tl_wait(2);
   run_menu(0, NULL);
   /* ...and with the other version in the partner's machine. */
   game_begin("Pokemon - Emerald Version (USA, Europe).gba", "BPEE",
              FE_CONSOLE_GBA, 1);
   g_sim.local.active = 1;
   g_sim.local.partner_game = SIM_PARTNERS_GBA[4];
   play_open();
   tl_shot("32_local_running_other");
   tl_wait(2);
   run_menu(0, NULL);
   /* Not while the network is in use. */
   game_begin("Pokemon - Emerald Version (USA, Europe).gba", "BPEE",
              FE_CONSOLE_GBA, 1);
   g_sim.local.available = 0;
   g_sim.local.why_not = "End the network game first.";
   play_open();
   tl_down(2);
   tl_press(PSP_CTRL_CROSS);
   tl_wait(2);
   tl_shot("32_local_not_now");
   tl_wait(2);
   run_menu(0, NULL);
   game_begin("Pokemon - Emerald Version (USA, Europe).gba", "BPEE",
              FE_CONSOLE_GBA, 1);
   g_sim.local_fail = 1; g_sim.local.partner_save = 1;
   play_open();
   tl_down(2);
   tl_press(PSP_CTRL_CROSS);
   tl_press(PSP_CTRL_DOWN);
   tl_press(PSP_CTRL_CROSS);                 /* Start: its own save */
   tl_wait(20);
   tl_shot("32_local_failed");
   tl_wait(2);
   run_menu(0, NULL);
   EXPECT(g_pcfg.local_partner == 1, "local: own save not recorded");
   g_pcfg.local_partner = 0;
   ui_set_link(NULL);

   /* THE LINKING SCREEN, as the main loop draws it during a bring-up. */
   {
      ui_linking_view v;
      ui_set_backdrop(g_snap, g_gw, g_gh);
      memset(&v, 0, sizeof(v));
      v.game = "Pokemon Emerald"; v.room = "Room 7";
      v.l1 = "Looking for Room 7";
      v.l2 = "Scanning for the host";
      v.frac = -1; v.busy = 1;
      ui_linking_draw(&v);
      shot_now("35_linking_busy");
      vid_swap();
      v.l1 = "Linked. Matching the two games";
      v.l2 = "Sending this console's save";
      v.amount = "84 / 128 KiB";
      v.l3 = "Both games start together when this finishes.";
      v.frac = 656;
      ui_linking_draw(&v);
      shot_now("35_linking_progress");
      vid_swap();
      {
         /* Every failure sentence, on the screen that shows them. */
         int w;
         for (w = 0; w < UI_FAIL_COUNT; w++)
         {
            char n[40];
            memset(&v, 0, sizeof(v));
            v.game = "Pokemon Emerald"; v.room = "Room 7";
            ui_link_fail_text(w, &v.l1, &v.l2);
            v.frac = -1; v.fail = 1;
            ui_linking_draw(&v);
            snprintf(n, sizeof(n), "35_linking_fail_%02d", w);
            shot_now(n);
            vid_swap();
         }
      }
   }
}

/* ---- cheats ---------------------------------------------------------------- */

static void cheats_open(void)
{
   menu_open();
   tl_down(3);
   tl_press(PSP_CTRL_CROSS);
   tl_wait(4);
}

/* A tap on a key of the code page's 6 x 4 keypad, by what it types. */
static void tl_code(const char *t)
{
   static const char *const rows[4] = { "123AB", "456CD", "789EF", "0" };
   for (; *t; t++)
   {
      int r, c, done = 0;
      if (*t == ' ')  { tl_tap(20 + 2 * 42, 68 + 3 * 36 + 17); continue; }
      if (*t == '\n') { tl_key(20, 68, 5, 1); continue; }
      if (*t == '-')  { tl_key(20, 68, 5, 2); continue; }
      for (r = 0; r < 4 && !done; r++)
         for (c = 0; rows[r][c] && !done; c++)
            if (rows[r][c] == *t)
            {
               tl_key(20, 68, c, r);
               done = 1;
            }
   }
}

static void test_cheats(void)
{
   int before;
   g_pcfg.theme = 0;
   game_begin("Pokemon - Emerald Version (USA, Europe).gba", "BPEE",
              FE_CONSOLE_GBA, 1);
   fake_cheats_fill(300);
   g_fch.e[1].enabled = g_fch.e[2].enabled = g_fch.e[140].enabled = 1;
   fake_cheat_put("My shiny starter", "82031DBC 0001", 0,
                  CHEAT_KIND_GBA_CODEBREAKER, 1, 0);

   menu_open();
   tl_down(3);
   tl_shot("60_menu_cheats");
   tl_press(PSP_CTRL_CROSS);
   tl_wait(4);
   tl_shot("60_cheats_list");
   tl_down(3);                               /* the first that is off */
   tl_press(PSP_CTRL_CROSS);                 /* on */
   tl_shot("60_cheats_switched_on");
   tl_down(4);
   tl_press(PSP_CTRL_CROSS);                 /* an unsupported code */
   tl_shot("60_cheats_unsupported");
   tl_press(PSP_CTRL_DOWN);                  /* a name too long for the row */
   tl_wait(150);
   tl_shot("60_cheats_long_name");
   tl_press(PSP_CTRL_RTRIGGER); tl_press(PSP_CTRL_RTRIGGER);
   tl_shot("60_cheats_paged");
   tl_hold(PSP_CTRL_DOWN, 260);              /* held: 1, then 3, then 6 rows */
   tl_shot("60_cheats_held_down");
   tl_hold(PSP_CTRL_DOWN, 240);
   tl_shot("60_cheats_end");                 /* the player's own entry */
   tl_press(PSP_CTRL_SQUARE);
   tl_shot("60_cheats_delete_ask");
   tl_press(PSP_CTRL_CIRCLE);                /* keep */
   tl_up(1);
   tl_press(PSP_CTRL_SQUARE);                /* a built-in one: no */
   tl_shot("60_cheats_delete_builtin");
   /* Find by name. */
   g_ime_reply = "money";
   g_ime_frames = 14;
   tl_press(PSP_CTRL_SELECT);
   g_t -= 3;
   tl_shot("60_cheats_find_keyboard");
   tl_wait(24);
   tl_shot("60_cheats_found");
   tl_press(PSP_CTRL_SELECT);                /* the whole list again */
   tl_wait(2);
   /* The finger: a flick, the coast, a tap on a row. */
   tl_drag(240, 200, 240, 90, 6, 4);
   tl_shot("62_cheats_flick_coasting");
   tl_wait(60);
   tl_shot("62_cheats_flick_rest");
   tl_finger(200, 130, 3);
   g_t -= 1;
   tl_shot("62_cheats_finger_down");
   tl_wait(8);
   tl_shot("62_cheats_tapped");
   tl_finger(474, 80, 2);                    /* the scrollbar: drag it */
   tl_drag(474, 80, 474, 200, 12, 0);
   g_t -= 1;
   tl_shot("62_cheats_scrollbar");
   tl_wait(6);
   tl_drag(240, 100, 240, 210, 6, 2);        /* flick back up the list... */
   tl_finger(200, 130, 3);                   /* ...a finger stops the coast, */
   tl_wait(30);                              /* and that is not a tap */
   tl_press(PSP_CTRL_CIRCLE);
   tl_wait(2);
   before = cheats_enabled_count();
   run_menu(0, NULL);
   EXPECT(cheats_enabled_count() == before + 2,
          "cheats: on-count %d -> %d (X, then a tap)", before,
          cheats_enabled_count());
   EXPECT(g_fch.n == 301, "cheats: nothing may have been deleted (%d)",
          g_fch.n);

   /* ---- add a code -------------------------------------------------------- */
   cheats_open();
   tl_press(PSP_CTRL_TRIANGLE);
   tl_wait(2);
   tl_shot("61_cheat_add_empty");
   tl_code("82025838 E0");
   tl_shot("61_cheat_add_typing");
   tl_tap(20 + 4 * 42 + 20, 68 + 3 * 36 + 17);    /* Save: too early */
   tl_shot("61_cheat_add_not_yet");
   tl_code("FF\n82025840 03E7");
   tl_shot("61_cheat_add_valid");
   g_ime_reply = "Lots of money";
   g_ime_frames = 14;
   tl_tap(370, 84);                               /* the NAME field */
   g_t -= 2;
   tl_shot("61_cheat_add_name_keyboard");
   tl_wait(24);
   tl_shot("61_cheat_add_named");
   tl_press(PSP_CTRL_CIRCLE);
   tl_shot("61_cheat_add_discard_ask");
   tl_press(PSP_CTRL_CIRCLE);                /* keep typing */
   tl_press(PSP_CTRL_TRIANGLE);              /* save */
   tl_wait(4);
   tl_shot("60_cheats_added");
   tl_press(PSP_CTRL_SQUARE);
   tl_press(PSP_CTRL_CROSS);                 /* and delete it again */
   tl_wait(2);
   tl_shot("60_cheats_deleted");
   /* The pad on the keypad: d-pad, X types, Square deletes. */
   tl_press(PSP_CTRL_TRIANGLE);
   tl_press(PSP_CTRL_RIGHT); tl_press(PSP_CTRL_RIGHT);
   tl_press(PSP_CTRL_RIGHT);                 /* A */
   tl_press(PSP_CTRL_CROSS); tl_press(PSP_CTRL_CROSS);
   tl_press(PSP_CTRL_DOWN);                  /* C */
   tl_press(PSP_CTRL_CROSS);
   tl_press(PSP_CTRL_SQUARE);
   tl_press(PSP_CTRL_RIGHT); tl_press(PSP_CTRL_RIGHT);
   tl_press(PSP_CTRL_RIGHT);                 /* past Line: the NAME field */
   tl_shot("61_cheat_add_pad");
   tl_press(PSP_CTRL_LEFT);
   tl_press(PSP_CTRL_SQUARE); tl_press(PSP_CTRL_SQUARE);
   tl_press(PSP_CTRL_CIRCLE);                /* empty: straight back */
   tl_press(PSP_CTRL_CIRCLE);
   tl_wait(2);
   run_menu(0, NULL);
   EXPECT(g_fch.n == 301, "cheats: add then delete leaves %d", g_fch.n);
   EXPECT(g_fch.added == 0, "cheats: the added one had a name");

   /* A Game Boy game: its code shapes, the dash key live. */
   game_begin("Pokemon - Red Version (USA, Europe).gb", "POKEMON RED",
              FE_CONSOLE_GB, 1);
   vid_set_mode(VID_SCALE_INT3, VID_FILTER_NEAREST);
   fake_cheats_fill(0);
   menu_open();
   tl_down(3);
   tl_shot("60_menu_cheats_none");
   tl_press(PSP_CTRL_CROSS);
   tl_wait(2);
   tl_shot("60_cheats_none");
   tl_press(PSP_CTRL_TRIANGLE);
   tl_code("01F-D2A-");
   tl_shot("61_cheat_add_gb");
   tl_press(PSP_CTRL_SQUARE); tl_press(PSP_CTRL_SQUARE);
   tl_press(PSP_CTRL_SQUARE); tl_press(PSP_CTRL_SQUARE);
   tl_press(PSP_CTRL_SQUARE); tl_press(PSP_CTRL_SQUARE);
   tl_press(PSP_CTRL_SQUARE); tl_press(PSP_CTRL_SQUARE);
   tl_press(PSP_CTRL_CIRCLE);
   tl_press(PSP_CTRL_CIRCLE);
   tl_wait(2);
   run_menu(0, NULL);

   /* Linked to another console: the list is there, and off. */
   game_begin("Pokemon - Emerald Version (USA, Europe).gba", "BPEE",
              FE_CONSOLE_GBA, 1);
   fake_cheats_fill(300);
   g_fch.e[1].enabled = g_fch.e[2].enabled = 1;
   g_fch.suspended = 1;
   cheats_open();
   tl_shot("60_cheats_suspended");
   tl_press(PSP_CTRL_CROSS);
   tl_shot("60_cheats_suspended_refused");
   tl_wait(2);
   run_menu(1, "linked");
   EXPECT(cheats_enabled_count() == 2, "cheats: changed while suspended");
   g_fch.suspended = 0;
   /* A game the database knew only by its title. */
   g_fch_match = 4;
   cheats_open();
   tl_shot("60_cheats_title_match");
   tl_wait(2);
   run_menu(0, NULL);
   g_fch_match = 0;
   /* No keyboard to be had: Find says so and nothing hangs. */
   pspcompat_host_ime(NULL);
   cheats_open();
   tl_press(PSP_CTRL_SELECT);
   tl_shot("60_cheats_no_keyboard");
   tl_wait(2);
   run_menu(0, NULL);
   pspcompat_host_ime(ime_script);
   fake_cheats_fill(0);
}

/* ---- the new pages on the light theme ----------------------------------------
 * Everything above is shot on the dark page; one of each new kind of page
 * on the light one, where a colour taken from the wrong role shows. */
static void test_light(void)
{
   g_pcfg.theme = 1;
   g_pcfg.net_conn = UI_CONN_WIFI;
   game_begin("Pokemon - Emerald Version (USA, Europe).gba", "BPEE",
              FE_CONSOLE_GBA, 1);
   fake_cheats_fill(300);
   g_fch.e[1].enabled = g_fch.e[2].enabled = 1;
   play_open();
   tl_shot("80_light_play");
   tl_down(3);
   tl_press(PSP_CTRL_CROSS);                 /* Connection */
   tl_shot("80_light_connection");
   tl_press(PSP_CTRL_CIRCLE);
   tl_up(2);
   tl_press(PSP_CTRL_CROSS);                 /* Join */
   tl_wait(160);
   tl_down(2);
   tl_press(PSP_CTRL_CROSS);
   tl_shot("80_light_join");
   tl_press(PSP_CTRL_CIRCLE);
   tl_press(PSP_CTRL_UP);
   tl_press(PSP_CTRL_CROSS);                 /* Host -> ask -> waiting */
   tl_press(PSP_CTRL_CROSS);
   tl_wait(30);
   tl_shot("80_light_hosting");
   tl_down(1);
   tl_press(PSP_CTRL_CROSS);                 /* stop */
   tl_wait(4);
   tl_down(2);
   tl_press(PSP_CTRL_CROSS);                 /* Play together again */
   tl_down(2);
   tl_press(PSP_CTRL_CROSS);                 /* Two players */
   tl_shot("80_light_local");
   tl_press(PSP_CTRL_CIRCLE);
   tl_down(3);
   tl_press(PSP_CTRL_CROSS);                 /* How to play */
   tl_shot("80_light_help");
   tl_press(PSP_CTRL_CIRCLE);
   tl_press(PSP_CTRL_CIRCLE);
   tl_down(3);
   tl_press(PSP_CTRL_CROSS);                 /* Cheats */
   tl_wait(2);
   tl_shot("80_light_cheats");
   tl_press(PSP_CTRL_TRIANGLE);
   tl_code("82025838 E0FF");
   tl_shot("80_light_cheat_add");
   tl_press(PSP_CTRL_CIRCLE);
   tl_press(PSP_CTRL_CROSS);                 /* discard */
   tl_press(PSP_CTRL_CIRCLE);
   tl_wait(2);
   run_menu(0, NULL);
   osd_reset();
   g_pcfg.net_conn = UI_CONN_INTERNET;
   game_begin("Pokemon - Emerald Version (USA, Europe).gba", "BPEE",
              FE_CONSOLE_GBA, 1);
   play_open();
   tl_press(PSP_CTRL_DOWN);
   tl_press(PSP_CTRL_CROSS);                 /* Join: the address */
   tl_wait(2);
   tl_shot("80_light_address");
   tl_wait(2);
   run_menu(0, NULL);
   g_pcfg.net_conn = UI_CONN_WIFI;
   g_pcfg.theme = 0;
   fake_cheats_fill(0);
}

/* ---- the finger ------------------------------------------------------------- */

#define F_MENU VID_GLYPH_X " select     " VID_GLYPH_O " resume"
#define F_SET  VID_GLYPH_X " change   DPAD move/change   " VID_GLYPH_O " back"
#define F_CONN VID_GLYPH_X " choose   " VID_GLYPH_O " back"
#define F_SLOT VID_GLYPH_X " load     " VID_GLYPH_SQ " save     " \
               VID_GLYPH_TRI " delete     " VID_GLYPH_O " back"

static void test_touch(void)
{
   static const char *const LIST_STAR[5] = {
      VID_GLYPH_X " play", "L/R page", "Left: states", VID_GLYPH_SQ " unstar",
      "START settings" };
   char out[PSP_FILE_PATH_CAP];
   int slot = 0, rc, amb;
   fe_console_t con = FE_CONSOLE_GBA;

   g_pcfg.theme = 0;
   game_begin("Pokemon - Emerald Version (USA, Europe).gba", "BPEE",
              FE_CONSOLE_GBA, 1);
   fake_cheats_fill(300);

   /* The menu: a finger lands on Settings (the band goes there at once),
    * lifts (it opens).  Theme by a tap; the list dragged and flicked. */
   menu_open();
   tl_finger(90, 80 + 4 * 24 + 8, 4);
   g_t -= 1;
   tl_shot("70_touch_menu_finger_down");
   tl_wait(8);
   tl_shot("70_touch_settings_open");
   tl_drag(240, 210, 240, 110, 10, 0);
   g_t -= 1;
   tl_shot("70_touch_settings_dragging");
   tl_wait(80);                               /* (a tap on a list still
                                               * coasting only stops it) */
   tl_shot("70_touch_settings_coasted");
   tl_tap(120, 100);                          /* the row there now: Ambient bars */
   tl_wait(2);
   tl_shot("70_touch_settings_tapped");
   tl_tap_footer(F_SET, VID_GLYPH_O " back");
   tl_wait(4);
   tl_shot("70_touch_back_in_menu");
   tl_tap_footer(F_MENU, VID_GLYPH_O " resume");
   tl_wait(4);
   amb = g_pcfg.ambient;
   run_menu(0, NULL);
   EXPECT(g_acts[UI_ACT_RESUME] == 1, "touch: the footer's O resume");
   EXPECT(g_pcfg.ambient != amb, "touch: a tapped Settings row changes");
   g_pcfg.ambient = amb;
   pcfg_display_commit();
   vid_ambient_mode(g_pcfg.ambient);

   /* Play together by touch alone: the page, Connection, Internet, back. */
   menu_open();
   tl_tap(70, 80 + 2 * 24 + 8);              /* Play together */
   tl_tap(100, 96 + 3 * 26 + 8);             /* Connection */
   tl_tap(150, 78 + 38 + 10);                /* Internet */
   tl_wait(2);
   tl_shot("70_touch_connection");
   tl_tap(150, 78 + 10);                     /* Wi-Fi again */
   tl_tap_footer(F_CONN, VID_GLYPH_O " back");
   tl_tap(100, 96 + 1 * 26 + 8);             /* Join a game */
   tl_wait(160);
   tl_finger(200, 68 + 34 + 16, 3);          /* the second room */
   g_t -= 1;
   tl_shot("70_touch_join_finger_down");
   tl_wait(8);
   run_menu(0, NULL);
   EXPECT(g_acts[UI_ACT_NET_JOIN] == 1 && g_joined.id == 0x1005,
          "touch: joined room id %x (acts %d)", g_joined.id,
          g_acts[UI_ACT_NET_JOIN]);
   EXPECT(g_pcfg.net_conn == UI_CONN_WIFI, "touch: connection is %d",
          g_pcfg.net_conn);

   /* Save states: a finger selects a slot and does NOT load it; the
    * footer's hints act; a tap beside a question keeps the slot. */
   menu_open();
   tl_tap(70, 80 + 1 * 24 + 8);              /* Save states */
   tl_wait(10);
   tl_tap(100, 72 + 34 + 12);                /* Slot 2 */
   tl_wait(10);
   tl_shot("70_touch_slot_selected");
   tl_tap_footer(F_SLOT, VID_GLYPH_TRI " delete");
   tl_wait(2);
   tl_shot("70_touch_slot_delete_ask");
   tl_tap(340, 120);                         /* beside the plate: keep */
   tl_wait(2);
   tl_shot("70_touch_slot_kept");
   tl_tap_footer(F_SLOT, VID_GLYPH_O " back");
   tl_tap(40, 80 + 8);                       /* Resume */
   tl_wait(4);
   run_menu(0, NULL);
   EXPECT(!g_acts[UI_ACT_LOADSTATE] && !g_acts[UI_ACT_SAVESTATE] &&
          g_acts[UI_ACT_RESUME] == 1,
          "touch: slots page load=%d save=%d resume=%d",
          g_acts[UI_ACT_LOADSTATE], g_acts[UI_ACT_SAVESTATE],
          g_acts[UI_ACT_RESUME]);

   /* The front panel switched off in config.ini: nothing answers. */
   g_pcfg.touch = 0;
   menu_open();
   tl_tap(40, 80 + 6 * 24 + 8);              /* Exit */
   tl_tap_footer(F_MENU, VID_GLYPH_O " resume");
   tl_wait(2);
   tl_press(PSP_CTRL_CIRCLE);
   tl_wait(2);
   run_menu(0, NULL);
   EXPECT(!g_acts[UI_ACT_EXIT] && g_acts[UI_ACT_RESUME] == 1,
          "touch = 0: exit=%d resume=%d", g_acts[UI_ACT_EXIT],
          g_acts[UI_ACT_RESUME]);
   g_pcfg.touch = 1;

   /* The wake overlay: its second row. */
   tl_reset();
   tl_wait(6);
   tl_finger(80, 168 + 32 + 8, 3);
   g_t -= 1;
   tl_shot("70_touch_wake");
   tl_wait(4);
   g_frame = 0;
   rc = ui_wake_menu(g_snap, g_gw, g_gh,
                     "Pokemon - Emerald Version (USA, Europe).gba");
   EXPECT(rc == 1, "touch: wake overlay's Quit to game list (rc=%d)", rc);
   g_running = 1;

   /* The game list, Shelf: a row selects, the cover starts the game. */
   g_pcfg.ui_shell = 0;
   g_pcfg.console = FE_CONSOLE_GBA;
   pcfg_display_select(FE_CONSOLE_GBA);
   snprintf(g_pcfg.last_rom, sizeof(g_pcfg.last_rom), "%s",
            "Pokemon - Emerald Version (USA, Europe).gba");
   tl_reset();
   tl_wait(50);
   tl_tap_list(LIST_STAR, 5, "Left: states"); /* Emerald's state shelf */
   tl_wait(40);
   tl_shot("71_touch_shelf_states");
   tl_finger(400, 62 + 35 + 10, 3);          /* its second slot */
   g_t -= 1;
   tl_shot("71_touch_shelf_states_finger");
   tl_wait(2);
   tl_quit();
   favourites_reset();
   g_running = 1; g_frame = 0;
   out[0] = '\0';
   slot = 0;
   rc = ui_browser(g_roms, out, sizeof(out), &slot, &con);
   printf("  touch shelf slot rc=%d slot=%d picked=%s\n", rc, slot, out);
   EXPECT(rc == 0 && slot == 2 && strstr(out, "Emerald"),
          "touch: a shelf slot loads it (rc=%d slot=%d)", rc, slot);
   ui_loading_finish(1);
   vid_ambient_drop();
   g_running = 1;

   tl_reset();
   tl_wait(50);
   tl_tap_list(LIST_STAR, 5, "Left: states");
   tl_wait(40);
   tl_tap(120, 120);                         /* beside the drawer: close */
   tl_wait(30);
   tl_shot("71_touch_shelf_states_closed");
   tl_finger(120, 50 + 4 * 33 + 6, 3);       /* two rows below the band */
   g_t -= 1;
   tl_shot("71_touch_shelf_finger_down");
   tl_wait(40);
   tl_shot("71_touch_shelf_selected");
   tl_drag(150, 200, 150, 80, 7, 0);         /* a flick up the list */
   tl_wait(6);
   tl_shot("71_touch_shelf_coasting");
   tl_wait(80);
   tl_shot("71_touch_shelf_rest");
   tl_tap(292, 16);                          /* SELECT's pill: favourites */
   tl_wait(50);
   tl_shot("71_touch_shelf_favourites");
   tl_tap(292, 16);
   tl_wait(40);
   tl_tap(450, 16);                          /* the console card: GB */
   tl_wait(140);
   tl_shot("71_touch_shelf_console");
   tl_tap(396, 120);                         /* the cover: play */
   tl_wait(10);
   tl_quit();
   favourites_reset();
   g_running = 1; g_frame = 0;
   out[0] = '\0';
   rc = ui_browser(g_roms, out, sizeof(out), &slot, &con);
   printf("  touch shelf rc=%d picked=%s\n", rc, out);
   EXPECT(rc == 0 && strstr(out, ".gb") && con == FE_CONSOLE_GB,
          "touch: the cover starts a GB game (rc=%d con=%d)", rc, (int)con);
   g_pcfg.console = FE_CONSOLE_GBA;
   pcfg_display_select(FE_CONSOLE_GBA);
   ui_loading_finish(1);
   vid_ambient_drop();
   g_running = 1;

   /* Marquee: tap the game two rows down, then again to start it. */
   g_pcfg.ui_shell = 1;
   snprintf(g_pcfg.last_rom, sizeof(g_pcfg.last_rom), "%s",
            "Pokemon - Emerald Version (USA, Europe).gba");
   tl_reset();
   tl_wait(50);
   tl_tap(120, 176 + 2 * 24 + 8);
   tl_wait(40);
   tl_shot("71_touch_marquee_selected");
   tl_tap(120, 176 + 8);                     /* the selected row: play */
   tl_wait(10);
   tl_quit();
   favourites_reset();
   g_running = 1; g_frame = 0;
   out[0] = '\0';
   rc = ui_browser(g_roms, out, sizeof(out), &slot, &con);
   printf("  touch marquee rc=%d picked=%s\n", rc, out);
   EXPECT(rc == 0 && strstr(out, "Mystery Dungeon"),
          "touch: marquee tap-tap picks two rows down (rc=%d %s)", rc, out);
   ui_loading_finish(1);
   vid_ambient_drop();
   g_running = 1;
   snprintf(g_pcfg.last_rom, sizeof(g_pcfg.last_rom), "%s",
            "Pokemon - Emerald Version (USA, Europe).gba");
   fake_cheats_fill(0);
}

/* THE WORST BACKDROP.  The menus draw over the game's picture; these are
 * the list pages over the picture that is hardest on each theme -- a white
 * frame under the dark theme's light text, a black one under the light
 * theme's dark text.  check_contrast.py measures what was drawn: the
 * background's level and each text colour's contrast against it. */
static void test_contrast(void)
{
   int theme, i;
   for (theme = 0; theme < 2; theme++)
   {
      const char *tn = theme ? "light" : "dark";
      char n[64];
      g_pcfg.theme = theme;
      game_begin("Pokemon - Emerald Version (USA, Europe).gba", "BPEE",
                 FE_CONSOLE_GBA, 1);
      vid_set_mode(VID_SCALE_STRETCH, VID_FILTER_NEAREST);  /* no bars */
      for (i = 0; i < 256 * 256; i++)
         g_snap[i] = theme ? 0x0000 : 0xFFFF;
      fake_cheats_fill(300);
      g_fch.e[1].enabled = g_fch.e[2].enabled = 1;
      menu_open();
      snprintf(n, sizeof(n), "90_contrast_menu_%s", tn);
      tl_shot(n);
      tl_down(1);
      tl_press(PSP_CTRL_CROSS);          /* Save states */
      tl_wait(12);
      snprintf(n, sizeof(n), "90_contrast_states_%s", tn);
      tl_shot(n);
      tl_press(PSP_CTRL_CIRCLE);         /* (back: the cursor is on Resume) */
      tl_down(2);
      tl_press(PSP_CTRL_CROSS);          /* Play together */
      tl_wait(4);
      snprintf(n, sizeof(n), "90_contrast_play_%s", tn);
      tl_shot(n);
      tl_press(PSP_CTRL_CIRCLE);
      tl_down(3);
      tl_press(PSP_CTRL_CROSS);          /* Cheats */
      tl_wait(4);
      snprintf(n, sizeof(n), "90_contrast_cheats_%s", tn);
      tl_shot(n);
      tl_press(PSP_CTRL_CIRCLE);         /* (back: the cursor is on Cheats) */
      tl_down(1);
      tl_press(PSP_CTRL_CROSS);          /* Settings */
      tl_wait(4);
      snprintf(n, sizeof(n), "90_contrast_settings_%s", tn);
      tl_shot(n);
      tl_press(PSP_CTRL_CIRCLE);
      tl_press(PSP_CTRL_CIRCLE);
      tl_wait(4);
      run_menu(0, NULL);
   }
   g_pcfg.theme = 0;
   vid_set_mode(VID_SCALE_INT3, VID_FILTER_NEAREST);
}

static void test_wake_and_return(void)
{
   int theme;
   for (theme = 0; theme < 2; theme++)
   {
      g_pcfg.theme = theme;
      game_begin("Pokemon - Emerald Version (USA, Europe).gba", "BPEE",
                 FE_CONSOLE_GBA, 1);
      vid_set_mode(VID_SCALE_FIT, VID_FILTER_SHARP);
      tl_reset();
      tl_wait(4);
      tl_shot(theme ? "40_wake_light" : "40_wake_dark");
      tl_press(PSP_CTRL_DOWN);
      if (!theme) tl_shot("40_wake_quit_selected");
      tl_press(PSP_CTRL_CIRCLE);
      g_frame = 0;
      ui_wake_menu(g_snap, g_gw, g_gh,
                   "Pokemon - Emerald Version (USA, Europe).gba");
      g_running = 1;
   }
   g_pcfg.theme = 0;
   /* "Quit to game list": the hold, then the crossfade into the browser. */
   vid_set_mode(VID_SCALE_INT3, VID_FILTER_NEAREST);
   ui_set_backdrop(g_snap, g_gw, g_gh);
   tl_reset();
   g_frame = 0;
   tl_shot("41_return_hold");
   ui_return_hold();
   g_pcfg.ui_shell = 1;
   tl_reset();
   tl_wait(6);
   tl_shot("41_return_crossfade");
   tl_wait(30);
   tl_quit();
   browser_run();
}

static void test_pictures(void)
{
   static const int scales[5] = { VID_SCALE_1X, VID_SCALE_INT3, VID_SCALE_FIT,
                                  VID_SCALE_INT2, VID_SCALE_STRETCH };
   static const char *const sname[5] = { "2x", "3x", "fit", "4xcrop",
                                         "stretch" };
   static const char *const fname[4] = { "nearest", "bilinear", "sharp",
                                         "lcd" };
   static uint16_t gba[240 * 160], gb[160 * 144];
   char n[64];
   int s, f;

   tl_reset();
   osd_reset();
   pattern(gba, 240, 160, "GBADHOC 240x160 TEST FRAME");
   pattern(gb, 160, 144, "GB 160x144 FRAME");
   vid_ambient_mode(0);
   for (s = 0; s < 5; s++)
      for (f = 0; f < VID_FILTER_MODES; f++)
      {
         vid_rect_t r;
         vid_set_mode(scales[s], f);
         vid_picture_rect(240, 160, &r);
         snprintf(n, sizeof(n), "50_pic_gba_%s_%s", sname[s], fname[f]);
         picture_shot(n, gba, 240, 160);
         if (f == 0)
            printf("  gba %-7s -> %dx%d at %d,%d\n", sname[s], r.w, r.h, r.x,
                   r.y);
      }
   for (s = 0; s < 5; s++)
      for (f = 0; f < VID_FILTER_MODES; f++)
      {
         vid_rect_t r;
         vid_set_mode(scales[s], f);
         vid_picture_rect(160, 144, &r);
         snprintf(n, sizeof(n), "51_pic_gb_%s_%s", sname[s], fname[f]);
         picture_shot(n, gb, 160, 144);
         if (f == 0)
            printf("  gb  %-7s -> %dx%d at %d,%d\n", sname[s], r.w, r.h, r.x,
                   r.y);
      }

   /* Ambient bars: the game's art, a game frame, the palette. */
   scene(g_game, 240, 160, 2);
   scene(g_game_b, 240, 160, 5);
   vid_set_mode(VID_SCALE_INT3, VID_FILTER_NEAREST);
   vid_ambient_fallback(ui_console_shade(FE_CONSOLE_GBA));
   vid_ambient_mode(PCFG_AMB_ART);
   picture_shot("52_ambient_palette_3x", g_game, 240, 160);
   {
      /* Art: what the browser's pick leaves baked (test_loading ran). */
      static uint16_t art[512 * 272];
      int x, y;
      for (y = 0; y < 272; y++)
         for (x = 0; x < 480; x++)
         {
            unsigned r = (unsigned)(40 + x * 180 / 480);
            unsigned g = (unsigned)(140 - y * 80 / 272);
            unsigned b = (unsigned)(90 + ((x / 40 + y / 40) & 1) * 60);
            art[y * 512 + x] = (uint16_t)((r >> 3) | ((g >> 2) << 5) |
                                          ((b >> 3) << 11));   /* PSP order */
         }
      vid_ambient_bake_art(art, 512, 480, 272, 1);
   }
   picture_shot("52_ambient_art_3x", g_game, 240, 160);
   vid_set_mode(VID_SCALE_FIT, VID_FILTER_SHARP);
   picture_shot("52_ambient_art_fit", g_game, 240, 160);
   vid_ambient_drop();
   vid_ambient_mode(PCFG_AMB_ART_GAME);
   {
      int i;
      for (i = 0; i < 200; i++)       /* AMB_SNAP_AFTER presented frames */
      {
         vid_draw_frame(g_game, 240, 160, 480);
         vid_swap();
      }
   }
   vid_set_mode(VID_SCALE_INT3, VID_FILTER_NEAREST);
   picture_shot("52_ambient_game_3x", g_game, 240, 160);

   /* Two pictures. */
   vid_ambient_mode(0);
   for (s = 0; s < VID_PAIR_LAYOUTS; s++)
   {
      vid_rect_t a, b;
      vid_pair_rects(240, 160, s, &a, &b);
      printf("  pair gba layout %d: %dx%d at %d,%d + %dx%d at %d,%d\n", s,
             a.w, a.h, a.x, a.y, b.w, b.h, b.x, b.y);
      vid_draw_frame_pair(g_game, g_game_b, 240, 160, 480, s);
      shot_now(s ? "53_pair_gba_inset" : "53_pair_gba_side");
      vid_swap();
   }
   {
      static uint16_t ga[160 * 144], gb2[160 * 144];
      scene(ga, 160, 144, 1);
      scene(gb2, 160, 144, 4);
      for (s = 0; s < VID_PAIR_LAYOUTS; s++)
      {
         vid_rect_t a, b;
         vid_pair_rects(160, 144, s, &a, &b);
         printf("  pair gb  layout %d: %dx%d at %d,%d + %dx%d at %d,%d\n", s,
                a.w, a.h, a.x, a.y, b.w, b.h, b.x, b.y);
         vid_draw_frame_pair(ga, gb2, 160, 144, 320, s);
         shot_now(s ? "53_pair_gb_inset" : "53_pair_gb_side");
         vid_swap();
      }
      /* With the bars lit, and with the test pattern for pixel checks. */
      vid_ambient_mode(PCFG_AMB_ART);
      vid_draw_frame_pair(gba, gba, 240, 160, 480, VID_PAIR_SIDE);
      osd_chip_session("Room 7  P1 + P2");
      osd_draw();
      shot_now("53_pair_gba_side_pattern");
      vid_swap();
      osd_reset();
   }
   vid_ambient_mode(0);
}

int main(int argc, char **argv)
{
   char cfg[400];
   const char *only = argc > 3 ? argv[3] : "";

   snprintf(g_root, sizeof(g_root), "%s", argc > 1 ? argv[1] : "vita_root");
   snprintf(g_out, sizeof(g_out), "%s", argc > 2 ? argv[2] : "shots_all");
   mkdir(g_out, 0755);
   pspcompat_set_root(g_root);
   pspcompat_make_dirs();
   snprintf(g_roms, sizeof(g_roms), "%s/roms", pspcompat_root());

   fe_evt_init(NULL, 0);
   snprintf(cfg, sizeof(cfg), "%s/config.ini", pspcompat_root());
   pcfg_load(cfg);
   vid_init();
   vid_set_mode(g_pcfg.scale, g_pcfg.filter);
   ui_set_theme_black(0);
   pspcompat_host_pad(tl_pad);
   pspcompat_host_vblank_hook(tl_vblank);
   pspcompat_host_touch(tl_touch);
   pspcompat_host_ime(ime_script);
   sim_reset();
   printf("backend=%s root=%s default scale=%s filter=%s\n",
          vid_backend_name(), pspcompat_root(), vid_scale_name(g_pcfg.scale),
          vid_filter_name(g_pcfg.filter));

   /* `ui_host_test <root> <out> measure`: the width of each line of stdin
    * in both faces, logical px -- for fitting a sentence to a page. */
   if (!strcmp(only, "measure"))
   {
      char line[400];
      while (fgets(line, sizeof(line), stdin))
      {
         line[strcspn(line, "\n")] = '\0';
         printf("%4d  hd %4d  %s\n", vid_text_w(line), vid_text_hd_w(line),
                line);
      }
      return 0;
   }
#define RUN(name, fn) \
   if (!only[0] || strstr(only, name)) { printf("== %s\n", name); fn(); }
   RUN("text", test_text)
   RUN("browser", test_browser)
   RUN("loading", test_loading)
   RUN("osd", test_game_osd)
   RUN("menu", test_menu)
   RUN("play", test_play)
   RUN("cheats", test_cheats)
   RUN("touch", test_touch)
   RUN("light", test_light)
   RUN("contrast", test_contrast)
   RUN("wake", test_wake_and_return)
   RUN("pictures", test_pictures)

   pcfg_save();
   vid_term();
   printf("%d screenshots in %s, %d expectation(s) failed\n", g_shots, g_out,
          g_fail);
   return g_shots > 0 && !g_fail ? 0 : 1;
}
