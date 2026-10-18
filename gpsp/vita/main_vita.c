/* main_vita.c -- GBAdhoc on the PS Vita: the application.
 *
 * The Vita's counterpart of psp/main_psp.c.  It owns the session:
 *
 *   boot  ->  [ game list  ->  load  ->  game loop  ->  unload ]*  ->  exit
 *
 * and everything a single console does in it: the in-game menu, save states
 * in five slots with their previews, battery saves, the control map, the
 * video presets, fast forward, the OSD, screenshots, the pause and
 * wake-from-suspend overlays, and going back to the game list in this
 * process, as many times as the player likes.
 *
 * WHAT IT IS BUILT FROM.  The screens are the PSP frontend's own
 * (psp/ui_psp.c, osd_psp.c, config_psp.c, ctl_map.c, compiled -DVITA) drawn
 * through vita/video_vita.c; the cores sit behind frontend-common/fe_host.
 * This file is the loop between them.  psp/main_psp.c is its reference for
 * behaviour -- the order of things in a frame, what each menu action does --
 * and four fifths of that file are absent here because the Vita has no use
 * for them: the PSP's memory budget and its relaunch tricks, the Media
 * Engine, the measurement rigs.
 *
 * WHAT IT DOES NOT OWN.  Linking (app_vita.h: link_vita.c for the network,
 * local_vita.c for two players on this console) and cheats (cheats_vita.h:
 * cheats_vita.c) are other files.  They are called here at every point of
 * the session where they have work to do -- app_vita.h has the order, and
 * the two link files are never called from anywhere else, so writing them
 * changes nothing here.  Until they exist, stubs_vita.c answers.
 *
 * BESIDE THE CONTROL MAP (which is the PSP's: buttons and the left stick):
 * the right stick pulled down is the cheat cartridge's button; held LEFT it
 * rewinds (as the Rewind shortcut does: rewind_vita.h) and held RIGHT it
 * fast-forwards for as long as it is held, whatever the fast-forward
 * button's mode -- time, on one stick; in the local link, where neither
 * exists, a sideways flick switches machines instead.  A short tap on the
 * game's picture opens the menu (config.ini touch = 0 and vita.ini
 * touch_menu = 0 both switch that off) or, in the local link, goes to
 * link_local_tap.  The menus read the touch screen themselves.
 *
 * PORTABILITY: there is no Sony call in this file.  The machine is behind
 * plat.h, so the same source builds for Linux against hostcore/plat_host.c
 * and runs headless there (x86-64, and armhf under qemu-arm with the ARM
 * dynarec the Vita uses) -- which is where its logic is tested.
 *
 *   ux0:data/GBAdhoc/           (the PSP's layout: a PSP folder copies over)
 *     roms/         cartridges (.gba .gb .gbc), in folders if you like, and
 *                   BESIDE each one its files, named as the PSP names them:
 *                     Game.sav   Game.gb.sav   Game.gbc.sav     battery
 *                     Game.st0 .. .st4  (.gb.st0, .gbc.st0)     save states
 *                     <state>.thumb  <state>.shot               previews
 *     boxart/ hero/ artwork for the game list
 *     cheats/       <rom name>.cht
 *     screenshots/  <rom name>-<date>-<time>.png
 *     log/          gbadhoc.log (EVT lines; the validation tools grep these)
 *     config.ini    the settings (psp/config_psp.h)
 *     gba_bios.bin  optional; without it the built-in open BIOS is used
 *     autoboot.txt  optional, one line: a ROM under roms/ or a full path;
 *                   the first game starts without the game list
 *     vita.ini      optional test switches: the g_sw struct below
 */
#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <sys/stat.h>

#include "fe_host.h"
#include "../gbcore/gbcore.h"   /* gbcore_set_color_correction */
#include "fe_gbadual.h"
#include "fe_evt.h"
#include "fe_util.h"
#include "link_games.h"

#define PSPCOMPAT_IMPL            /* the declarations, not the name remapping */
#include "pspcompat.h"
#include "ui_psp.h"
#include "osd_psp.h"
#include "config_psp.h"
#include "ctl_map.h"
#include "video_vita.h"
#include "render_thread.h"
#include "ui_host_hooks.h"
#include "state_slots.h"
#include "rom_paths.h"
#include "ff_gate.h"
#include "stb_image_write.h"

#include "plat.h"
#include "audio_vita.h"
#include "app_vita.h"
#include "mgift_vita.h"
#include "mgift_net.h"       /* MGNET_LISTEN_PORT */
#include "cheats_vita.h"
#include "rewind_vita.h"
#include "clip_vita.h"
#include "gamedb.h"
#include "artfetch.h"
#include "net_vita.h"
#include "../wide.h"
#include "vstyle.h"
#include "../gbalink/gbalink_script.h"

/* The control map's bits are the PSP's pad bits (psp/ctl_map.h says so of
 * the PSP SDK; pspcompat.h is that SDK here). */
typedef char app_pad_bits_agree[(CTL_SELECT == PSP_CTRL_SELECT &&
                                 CTL_START == PSP_CTRL_START &&
                                 CTL_CROSS == PSP_CTRL_CROSS &&
                                 CTL_CIRCLE == PSP_CTRL_CIRCLE) ? 1 : -1];

/* ---- test switches (vita.ini) -------------------------------------------- */

/* PACING.  The GBA draws 59.7275 frames a second; the Vita's panel refreshes
 * at 60.00 Hz (59.94 on the OLED model).  Two honest ways to reconcile them:
 *
 *   PACE_VSYNC  one emulated frame per refresh.  Every refresh shows a new
 *               frame, so scrolling is perfectly even; the game runs 0.36 to
 *               0.46 % fast and the sound is resampled by the same amount
 *               (audio_vita.c's rate follow), which nobody can hear.  The
 *               PSP frontend is refresh-locked too, but sheds the surplus
 *               sound by waiting an extra refresh every few seconds (its
 *               audio-ring nudge), which shows as a hitch.
 *   PACE_TIMER  frames timed against the clock at exactly 59.7275 Hz.  The
 *               speed is exact, and once every 3.7 s (4.7 s on an OLED) a
 *               refresh passes with no new frame: a visible judder in any
 *               scrolling game.
 *
 * VSYNC is the default because the judder is visible and the speed-up is
 * not.  TIMER stays one switch away for anything that needs the exact rate
 * (and as the fallback if a display mode reports a refresh we do not
 * expect).  Link sessions are lockstep and do not care: a console that is
 * ahead simply waits for its partner's input. */
#define PACE_VSYNC 0
#define PACE_TIMER 1

static struct
{
   int      pace_mode;        /* pace_mode = 0 | 1                             */
   int      drc;              /* drc = 0: interpreter (first hardware bring-up:
                               * rules the dynarec in or out of a crash)       */
   unsigned autoexit_frames;  /* autoexit_frames = N: leave the app after N
                               * emulated frames of a game                     */
   int      link_bench;       /* link_bench = 1: fe_host's one-console bench
                               * pair (GBA-LINK-MIRROR.md 4): from the next
                               * state LOAD on, the partner's machine
                               * (instance B) runs beside the player's with
                               * the cable between them and the player's own
                               * buttons, and both machines' per-frame hashes
                               * go to log/linkhash.txt.  It is how instance
                               * B is exercised before any network exists.  */
   int      io_thread;        /* io_thread = 0: no worker thread; the log is
                               * flushed line by line on the emulation thread
                               * (nothing is lost if the app dies) and the
                               * save is written from it too                */
   int      touch_menu;       /* touch_menu = 0: a tap on the game's picture
                               * does not open the in-game menu (config.ini
                               * touch = 0 switches every touch off)        */
   int      up_bench;         /* up_bench = N: time every upscaler on the
                               * frame on screen, N times (upscale_vita.h)  */
   int      wide;             /* wide = 0..4: gpsp/wide.h's WIDE_* in place
                               * of the Settings row; + 8: the strips are
                               * checked against the picture (selftest);
                               * + 16: a log line for every look ahead    */
   int      wide_fill;        /* wide_fill = 0 / 1 / 2 in place of the row
                               * (bars, soft edges, stretch)             */
   int      xbrz_dist;        /* xbrz_dist = 0 / 1 / 2: how xBRZ compares
                               * colours (large table, compact, arithmetic) */
   int      vm_probe;         /* vm_probe = 1 / 2 / 3: at start, ask the
                               * console whether a second thread may write
                               * and run translated code; the answer goes to
                               * log/vm_probe.txt (plat.h).  May end the app:
                               * take the line out again afterwards         */
   int      pad_trace;        /* pad_trace = N: log the first N changes of
                               * the pad as the console reports it, and
                               * what the game was given (a console driven
                               * from afar: did the presses arrive?)      */
   int      keys_script;      /* keys_script = 1: the first game of the run
                               * is also given the buttons of keys.txt in
                               * the data folder, frame by frame
                               * (gbalink_script.h's grammar: wait N,
                               * press KEY [N], hold KEY N, dump = the
                               * app's screenshot, evt TEXT) -- a measured
                               * run that reaches the same place each time,
                               * with nobody at the console               */
   int      clips;            /* clips = 0: no preview is recorded, whatever
                               * Settings say (clip_vita.h)               */
   int      up_slide;         /* up_slide = 0 / 1 / 2: how the upscalers
                               * follow a scrolled picture
                               * (upscale_slide_mode; 2 the default)      */
   int      main_cpu;         /* main_cpu = 0..2: keep the emulator's thread
                               * on that core; -1 leave it to the system */
   int      drain;            /* drain = 0 / 1 / 2: a frame starts only once
                               * the last picture is on the glass (1, the
                               * default) / taken by the display queue (2) /
                               * at once (0, as before build 18): pace_frame */
   int      art;              /* art = 0: no box art is fetched (artfetch.h) */
   char     art_url[128];     /* art_url = http://host[:port]: where from
                               * (the twin fetches only when this is set) */
   int      tex_mem;          /* tex_mem = 1: the textures rewritten every
                               * frame in the main RAM, uncached (measured
                               * on a console: no faster to write, and
                               * 80 ms to make; video_vita.h)            */
   int      fade_drop;        /* fade_drop = 0..2: upscale_fade_drop    */
   int      render_thread;    /* render_thread = 1: the GBA's picture is
                               * drawn on another core (render_thread.h,
                               * the default); 2: drawn both ways and
                               * compared (the log's vita_render line);
                               * 0: on the emulator's thread, as before */
   int      core_phase;       /* core_phase = 1..3: the core's own split of
                               * a frame (cpu / vid = the GBA's picture,
                               * line by line / jit / sound), `EVT
                               * core_phase` every ten seconds (main.h has
                               * what each level costs); 0 = off        */
   int      gpu_xbrz;         /* gpu_xbrz = 1: xBRZ drawn by the GPU where
                               * it can be (video_vita.c, "xBRZ on the
                               * GPU"); 0 = the CPU's cores, the default:
                               * on the console the GPU took 31 ms a
                               * frame for it                            */
   int      gpu_time;         /* gpu_time = 1: each present waits for the
                               * GPU to finish, and the log's vita_gpu
                               * line says how long (measuring only)     */
   int      gpu_probe;        /* gpu_probe = 1: at start, compile test
                               * programs and log what the shader
                               * compiler says (`gpu_probe` lines); N >=
                               * 2: what GPU xBRZ's first passes left in
                               * their targets at its Nth picture
                               * (`gpu_rt` lines)                       */
   int      gpu_view;         /* gpu_xbrz_view = 1/2: the screen shows GPU
                               * xBRZ's passes 1/0 (video_vita.c)       */
   /* FASTER LINK PLAY (GBAdhoc 4.0; the PSP's PLAY_* defaults): the GBA
    * Link Cable's fast path with the lean hooks (gba_link_fast = 0..3; 0 =
    * every bus walk, as up to build 19) and a network session's partner
    * machine mixing no sound at all (gba_link_headless_audio = 2; 1 = mixed
    * and dropped, 0 = mixed).  Both consoles of a session agree by
    * construction: the switches change the cost, not the machine. */
   int      gba_link_fast;
   int      gba_link_headless_audio;
   int      art_titles;       /* art_titles = 0: box art only, no title
                                 screens (artfetch_titles)              */
} g_sw = { PACE_VSYNC, 1, 0, 0, 1, 1, 0, -1, -1, 0, 0, 0, 0, 1, 2, -1, 1, 1,
           "", 0, 2, 1, 0, 0, 0, 0, 0, 3, 2, 1 };

/* vita.ini core_phase: the core's per-phase split of a frame (main.h,
 * "Phase 5h").  Declared here, not through the core's main.h (the whole
 * emulator header set), as main_psp.c does. */
extern unsigned int (*smc_prof_clock)(void);
extern unsigned int wide_prof_hook_us, wide_prof_strip_us, wide_prof_rest_us;
void core_phase_set_level(unsigned lvl);
void core_phase_take(unsigned *lvl, unsigned *clk_ns, unsigned *frames,
                     unsigned *reads, unsigned *mean, unsigned *max,
                     unsigned *worst, unsigned nph,
                     unsigned *bkr, unsigned *bkw, unsigned *wbkr,
                     unsigned *wbkw, unsigned *ugba, unsigned *dman,
                     unsigned *stmr, unsigned *negc, unsigned *nego,
                     unsigned *rfux, unsigned *rfut,
                     unsigned *wrfux, unsigned *wrfut);

static unsigned int app_prof_clock(void)
{
   return (unsigned int)plat_time_us();
}

static void app_core_phase(unsigned *lvl, unsigned *clk_ns, unsigned *frames,
                           unsigned *reads, unsigned *mean, unsigned *max,
                           unsigned *worst, unsigned nph,
                           unsigned *bkr, unsigned *bkw, unsigned *wbkr,
                           unsigned *wbkw, unsigned *ugba, unsigned *dman,
                           unsigned *stmr, unsigned *negc, unsigned *nego,
                           unsigned *rfux, unsigned *rfut,
                           unsigned *wrfux, unsigned *wrfut)
{
   core_phase_take(lvl, clk_ns, frames, reads, mean, max, worst, nph,
                   bkr, bkw, wbkr, wbkw, ugba, dman, stmr, negc, nego,
                   rfux, rfut, wrfux, wrfut);
}

/* The GBA's frame period: 280896 cycles of a 16.777216 MHz clock. */
#define GBA_FRAME_NS   16742706ull
#define GBA_FPS_X1000  59727.5

/* A frame this many refreshes behind is not chased: the debt is forgiven.
 * Without it a console that was genuinely too slow for a moment would sprint
 * afterwards, which looks worse than having been late. */
#define PACE_RESYNC_VB 3

/* CATCHING UP (Settings > Keep full speed, config.ini full_speed).  A frame
 * that overran by a whole refresh or more leaves the game that many frames
 * behind the display -- and behind its own sound, which the port goes on
 * playing: the ring drained, and a slow moment (the recompiler meeting a new
 * scene's code: 30-45 ms frames on a console) was a break in the music.  So
 * pace_frame notes the debt and the next iteration runs those frames first,
 * emulated in full but neither drawn nor presented (the core's frameskip;
 * no present means no wait for the display), then the frame it draws.  The
 * game's time and its sound keep full speed; the picture skips what it had
 * to.  At most CATCHUP_PER_FRAME a refresh, so the picture never stops for
 * long; a debt beyond CATCHUP_HORIZON is a stall (a load, the system's own
 * pause), not a slow frame, and is forgiven as before.  Not in fast forward,
 * a link of either kind (their timing is the link layer's), the bench pair,
 * rewind or a menu. */
#define CATCHUP_PER_FRAME 2
#define CATCHUP_HORIZON   8
static unsigned g_catchup;        /* frames owed (pace_frame)              */
/* The display queue at the start of a frame (vb_present_state), this
 * window: frames whose last picture was not yet taken / not yet shown, and
 * the refreshes waited for it (drain). */
static unsigned g_q_taken, g_q_glass, g_q_waits;
static int      g_catchup_ok;     /* this iteration may run them           */
static unsigned g_caught, g_caught_us;   /* this window's: run, their time */

/* START+SELECT held this many refreshes opens the in-game menu (a quarter of
 * a second, as on the PSP).  The PSP's longer hold that ends the run is not
 * here: it exists there because HOME is unreliable under the PSP's harness,
 * and on the Vita the PS button always works. */
#define MENU_CHORD_FRAMES 15
#define MENU_CHORD_US     240000u   /* ...or this long, at a low frame rate */

/* How far into a 16.7 ms frame the emulator's thread still starts a stripe
 * of upscaling in its idle time (a stripe of xBRZ is 1 to 2.5 ms on a
 * console). */
#define HELP_UNTIL_US     11000u
/* ...and how far into it the wide view's scout still starts a frame of the
 * second machine.  The scout is worth little unless it runs faster than the
 * player walks -- two or three of its frames in one of ours (fe_gbadual.c)
 * -- and this is what decides that.  On the console (9 Oct 2026, build 12)
 * a frame of it was about 1.1 ms in a light game, and with the old limit of
 * 8 ms while xBRZ was on it got one frame in each of ours: never ahead.
 * The upscalers run on their own threads; what the main thread lends them
 * after this is a bonus (HELP_UNTIL_US). */
#define SCOUT_UNTIL_US       12500u

/* The right stick counts as pushed this far from its centre (of 128). */
#define RSTICK_PUSH 80
/* ...and held LEFT for this many refreshes it rewinds, held RIGHT it
 * fast-forwards. */
#define RSTICK_REWIND_HOLD 6
/* ctl_shortcuts' bits are the control map's; this one is the loop's own:
 * fast forward asked for by the stick (ff_frame). */
#define FIRE_FF_STICK 0x10000u

/* ---- what app_vita.h publishes --------------------------------------------- */

app_game g_game;
char     g_app_root[APP_PATH_CAP];
char     g_app_rom_dir[APP_PATH_CAP];
char     g_app_config[APP_PATH_CAP];
unsigned g_app_pad;
uint32_t g_app_keys;
uint64_t g_app_frame_t0;
unsigned g_app_vc_target;
int      g_app_drew;

/* ui_host_hooks.h: 1 while the application runs.  Every modal screen of the
 * UI loops on it. */
volatile int g_running = 1;

/* ---- state ----------------------------------------------------------------- */

/* Why the game loop ended. */
enum { QUIT_NONE = 0, QUIT_GAMELIST, QUIT_EXIT };
static int  g_quit;
static char g_quit_why[96];            /* app_quit_game's reason, for a notice */

static int  g_game_loaded;             /* fe_host holds a game                */
static int  g_audio_ok;
static int  g_worker;                  /* the I/O worker thread is running    */

static uint64_t g_next_ns;             /* PACE_TIMER: this frame's deadline   */
static unsigned g_pace_late, g_pace_forgiven;

/* The frame the core last presented (libretro RGB565; the core's own buffer,
 * valid until the game is unloaded or a session takes the cartridge). */
static struct { const uint16_t *pix; unsigned w, h; size_t pitch; } g_last;
/* The same frame as it is SHOWN: with the wide picture's margins when the
 * core hands them over (gpsp/wide.c).  g_last is then the console's own
 * 240 pixels inside it -- what a state's picture, the menu's backdrop, the
 * pair's two pictures and the upscaler bench are made from. */
static struct { const uint16_t *pix; unsigned w, h; size_t pitch; } g_show;
static int g_wide_mode;                /* what the core was last told       */
static int g_wide_fill = -1;

/* The frame the player was looking at, as the UI wants it: PSP-order 565 at
 * the top-left of a 256x256 texture.  The menu's backdrop, the pause and
 * wake overlays, and the picture the return to the game list fades from. */
#define SNAP_TEX 256
static uint16_t g_snap[SNAP_TEX * SNAP_TEX];
static unsigned g_snap_w, g_snap_h;
static int      g_snap_have;
/* The Save states page's five thumbnails (ui_set_thumb_buffer). */
static uint16_t g_thumbs[PSP_STATE_SLOT_COUNT * 64 * 64];

static unsigned g_prev_pad;
/* Buttons that answered a screen -- the X that picked the game, the O that
 * closed the menu, the choice on the pause overlay -- are not the game's:
 * they are kept from it until they are let go. */
static unsigned g_pad_block;
static unsigned g_chord_frames;        /* START+SELECT held                   */
static uint64_t g_chord_t0;            /* ...since                            */
static int      g_menu_was_open;
/* Why main_vita.c itself refused a Host or a local start (the two link
 * layers exclude each other here, so neither has to know the other): shown
 * by the page that asked, through the view, until the next action. */
static const char *g_view_error;
/* Whose reason the view may carry: the part the last start went to.  A link
 * file may go on reporting why its last start failed for as long as it
 * likes; a reason older than the last action is not shown. */
enum { ERR_NONE, ERR_NET, ERR_LOCAL };
static int g_view_err_part;
/* The finger, while the menu is closed: a short touch that does not move is
 * a tap.  `wait` = ignore the panel until it is released (the touch that
 * closed a menu is not a tap on the game). */
static struct { int down, wait, moved, frames, x, y; } g_tap;
static int      g_rs_side;             /* the right stick is held sideways    */
static unsigned g_rs_left;             /* ...to the left, for this many refreshes */
static unsigned g_rs_right;            /* ...to the right                         */
static int      g_cheat_btn;           /* ...or down: the cheat button        */
static int      g_wake_menu;           /* show the pause / wake overlay       */
static int      g_shot_pending;        /* the screenshot chord was pressed    */
static int      g_autoload_slot;       /* a state picked on the list's shelf  */
static unsigned g_faults_seen;

/* --- fast forward (docs/FF-PRESETS.md; psp/main_psp.c is the reference) ----
 * 3x batches three emulated frames into a refresh and draws the last.
 * Unlimited and Unlimited Smooth run without waiting for the display and
 * DRAW at most 30 / 60 frames a second (the draw gate, psp/ff_gate.h): a
 * frame the gate turns away is emulated but never rendered. */
static struct
{
   int       on;          /* engaged                                         */
   int       toggled;     /* toggle mode's latch                             */
   int       mode;        /* PCFG_FF_* it was engaged with                   */
   int       mult;        /* 3x: three frames per refresh                    */
   int       uncapped;    /* unlimited / smooth: no pacing                   */
   int       locked_told; /* the "off during a link" toast was shown         */
   ff_gate_t gate;
   /* since it was engaged, for EVT ff_stats */
   unsigned  frames0, vc0, drawn0;
   uint64_t  t0;
} g_ff;
static int g_blit_suppress;            /* 3x: an intermediate frame, not drawn */

/* FPS chip / telemetry counters, bumped in the video sink. */
static unsigned g_fps_emu, g_fps_drawn, g_drawn_total;
static uint64_t g_fps_t0;

/* A toast that replaces the one on screen (with the OSD, further down). */
static void toast_now(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

/* ---- small helpers --------------------------------------------------------- */

static unsigned long long evt_clock(void)
{
   return (unsigned long long)plat_time_us();
}

/* snprintf that says when the result did not fit, so no path is ever used
 * truncated (a truncated save path is somebody else's save). */
static int path_fmt(char *out, size_t cap, const char *fmt, ...)
{
   va_list ap;
   int n;
   va_start(ap, fmt);
   n = vsnprintf(out, cap, fmt, ap);
   va_end(ap);
   if (n < 0 || (size_t)n >= cap)
   {
      out[0] = '\0';
      return -1;
   }
   return 0;
}

/* Where gba_bios.bin is: the player's own file in the data root wins, then
 * one packed into the application (a private build may carry its owner's
 * dump: make vpk BIOS=...), else the data root again -- where the core
 * finds nothing and uses its built-in replacement.  Looked up at every
 * boot, so a file copied in between two games is seen. */
const char *app_bios_dir(void)
{
   static char dir[APP_PATH_CAP];
   static const char *found;
   char path[APP_PATH_CAP + 16];
   const char *app = plat_app_dir();
   const char *was = found;
   FILE *f;

   snprintf(dir, sizeof(dir), "%s", g_app_root);
   found = "none";
   if (path_fmt(path, sizeof(path), "%s/gba_bios.bin", g_app_root) == 0 &&
       (f = fopen(path, "rb")) != NULL)
   {
      fclose(f);
      found = "data";
   }
   else if (app && app[0] &&
            path_fmt(path, sizeof(path), "%s/gba_bios.bin", app) == 0 &&
            (f = fopen(path, "rb")) != NULL)
   {
      fclose(f);
      snprintf(dir, sizeof(dir), "%s", app);
      found = "app";
   }
   if (found != was)
      fe_evt("bios_file where=%s dir=%s", found, dir);
   return dir;
}

static int file_size(const char *path, long *size)
{
   struct stat st;
   if (stat(path, &st) != 0 || !S_ISREG(st.st_mode))
      return -1;
   if (size)
      *size = (long)st.st_size;
   return 0;
}

static int has_ext(const char *name, const char *ext)
{
   size_t n = strlen(name), e = strlen(ext);
   return n > e && strcasecmp(name + n - e, ext) == 0;
}

/* A cartridge started without the game list is the console its extension
 * says; one picked in the list is the console the player was browsing. */
static fe_console_t console_for(const char *path)
{
   if (has_ext(path, ".gbc")) return FE_CONSOLE_GBC;
   if (has_ext(path, ".gb"))  return FE_CONSOLE_GB;
   return FE_CONSOLE_GBA;
}

static const char *console_name(fe_console_t c)
{
   return c == FE_CONSOLE_GBC ? "GBC" : c == FE_CONSOLE_GB ? "GB" : "GBA";
}

static const char *base_name(const char *path)
{
   const char *s = strrchr(path, '/');
   const char *c = strrchr(path, ':');
   if (c && (!s || c > s))
      s = c;
   return s ? s + 1 : path;
}

/* A plat pad in the PSP's bit layout, the left stick folded into the d-pad:
 * what pspcompat_pad() makes of the first pad, for a second one. */
static unsigned pad_psp_bits(const plat_pad *p)
{
   unsigned b = p->buttons & CTL_ALL;
   int dx = (int)p->lx - 128, dy = (int)p->ly - 128;
   if (dx < -PSPCOMPAT_STICK_DEADZONE) b |= PSP_CTRL_LEFT;
   if (dx >  PSPCOMPAT_STICK_DEADZONE) b |= PSP_CTRL_RIGHT;
   if (dy < -PSPCOMPAT_STICK_DEADZONE) b |= PSP_CTRL_UP;
   if (dy >  PSPCOMPAT_STICK_DEADZONE) b |= PSP_CTRL_DOWN;
   return b;
}

/* ---- the data root ---------------------------------------------------------- */

static int roots_init(void)
{
   static const char *const sub[] = { "cheats", "screenshots", "scenes",
                                      "clips" };
   char path[APP_PATH_CAP];
   size_t i;

   if (path_fmt(g_app_root, sizeof(g_app_root), "%s", plat_data_root()) != 0)
      return -1;
   /* The UI derives every path from pspcompat's root, which is shorter
    * than ours: a root that does not fit there is not usable. */
   pspcompat_set_root(g_app_root);
   if (strcmp(pspcompat_root(), g_app_root) != 0)
      return -1;
   if (path_fmt(g_app_rom_dir, sizeof(g_app_rom_dir), "%s/roms",
                g_app_root) != 0 ||
       path_fmt(g_app_config, sizeof(g_app_config), "%s/config.ini",
                g_app_root) != 0)
      return -1;
   pspcompat_make_dirs();          /* roms boxart hero saves log */
   for (i = 0; i < sizeof(sub) / sizeof(sub[0]); i++)
      if (path_fmt(path, sizeof(path), "%s/%s", g_app_root, sub[i]) == 0)
         mkdir(path, 0777);        /* already there is the normal case */
   return 0;
}

static void read_switches(void)
{
   char ini[APP_PATH_CAP];
   path_fmt(ini, sizeof(ini), "%s/vita.ini", g_app_root);
   g_sw.pace_mode       = (int)fe_ini_get_int(ini, "pace_mode", g_sw.pace_mode);
   g_sw.drc             = (int)fe_ini_get_int(ini, "drc", g_sw.drc);
   g_sw.autoexit_frames = (unsigned)fe_ini_get_int(ini, "autoexit_frames", 0);
   g_sw.link_bench      = (int)fe_ini_get_int(ini, "link_bench", 0);
   mgift_vita_set_port((unsigned)fe_ini_get_int(ini, "mgift_port",
                                                MGNET_LISTEN_PORT));
   g_sw.gba_link_fast   = (int)fe_ini_get_int(ini, "gba_link_fast",
                                              g_sw.gba_link_fast);
   g_sw.gba_link_headless_audio = (int)fe_ini_get_int(ini,
      "gba_link_headless_audio", g_sw.gba_link_headless_audio);
   g_sw.io_thread       = (int)fe_ini_get_int(ini, "io_thread", g_sw.io_thread);
   g_sw.touch_menu      = (int)fe_ini_get_int(ini, "touch_menu", g_sw.touch_menu);
   g_sw.up_bench        = (int)fe_ini_get_int(ini, "up_bench", 0);
   g_sw.xbrz_dist       = (int)fe_ini_get_int(ini, "xbrz_dist", g_sw.xbrz_dist);
   g_sw.vm_probe        = (int)fe_ini_get_int(ini, "vm_probe", 0);
   g_sw.pad_trace       = (int)fe_ini_get_int(ini, "pad_trace", 0);
   g_sw.keys_script     = (int)fe_ini_get_int(ini, "keys_script", 0);
   g_sw.clips           = (int)fe_ini_get_int(ini, "clips", 1);
   g_sw.up_slide        = (int)fe_ini_get_int(ini, "up_slide", g_sw.up_slide);
   g_sw.main_cpu        = (int)fe_ini_get_int(ini, "main_cpu", g_sw.main_cpu);
   g_sw.tex_mem         = (int)fe_ini_get_int(ini, "tex_mem", g_sw.tex_mem);
   g_sw.fade_drop       = (int)fe_ini_get_int(ini, "fade_drop", 2);
   g_sw.render_thread   = (int)fe_ini_get_int(ini, "render_thread", 1);
   if (g_sw.render_thread < 0 || g_sw.render_thread > 2)
      g_sw.render_thread = 1;
   g_sw.gpu_xbrz        = (int)fe_ini_get_int(ini, "gpu_xbrz", 0) ? 1 : 0;
   g_sw.gpu_time        = (int)fe_ini_get_int(ini, "gpu_time", 0) ? 1 : 0;
   g_sw.gpu_probe       = (int)fe_ini_get_int(ini, "gpu_probe", 0);
   g_sw.gpu_view        = (int)fe_ini_get_int(ini, "gpu_xbrz_view", 0);
   g_sw.core_phase      = (int)fe_ini_get_int(ini, "core_phase", 0);
   if (g_sw.core_phase < 0 || g_sw.core_phase > 3)
      g_sw.core_phase = 0;
   g_sw.art             = (int)fe_ini_get_int(ini, "art", g_sw.art);
   g_sw.drain           = (int)fe_ini_get_int(ini, "drain", g_sw.drain);
   if (!fe_ini_get(ini, "art_url", g_sw.art_url, sizeof(g_sw.art_url)))
      g_sw.art_url[0] = '\0';
   g_sw.art_titles      = (int)fe_ini_get_int(ini, "art_titles", 1) ? 1 : 0;
   g_sw.wide            = (int)fe_ini_get_int(ini, "wide", -1);
   g_sw.wide_fill       = (int)fe_ini_get_int(ini, "wide_fill", -1);
   if (g_sw.pace_mode != PACE_TIMER)
      g_sw.pace_mode = PACE_VSYNC;
}

/* ---- ROM folders beside roms/ ------------------------------------------------
 *
 * The game list shows roms/ and, after it, the games of these folders (the
 * same depth, the same file types; psp/ui_psp.c rom_scan).  Where they come
 * from:
 *
 *   vita.ini  extra_roms = ux0:ROMS/GBA ; ux0:Games/GB     up to eight, ';'
 *             between, blanks around each dropped.  When the key is there it
 *             is the whole list; `extra_roms = 0` (or nothing after the '=')
 *             means no extra folder at all.
 *   no key    the well-known places below, the ones that exist: a card that
 *             has none of them is read exactly as before.
 *
 * Never the roms/ folder itself and never one folder twice (compared without
 * regard to case and to the kind of slash: the card's file system does not
 * tell ROMS from roms).  A folder has to carry its device ("ux0:...";
 * psp/rom_paths.h): that is what makes a game's name a whole path.
 * Built once at start; ui_host_extra_roots / _root hand it to the list and
 * to the link layer (link_vita.c looks for a partner's cartridge in them). */
static const char *const k_builtin_roots[] = {
   "ux0:ROMS/GBA", "ux0:ROMS/GB", "ux0:ROMS/GBC",
   "ux0:roms/GBA", "ux0:roms/GB", "ux0:roms/GBC",
   "ux0:data/RetroFlow/ROMS/Nintendo - Game Boy Advance",
   "ux0:data/RetroFlow/ROMS/Nintendo - Game Boy",
   "ux0:data/RetroFlow/ROMS/Nintendo - Game Boy Color"
};
#define EXTRA_ROOT_MAX_LEN 200        /* leaves 119 bytes of a 319-byte name */

static char g_extra_roots[UI_EXTRA_ROOTS_MAX][APP_PATH_CAP];
static int  g_extra_n;
static int  g_extra_skipped;

int ui_host_extra_roots(void)
{
   return g_extra_n;
}

const char *ui_host_extra_root(int i)
{
   return i >= 0 && i < g_extra_n ? g_extra_roots[i] : "";
}

/* One spelling of a folder: blanks around it dropped, a backslash as '/', no
 * doubled or trailing '/'.  0 = nothing left, or it does not fit. */
static int root_norm(char *out, size_t cap, const char *in)
{
   size_t n = 0;
   while (*in && isspace((unsigned char)*in))
      in++;
   for (; *in; in++)
   {
      char c = *in == '\\' ? '/' : *in;
      if (c == '/' && n && out[n - 1] == '/')
         continue;
      if (n + 1 >= cap)
         return 0;
      out[n++] = c;
   }
   while (n && (out[n - 1] == '/' || isspace((unsigned char)out[n - 1])))
      n--;
   out[n] = '\0';
   return n > 0;
}

static void extra_root_add(const char *spec, int log_skips)
{
   char norm[APP_PATH_CAP], prim[APP_PATH_CAP];
   const char *why = NULL;
   struct stat st;
   int i;

   if (!spec[0] || !root_norm(norm, sizeof(norm), spec))
      return;                       /* an empty item ("a;;b", a trailing ';') */
   root_norm(prim, sizeof(prim), g_app_rom_dir);
   if (strlen(norm) > EXTRA_ROOT_MAX_LEN)
      why = "too_long";
   else if (!psp_rom_name_is_absolute(norm))
      why = "no_device";
   else if (!strcasecmp(norm, prim))
      why = "same_as_roms";
   for (i = 0; !why && i < g_extra_n; i++)
      if (!strcasecmp(norm, g_extra_roots[i]))
         why = "duplicate";
   if (!why && (stat(norm, &st) != 0 || !S_ISDIR(st.st_mode)))
      why = "missing";
   if (!why && g_extra_n >= UI_EXTRA_ROOTS_MAX)
      why = "too_many";
   if (why)
   {
      /* a well-known folder that is not on this card is no news; one the
       * player named is */
      if (log_skips)
      {
         g_extra_skipped++;
         fe_evt("rom_roots_skip path=\"%s\" reason=%s", norm, why);
      }
      return;
   }
   snprintf(g_extra_roots[g_extra_n++], sizeof(g_extra_roots[0]), "%s", norm);
}

static void rom_roots_init(void)
{
   char ini[APP_PATH_CAP], list[APP_PATH_CAP], all[UI_EXTRA_ROOTS_MAX *
                                                   (EXTRA_ROOT_MAX_LEN + 1)];
   const char *source = "builtin";
   char *tok, *next;
   size_t i, used = 0;

   g_extra_n = g_extra_skipped = 0;
   path_fmt(ini, sizeof(ini), "%s/vita.ini", g_app_root);
   if (fe_ini_get(ini, "extra_roms", list, sizeof(list)))
   {
      /* the key is the whole list; 0 or nothing is no list */
      char *v = list;
      size_t n;
      while (*v && isspace((unsigned char)*v))
         v++;
      n = strlen(v);
      while (n && isspace((unsigned char)v[n - 1]))
         v[--n] = '\0';
      if (!n || !strcmp(v, "0"))
         source = "off";
      else
      {
         source = "ini";
         for (tok = v; tok; tok = next)
         {
            next = strchr(tok, ';');
            if (next)
               *next++ = '\0';
            extra_root_add(tok, 1);
         }
      }
   }
   else
      for (i = 0; i < sizeof(k_builtin_roots) / sizeof(k_builtin_roots[0]); i++)
         extra_root_add(k_builtin_roots[i], 0);

   all[0] = '\0';
   for (i = 0; i < (size_t)g_extra_n; i++)
      used += (size_t)snprintf(all + used, sizeof(all) - used, "%s%s",
                               i ? ";" : "", g_extra_roots[i]);
   fe_evt("rom_roots primary=\"%s\" extra=%d source=%s skipped=%d list=\"%s\"",
          g_app_rom_dir, g_extra_n, source, g_extra_skipped, all);
}

/* autoboot.txt: the first game of this run, started without the list. */
static int rom_from_autoboot(char *out, size_t cap)
{
   char path[APP_PATH_CAP], line[APP_PATH_CAP];
   size_t n;
   FILE *f;

   path_fmt(path, sizeof(path), "%s/autoboot.txt", g_app_root);
   f = fopen(path, "r");
   if (!f)
      return -1;
   if (!fgets(line, sizeof(line), f))
      line[0] = '\0';
   fclose(f);
   n = strlen(line);
   while (n && isspace((unsigned char)line[n - 1]))
      line[--n] = '\0';
   if (!n)
      return -1;
   /* A device prefix ("ux0:...") or a leading slash is a full path; anything
    * else is a name under roms/. */
   if (strchr(line, ':') || line[0] == '/')
      path_fmt(out, cap, "%s", line);
   else
      path_fmt(out, cap, "%s/%s", g_app_rom_dir, line);
   if (file_size(out, NULL) != 0)
   {
      fe_evt("autoboot missing path=%s", out);
      return -1;
   }
   return 0;
}

/* ---- a notice ----------------------------------------------------------------
 * A full-screen message that is not the end of the app (a game that would
 * not start, a link that had to close the game): shown until a button or
 * `hold_ms`, then the caller goes back to the game list.  ui_psp.c's own
 * ui_notice says "return to the LiveArea", which is not where these go. */
static void app_notice(const char *title, const char *l1, const char *l2,
                       unsigned hold_ms)
{
   uint64_t t0 = plat_time_us();
   unsigned first = pspcompat_pad();

   fe_evt("notice title=\"%s\" l1=\"%s\"", title, l1 ? l1 : "");
   while (g_running)
   {
      uint64_t dt = plat_time_us() - t0;
      unsigned pad = pspcompat_pad();
      const char *hint = "Press any button for the game list";

      vid_overlay_begin(1);
      /* VitAdhoc's look (vita/ui_vita_style.h): the navy page, a glow, the
       * wordmark small, the words centred */
      vs_rect(0, 0, VS_W, VS_H, VS_HEX(0x060914, 255));
      vs_shadow(330, 110, 300, 220, 110, 160, VS_HEX(0xFF6B6B, 34));
      vs_wordmark(36, 26, 34, VS_HEX(0xFFFFFF, 220));
      vs_circle(VS_W * 0.5f, 170, 44, VS_HEX(0xFFFFFF, 30));
      vs_icon(VS_I_CART, VS_W * 0.5f, 170, 46, VS_HEX(0xFF8A8A, 255));
      vs_text_c(VS_TITLE, VS_W * 0.5f, 236, title, VS_HEX(0xF5F7FC, 255));
      if (l1) vs_text_c(VS_LABEL, VS_W * 0.5f, 298, l1, VS_HEX(0xFFC38A, 255));
      if (l2) vs_text_c(VS_BODY, VS_W * 0.5f, 336, l2, VS_HEX(0xA7B0C6, 255));
      vs_text_c(VS_CAPTION, VS_W * 0.5f, VS_H - 60, hint,
                VS_HEX(0x6E7891, 255));
      vid_overlay_end();
      pspcompat_wait_vblank();     /* -> modal_tick */
      vid_swap();
      /* A button that was already down when the screen came up is not an
       * answer to it. */
      first &= pad;
      if ((pad & ~first) && dt > 300000u)
         break;
      if (dt >= (uint64_t)hold_ms * 1000u)
         break;
   }
}

/* ---- fe_host's platform sinks ----------------------------------------------- */

static void sink_video(const uint16_t *pix, unsigned w, unsigned h,
                       size_t pitch_bytes)
{
   /* the picture drawn on another core, when it was (render_thread.h) */
   pix = rt_frame_end(pix);
   /* A link session's stall re-presents the last picture without emulating
    * anything: it is not a frame of the game. */
   if (!fe_host_gblink_stalled())
      g_fps_emu++;
   if (pix)
   {
      /* A GBA picture wider than the console's is the wide one; of its
       * margins, what the scale in use has room for is shown. */
      unsigned m = (h == 160 && w > 240) ? (w - 240) / 2 : 0;
      unsigned fit = m ? (unsigned)vid_margin_fit(w - 2 * m, h, (int)m) : 0;
      g_last.pix = pix + m;
      g_last.w = w - 2 * m;
      g_last.h = h;
      g_last.pitch = pitch_bytes;
      g_show.pix = pix + (m - fit);
      g_show.w = g_last.w + 2 * fit;
      g_show.h = h;
      g_show.pitch = pitch_bytes;
   }
   if (g_blit_suppress)
      return;                 /* an intermediate frame of 3x fast forward */
   if (g_ff.uncapped && !pix)
      return;                 /* turned away by the draw gate: nothing new */
   if (!g_last.pix)
      return;

   if (link_local_active())
   {
      /* Both machines on this console: local_vita.c draws the whole frame,
       * the two pictures side by side (vid_draw_frame_pair), from the
       * player's machine's frame and the partner's it kept (app_vita.h,
       * part B: "who draws"). */
      link_local_present(g_last.pix, g_last.w, g_last.h, g_last.pitch);
      g_app_drew = 1;
   }
   else if (!link_owns_picture())
   {
      /* pix == NULL is a repeated frame: the display is double-buffered, so
       * the previous picture is drawn again. */
      vid_draw_frame(g_show.pix, g_show.w, g_show.h, g_show.pitch);
      g_app_drew = 1;
   }
   if (pix)
   {
      g_fps_drawn++;
      g_drawn_total++;
   }
}

static void sink_audio(const int16_t *stereo_lr, size_t frames)
{
   /* Fast forward is silent (config.ini ff_audio = 1 keeps the sound, for
    * measurements): the ring drains and the port plays the fade-out. */
   if ((g_ff.on && !g_pcfg.ff_audio) || rewind_active())
      return;                 /* ...and so is a game running backwards */
   /* Two machines on this console: only one of them is heard. */
   if (link_local_active() && link_local_audio(stereo_lr, frames))
      return;
   audio_push(stereo_lr, frames);
}

/* ---- box art, fetched by itself (artfetch.h) ------------------------------ */

#ifdef __vita__
/* The network may be used for it: Wi-Fi up, the radio not in ad hoc mode. */
static int art_online(void)
{
   net_vita_wifi w;
   return net_vita_wifi_status(&w) && net_vita_use() != NET_VITA_ADHOC;
}
#endif

static void art_start(void)
{
   int rc = -1;
#ifdef __vita__
   const int want = g_sw.art != 0;
#else
   const int want = g_sw.art != 0 && g_sw.art_url[0];   /* the twin: tests */
#endif
   artfetch_titles(g_sw.art_titles);
   if (want)
      rc = artfetch_start(g_app_root, g_sw.art_url[0] ? g_sw.art_url : NULL,
#ifdef __vita__
                          art_online
#else
                          NULL
#endif
                          );
   fe_evt("art_fetch start=%d rc=%d url=%s", want, rc,
          g_sw.art_url[0] ? g_sw.art_url : "libretro");
}

/* keys_script (g_sw): the buttons of keys.txt, frame by frame. */
static gbalink_script g_keys;
static int g_keys_on;
static unsigned g_keys_seen;          /* frames looked at so far */

static void keys_script_start(void)
{
   char path[APP_PATH_CAP];
   if (!g_sw.keys_script || g_keys_on)
      return;
   g_sw.keys_script = 0;                 /* the first game only */
   memset(&g_keys, 0, sizeof(g_keys));
   path_fmt(path, sizeof(path), "%s/keys.txt", g_app_root);
   if (gbalink_script_load(&g_keys, path, 60u * 60u * 10u) != 0)
   {
      fe_evt("keys_script FAILED path=%s", path);
      return;
   }
   g_keys_on = 1;
   g_keys_seen = 0;
   fe_evt("keys_script on path=%s frames=%u", path, g_keys.n);
}

static void keys_script_end(void)
{
   if (!g_keys_on)
      return;
   g_keys_on = 0;
   gbalink_script_free(&g_keys);
}

/* The script's buttons (GBA KEYINPUT order) as the joypad mask the core
 * is given (RETRO_DEVICE_ID_JOYPAD_* bits). */
static uint32_t keys_script_mask(unsigned frame)
{
   static const unsigned char to_retro[10] = {
      8 /*A*/, 0 /*B*/, 2 /*SELECT*/, 3 /*START*/, 7 /*RIGHT*/, 6 /*LEFT*/,
      4 /*UP*/, 5 /*DOWN*/, 11 /*R*/, 10 /*L*/ };
   uint32_t k, m = 0;
   unsigned i;
   if (!g_keys_on)
      return 0;
   k = gbalink_script_keys(&g_keys, frame);
   for (i = 0; i < 10; i++)
      if (k & (1u << i))
         m |= 1u << to_retro[i];
   return m;
}

/* Once a frame from the game loop: the script's screenshots and marks. */
static void keys_script_frame(unsigned frame)
{
   if (!g_keys_on)
      return;
   for (; g_keys_seen < frame && g_keys_seen < g_keys.n; g_keys_seen++)
   {
      if (g_keys.dump && g_keys.dump[g_keys_seen])
         g_shot_pending = 1;
      if (g_keys.mark && g_keys.mark[g_keys_seen])
         fe_evt("keys_script mark frame=%u %s", g_keys_seen,
                g_keys.mark[g_keys_seen]);
   }
}

static uint32_t g_game_keys;           /* what sink_input last gave the core */
static int g_game_console = -1;        /* FE_CONSOLE_* of the running game  */

/* TURBO BUTTONS (psp/ctl_map.h, GBAdhoc 4.0; Settings > Controls > Turbo
 * buttons).  The pulse is counted in emulated frames, so it is ORed in here,
 * where each frame's buttons are asked for -- a catch-up frame gets the
 * press its frame number says, as a frame drawn would.  Stepped once per
 * pad sample in game_loop; stopped whenever play stops (the menu, a loaded
 * state, rewinding, leaving the game, sleep) and off during a link. */
static ctl_turbo g_turbo;

static uint32_t sink_input(void)
{
   uint32_t frame = fe_host_frame_count();
   g_game_keys = g_app_keys | keys_script_mask(frame);
   if (g_turbo.live)
      g_game_keys |= ctl_turbo_mask(&g_pcfg.controls, &g_turbo, frame);
   return g_game_keys;
}

static void sink_yield(void)
{
   plat_sleep_us(1000);
}

static void loading_progress(unsigned loaded, unsigned total)
{
   ui_loading_update(total ? "Reading ROM" : "Checking cartridge", loaded,
                     total);
}

static void loading_stage(const char *stage)
{
   ui_loading_update(stage, 0, 0);
}

/* ---- the I/O worker -------------------------------------------------------
 * The event log and the GBA battery save are written by a low-priority
 * thread, as on the PSP (ADR-0024/0025): a write to the memory card takes
 * anything from under a millisecond to tens of them, and a frame has 16.7.
 * fe_evt and fe_host each allow exactly one consumer, so the order in
 * io_start/io_stop matters; fe_host.h has the contract.  The worker lives
 * for one game: started after the boot, stopped before the unload, so the
 * save is single-threaded at both ends.  With no thread (plat_worker_start
 * failed, the switch, or the twin's default) both stay synchronous and
 * fe_host checks the save every 300 frames itself. */

static void io_step(void)
{
   fe_evt_service();
   fe_host_sram_service_io();
   link_local_io();             /* the local link's partner has a save too */
   rewind_worker();             /* the newest state's XOR and squeeze */
   clip_worker();               /* the preview's newest frame, squeezed */
}

static void io_yield(void)
{
   plat_sleep_us(1000);
}

static const fe_host_io g_io = { plat_worker_wake, io_yield };

static void io_start(void)
{
   /* 250 ms: how stale the log on the card may be.  The save's own cadence
    * (a scan every 5 s) is fe_host's. */
   g_worker = g_sw.io_thread && plat_worker_start(io_step, 250000) == 0;
   if (g_worker)
   {
      fe_evt_set_async(plat_worker_wake);
      /* A GB cartridge's RAM has no race-safe block scanner (the GBA core
       * has): its battery snapshot stays on the emulation thread, as on
       * the PSP.  The log is asynchronous either way. */
      if (g_game.console == FE_CONSOLE_GBA)
         fe_host_set_io(&g_io);
   }
   rewind_set_async(g_worker ? plat_worker_wake : NULL);
   clip_set_async(g_worker ? plat_worker_wake : NULL);
   fe_evt("io_worker on=%d sram_async=%d", g_worker,
          g_worker && g_game.console == FE_CONSOLE_GBA);
}

static void io_stop(void)
{
   if (!g_worker)
      return;
   plat_worker_stop();          /* the worker's last pass, then it is gone */
   rewind_set_async(NULL);
   clip_set_async(NULL);
   fe_host_set_io(NULL);
   fe_evt_set_async(NULL);      /* drains the log's ring on this thread    */
   fe_host_sram_sync();         /* whatever the writer had not finished    */
   g_worker = 0;
}

/* ---- pacing --------------------------------------------------------------- */

void app_pace_reset(void)
{
   g_app_vc_target = plat_vcount();
   g_next_ns = plat_time_us() * 1000ull;
   g_catchup = 0;
}

unsigned app_vcount(void)
{
   return plat_vcount();
}

/* Wait out the rest of this frame's slot.  Called after the frame's work;
 * the network is looked at again while waiting (link_idle_pump). */
static void pace_frame(void)
{
   if (g_sw.pace_mode == PACE_VSYNC)
   {
      /* Present on the refresh AFTER the one the last frame used.  Counting
       * against the display's absolute counter instead of calling "wait for
       * a vblank" once means a frame that overran consumes the refresh it
       * overran into; a plain wait would add a second one on top and drop
       * to 30 fps the moment a frame cost 17 ms (ADR-0047 on the PSP). */
      unsigned vc;
      int behind;
      g_app_vc_target++;
      vc = plat_vcount();
      behind = (int)(vc - g_app_vc_target);
      if (behind > (g_catchup_ok ? CATCHUP_HORIZON : PACE_RESYNC_VB))
      {
         g_app_vc_target = vc;
         g_pace_forgiven++;
      }
      else if (behind >= 1)
      {
         /* a whole refresh or more lost: owed (catching up, above) */
         g_pace_late++;
         if (g_catchup_ok)
            g_catchup = (unsigned)behind;
         else if (g_sw.drain)
            /* Keep full speed off (or a link session, fast forward): the
             * time is lost, the game slows down with the console.  The
             * same pacing as staying a refresh behind (the wait below would
             * do it), but `late` counts the moment, not every frame after. */
            g_app_vc_target = vc;
      }
      while ((int)(plat_vcount() - g_app_vc_target) < 0)
      {
         link_idle_pump();
         plat_wait_vblank();
      }
      /* A FRAME STARTS ONCE THE LAST PICTURE IS SHOWN.  The display queue
       * holds two pictures; a present that finds the one before still
       * queued waits in the queue for a refresh, and from then on every
       * picture reaches the glass a refresh later than it could (measured:
       * the present waiting ~9 ms each frame, `late` counting every
       * frame).  So the frame waits here instead, before it reads the pad;
       * the refresh this costs the game is a frame owed, and caught up.
       * Not when frames are owed already: those run without a picture, so
       * the queue has their time to empty, and waiting here would only
       * stretch the gap in the sound (the game's samples come with its
       * frames; measured on the twin: three 25 ms frames in a row were a
       * gap with the wait, none without it). */
      if (!g_catchup)
      {
         static unsigned stuck;
         const int st = vid_present_state();
         const int mask = g_sw.drain == 2 ? 1 : g_sw.drain == 1 ? 2 : 0;
         int n = 0;
         g_q_taken += st & 1;
         g_q_glass += (st >> 1) & 1;
         while (mask && (vid_present_state() & mask) && n < 2)
         {
            link_idle_pump();
            plat_wait_vblank();
            n++;
         }
         g_q_waits += (unsigned)n;
         /* A display that never says "shown" (a system overlay owning the
          * screen, a firmware that answers otherwise) would halve the frame
          * rate: two refreshes waited for nothing, many frames running, and
          * the wait is given up for the session. */
         stuck = n >= 2 ? stuck + 1 : 0;
         if (stuck >= 30)
         {
            fe_evt("drain off reason=never_shown state=%d", st);
            g_sw.drain = 0;
            stuck = 0;
         }
      }
   }
   else
   {
      uint64_t now_ns = plat_time_us() * 1000ull;
      g_next_ns += GBA_FRAME_NS;
      if (now_ns > g_next_ns + (g_catchup_ok ? CATCHUP_HORIZON : PACE_RESYNC_VB)
                               * GBA_FRAME_NS)
      {
         g_next_ns = now_ns;
         g_pace_forgiven++;
      }
      else if (now_ns >= g_next_ns)
      {
         g_pace_late++;
         if (g_catchup_ok && now_ns - g_next_ns >= GBA_FRAME_NS)
            g_catchup = (unsigned)((now_ns - g_next_ns) / GBA_FRAME_NS);
      }
      else
      {
         link_idle_pump();
         plat_sleep_us((uint32_t)((g_next_ns - now_ns) / 1000ull));
      }
      /* link_vita.c's stall budget reads the refresh this frame is for. */
      g_app_vc_target = plat_vcount();
   }
}

/* How fast the pacer runs the game relative to a real GBA, in millionths:
 * the resampler's starting point (audio_vita.h). */
static unsigned pace_speed_x1e6(void)
{
   if (g_sw.pace_mode == PACE_VSYNC)
      return (unsigned)((double)plat_refresh_mhz() * 1000000.0 / GBA_FPS_X1000 + 0.5);
   return 1000000u;
}

void app_audio_flush(void)
{
   audio_flush();
}

/* ---- the frame the player was looking at ------------------------------------ */

const uint16_t *app_last_frame(unsigned *w, unsigned *h, size_t *pitch_bytes)
{
   if (w) *w = g_last.w;
   if (h) *h = g_last.h;
   if (pitch_bytes) *pitch_bytes = g_last.pitch;
   return g_last.pix;
}

const uint16_t *app_shown_frame(unsigned *w, unsigned *h, size_t *pitch_bytes)
{
   if (w) *w = g_show.w;
   if (h) *h = g_show.h;
   if (pitch_bytes) *pitch_bytes = g_show.pitch;
   return g_show.pix;
}

void app_frame_forget(void)
{
   memset(&g_last, 0, sizeof(g_last));
   memset(&g_show, 0, sizeof(g_show));
}

/* The wide picture is the single game's: with two machines on this console
 * each has its half of the screen.  Called wherever one of the three things
 * it depends on changes (the Settings row, the console in context, the
 * pair starting or stopping); vita.ini's `wide` overrides the row. */
void app_wide_apply(void)
{
   int mode = pcfg_display_console() == FE_CONSOLE_GBA ? g_pcfg.wide : WIDE_OFF;
   int fill = g_pcfg.wide_fill;
   if (g_sw.wide >= 0)
      mode = g_sw.wide & 7;
   if (link_local_active())
      mode = WIDE_OFF;
   if (g_sw.wide_fill >= 0)
      fill = g_sw.wide_fill;
   if (mode != g_wide_mode || fill != g_wide_fill)
   {
      g_wide_mode = mode;
      g_wide_fill = fill;
      wide_set_fill(fill);
      wide_set(mode);
      fe_evt("wide mode=%d fill=%d margin=%d profile=%d name=\"%s\"", mode,
             fill, wide_margin_px(), wide_profile_id(), wide_profile_name());
   }
}

int app_snapshot_take(void)
{
   unsigned w = g_last.w, h = g_last.h, y;

   if (!g_last.pix || !w || !h)
      return g_snap_have;         /* keep what there is */
   if (w > 240) w = 240;
   if (h > 160) h = 160;
   memset(g_snap, 0, sizeof(g_snap));
   vid_frame_to_psp565(g_snap, SNAP_TEX, g_last.pix, w, h, g_last.pitch);
   /* The edge texels once more, right and below, as the PSP's snapshot has
    * them: a filtered tap past the picture's edge reads the edge, not
    * black. */
   for (y = 0; y < h; y++)
      g_snap[y * SNAP_TEX + w] = g_snap[y * SNAP_TEX + w - 1];
   memcpy(g_snap + h * SNAP_TEX, g_snap + (h - 1) * SNAP_TEX,
          (w + 1) * sizeof(uint16_t));
   /* The renderer keeps a GPU texture per pointer: these bytes changed. */
   vid_tex_invalidate(g_snap, sizeof(g_snap));
   g_snap_w = w;
   g_snap_h = h;
   g_snap_have = 1;
   return 1;
}

/* The home screen's picture of a game (vita/ui_vita_home.h): the last
 * frame the player saw, written when the game is left as
 * scenes/<game>.shot -- the .shot format of a save state's preview, so one
 * reader serves both.  A failed write leaves nothing (no torn picture). */
static void scene_save(void)
{
   char stem[96], path[APP_PATH_CAP];
   unsigned char hdr[PSP_STATE_THUMB_HEADER_SIZE];
   char *dot;
   unsigned y;
   FILE *f;
   int ok;

   if (!g_snap_have || !g_snap_w || !g_snap_h ||
       g_snap_w > PSP_STATE_SHOT_MAX_W || g_snap_h > PSP_STATE_SHOT_MAX_H)
      return;
   snprintf(stem, sizeof(stem), "%s", base_name(g_game.rom_path));
   dot = strrchr(stem, '.');
   if (dot && dot != stem)
      *dot = '\0';
   if (path_fmt(path, sizeof(path), "%s/scenes/%s.shot", g_app_root,
                stem) != 0)
      return;
   psp_state_shot_header(hdr, g_snap_w, g_snap_h);
   f = fopen(path, "wb");
   if (!f)
      return;
   ok = fwrite(hdr, 1, sizeof(hdr), f) == sizeof(hdr);
   /* g_snap's stride is the .shot's (SNAP_TEX = PSP_STATE_SHOT_STRIDE) */
   for (y = 0; ok && y < g_snap_h; y++)
      ok = fwrite(g_snap + (size_t)y * SNAP_TEX, 2, PSP_STATE_SHOT_STRIDE, f)
           == PSP_STATE_SHOT_STRIDE;
   if (fclose(f) != 0)
      ok = 0;
   if (!ok)
      remove(path);
   fe_evt("scene_save ok=%d w=%u h=%u", ok, g_snap_w, g_snap_h);
}

typedef char snap_is_shot_stride[SNAP_TEX == PSP_STATE_SHOT_STRIDE ? 1 : -1];

/* The home screen's preview of a game (clip_vita.h): the last ten seconds
 * played, written when the game is left as clips/<game>.clip.  A game
 * played for less than three seconds keeps the clip it had. */
static void clip_save_game(void)
{
   char stem[96], path[APP_PATH_CAP];
   clip_stats st;
   char *dot;
   uint64_t t0;
   int rc;

   snprintf(stem, sizeof(stem), "%s", base_name(g_game.rom_path));
   dot = strrchr(stem, '.');
   if (dot && dot != stem)
      *dot = '\0';
   if (path_fmt(path, sizeof(path), "%s/clips/%s.clip", g_app_root,
                stem) != 0)
      return;
   t0 = plat_time_us();
   /* a core frame: 280896 cycles of 16.78 MHz (GBA), 70224 of 4.19 MHz
    * (GB, GBC) -- the same 59.73 Hz */
   rc = clip_save(path, 16743);
   clip_get_stats(&st);
   fe_evt("clip_save rc=%d saved=%u calm_cut=%u frames=%u keys=%u ticks=%u "
          "bytes=%u kept=%u skipped=%u enc_us_avg=%u enc_us_max=%u "
          "write_ms=%u", rc, st.saved, st.calm_cut, st.frames, st.keys,
          st.ticks, (unsigned)st.bytes, st.kept, st.skipped,
          st.kept ? (unsigned)(st.enc_us_sum / st.kept) : 0, st.enc_us_max,
          (unsigned)((plat_time_us() - t0) / 1000));
}

void app_backdrop_from_snapshot(void)
{
   ui_set_backdrop(g_snap_have ? g_snap : NULL, (int)g_snap_w, (int)g_snap_h);
}

/* THE IN-GAME MENU OPENS OVER THE GAME (docs/UI-OVERLAY.md): the core is
 * paused from this frame on, so the snapshot is the picture the player was
 * looking at, and the menu draws it, scrimmed, behind every page. */
/* The first games' hints (config.ini hints) are counted down by games in
 * which the player OPENED the menu, not by games started: someone who has
 * not found it yet keeps being told how.  (The owner's first session on a
 * console: three games started, the line missed each time, and then no way
 * to learn it.) */
static int g_hint_owed;

static void menu_open(const char *via)
{
   app_snapshot_take();
   app_backdrop_from_snapshot();
   ui_open();
   g_chord_frames = 0;
   fe_evt("menu_open via=%s frame=%u", via, fe_host_frame_count());
   if (g_hint_owed)
   {
      g_hint_owed = 0;
      if (g_pcfg.hints > 0)
         g_pcfg.hints--;
      fe_evt("hints counted left=%d", g_pcfg.hints);
   }
}

/* ---- save states -------------------------------------------------------------
 * Five slots beside the ROM, named and formatted exactly as the PSP writes
 * them (psp/state_slots.h; psp/main_psp.c state_thumb_write /
 * state_shot_write), so a state made on either console previews on the
 * other.  One difference in how the bytes are made: the PSP's core emits
 * its frames in the PSP's channel order already, the Vita's in libretro's,
 * so the frame is converted first (vid_frame_to_psp565). */

static uint16_t g_conv[PSP_STATE_SHOT_MAX_W * PSP_STATE_SHOT_MAX_H];

/* The frame the previews are made from, in PSP order, tightly packed at its
 * own size (240x160 GBA, 160x144 GB/GBC).  0 = there is one. */
static int preview_frame(unsigned *w, unsigned *h)
{
   if (!g_last.pix || !g_last.w || !g_last.h ||
       g_last.w > PSP_STATE_SHOT_MAX_W || g_last.h > PSP_STATE_SHOT_MAX_H ||
       g_last.pitch < g_last.w * sizeof(uint16_t))
      return -1;
   vid_frame_to_psp565(g_conv, (int)g_last.w, g_last.pix, g_last.w, g_last.h,
                       g_last.pitch);
   *w = g_last.w;
   *h = g_last.h;
   return 0;
}

/* `<state>.thumb`: 16-byte header ("STH1", w, h, stride as u16), then a
 * 64x42 picture.  3:2 like a GBA screen; a narrower frame keeps its shape
 * and is pillarboxed (a GB screen becomes 47x42, centred on black). */
static int state_thumb_write(const char *state_file, unsigned sw, unsigned sh)
{
   uint16_t thumb[PSP_STATE_THUMB_WIDTH * PSP_STATE_THUMB_HEIGHT];
   unsigned char hdr[PSP_STATE_THUMB_HEADER_SIZE] = { 0 };
   char path[PSP_STATE_THUMB_PATH_CAP];
   unsigned x, y, x0, dw;
   FILE *f;
   int ok;

   if (psp_state_thumb_path(path, sizeof(path), state_file) != 0)
      return -1;
   dw = PSP_STATE_THUMB_WIDTH;
   if (sw * PSP_STATE_THUMB_HEIGHT < dw * sh)
      dw = (sw * PSP_STATE_THUMB_HEIGHT + sh / 2) / sh;
   x0 = (PSP_STATE_THUMB_WIDTH - dw) / 2;
   memset(thumb, 0, sizeof(thumb));
   for (y = 0; y < PSP_STATE_THUMB_HEIGHT; y++)
   {
      const uint16_t *src = g_conv + (size_t)(y * sh / PSP_STATE_THUMB_HEIGHT) * sw;
      for (x = 0; x < dw; x++)
         thumb[y * PSP_STATE_THUMB_WIDTH + x0 + x] = src[x * sw / dw];
   }
   memcpy(hdr, PSP_STATE_THUMB_MAGIC, 4);
   hdr[4] = PSP_STATE_THUMB_WIDTH;
   hdr[6] = PSP_STATE_THUMB_HEIGHT;
   hdr[8] = PSP_STATE_THUMB_WIDTH;
   f = fopen(path, "wb");
   if (!f)
      return -1;
   ok = fwrite(hdr, 1, sizeof(hdr), f) == sizeof(hdr) &&
        fwrite(thumb, 1, sizeof(thumb), f) == sizeof(thumb);
   if (fclose(f) != 0)
      ok = 0;
   if (!ok)
      remove(path);
   return ok ? 0 : -1;
}

/* `<state>.shot`: the same header with the frame's own size, then its rows
 * at the page texture's stride of 256 texels (zero beyond the picture). */
static int state_shot_write(const char *state_file, unsigned sw, unsigned sh)
{
   uint16_t row[PSP_STATE_SHOT_STRIDE];
   unsigned char hdr[PSP_STATE_THUMB_HEADER_SIZE];
   char path[PSP_STATE_SHOT_PATH_CAP];
   unsigned y;
   FILE *f;
   int ok;

   if (psp_state_shot_path(path, sizeof(path), state_file) != 0)
      return -1;
   psp_state_shot_header(hdr, sw, sh);
   f = fopen(path, "wb");
   if (!f)
      return -1;
   ok = fwrite(hdr, 1, sizeof(hdr), f) == sizeof(hdr);
   memset(row, 0, sizeof(row));
   for (y = 0; ok && y < sh; y++)
   {
      memcpy(row, g_conv + (size_t)y * sw, sw * sizeof(uint16_t));
      ok = fwrite(row, 1, sizeof(row), f) == sizeof(row);
   }
   if (fclose(f) != 0)
      ok = 0;
   if (!ok)
      remove(path);                /* a torn picture is never left */
   return ok ? 0 : -1;
}

/* Slot 1..5.  0 = the state is on the card.  The previews are companions: a
 * failed one is removed (never a stale picture beside a new state) and does
 * not fail the save. */
static int state_save_slot(unsigned slot)
{
   char path[PSP_FILE_PATH_CAP];
   unsigned w = 0, h = 0;
   int thumb_rc = -1, shot_rc = -1;
   uint64_t t0 = plat_time_us();

   if (psp_state_path_for_slot(path, sizeof(path), g_game.state_path,
                               slot) != 0 ||
       fe_host_state_save(path) != 0)
      return -1;
   if (preview_frame(&w, &h) == 0)
   {
      thumb_rc = state_thumb_write(path, w, h);
      shot_rc = state_shot_write(path, w, h);
   }
   if (thumb_rc != 0)
   {
      char p[PSP_STATE_THUMB_PATH_CAP];
      if (psp_state_thumb_path(p, sizeof(p), path) == 0)
         remove(p);
   }
   if (shot_rc != 0)
   {
      char p[PSP_STATE_SHOT_PATH_CAP];
      if (psp_state_shot_path(p, sizeof(p), path) == 0)
         remove(p);
   }
   /* The local link's partner and the cable go beside it. */
   if (link_local_active())
      link_local_state_saved(path);
   fe_evt("state_save slot=%u thumb=%d shot=%d us=%u", slot, thumb_rc,
          shot_rc, (unsigned)(plat_time_us() - t0));
   return 0;
}

static int state_load_slot(unsigned slot, const char *via)
{
   char path[PSP_FILE_PATH_CAP];
   int rc;

   if (psp_state_path_for_slot(path, sizeof(path), g_game.state_path,
                               slot) != 0)
      return -1;
   rc = fe_host_state_load(path);
   fe_evt("state_load slot=%u via=%s rc=%d", slot, via, rc);
   if (rc == 0)
   {
      ctl_turbo_stop(&g_turbo); /* a loaded state starts with no auto-fire  */
      cheats_apply();           /* the state brought its own RAM back       */
      rewind_clear();           /* "a moment ago" was another timeline      */
      clip_cut();               /* ...and the preview's next frame is whole */
      if (link_local_active())
         link_local_state_loaded(path);
      audio_flush();            /* what is queued belongs to the old timeline */
      app_pace_reset();         /* a load is not a late frame               */
   }
   return rc;
}

/* ---- screenshots ---------------------------------------------------------------
 * The game's own frame at its own size, as a PNG in screenshots/: exact
 * pixels, no bars and no OSD -- with the wide view's margins when they are
 * on screen.  (The PSP writes the same frame as a BMP into its log folder;
 * a file a player goes looking for deserves a better name and place.) */
#define SHOT_MAX_W (PSP_STATE_SHOT_MAX_W + 64)
static void screenshot_write(void)
{
   static unsigned char rgb[SHOT_MAX_W * PSP_STATE_SHOT_MAX_H * 3];
   char stem[80], path[APP_PATH_CAP];
   unsigned w = g_show.w, h = g_show.h, x, y, n;
   time_t now = plat_wallclock();
   struct tm tm;
   char *dot;

   if (!g_show.pix || !w || !h || w > SHOT_MAX_W ||
       h > PSP_STATE_SHOT_MAX_H)
   {
      toast_now("No picture to save yet");
      return;
   }
   for (y = 0; y < h; y++)
   {
      const uint16_t *s = (const uint16_t *)
         ((const unsigned char *)g_show.pix + (size_t)y * g_show.pitch);
      unsigned char *d = rgb + (size_t)y * w * 3;
      for (x = 0; x < w; x++, d += 3)
      {
         unsigned v = s[x], r = v >> 11, g = (v >> 5) & 0x3F, b = v & 0x1F;
         d[0] = (unsigned char)((r << 3) | (r >> 2));
         d[1] = (unsigned char)((g << 2) | (g >> 4));
         d[2] = (unsigned char)((b << 3) | (b >> 2));
      }
   }
   snprintf(stem, sizeof(stem), "%s", base_name(g_game.rom_path));
   dot = strrchr(stem, '.');
   if (dot && dot != stem)
      *dot = '\0';
   memset(&tm, 0, sizeof(tm));
   gmtime_r(&now, &tm);            /* plat_wallclock is local time already */
   for (n = 1; n < 100; n++)
   {
      char tail[16] = "";
      if (n > 1)
         snprintf(tail, sizeof(tail), "-%u", n);
      if (path_fmt(path, sizeof(path),
                   "%s/screenshots/%s-%04d%02d%02d-%02d%02d%02d%s.png",
                   g_app_root, stem, tm.tm_year + 1900, tm.tm_mon + 1,
                   tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec, tail) != 0)
         break;
      if (file_size(path, NULL) != 0)
      {
         if (stbi_write_png(path, (int)w, (int)h, 3, rgb, (int)w * 3))
         {
            fe_evt("screenshot file=%s w=%u h=%u", path, w, h);
            toast_now("Screenshot saved");
            return;
         }
         break;
      }
   }
   fe_evt("screenshot failed path=%s", path);
   toast_now("Screenshot could not be saved");
}

/* ---- power: suspend and resume ------------------------------------------------
 * The Vita suspends a process whole and resumes it with its memory and file
 * handles intact.  What has to happen around it: the battery save goes to
 * the card before the console may be left for days, the picture the player
 * was looking at is kept for the overlay he resumes into, and afterwards
 * nothing may treat the time asleep as a late frame or play sound queued
 * before it. */
static void power_service(void)
{
   unsigned e = plat_power_events();
   if (e & PLAT_POWER_SUSPEND)
   {
      fe_evt("power suspend game=%d frame=%u info=%08x", g_game_loaded,
             g_game_loaded ? fe_host_frame_count() : 0u, plat_power_info());
      link_power(0);
      link_local_power(0);
      if (g_game_loaded)
      {
         if (!ui_active())
            app_snapshot_take();
         /* force = written and drained before this returns */
         fe_host_sram_flush(1);
         cheats_flush();
      }
   }
   if (e & (PLAT_POWER_SUSPEND | PLAT_POWER_RESUME))
      ctl_turbo_stop(&g_turbo);   /* the console slept: auto-fire is over */
   if (e & PLAT_POWER_RESUME)
   {
      fe_evt("power resume game=%d", g_game_loaded);
      link_power(1);
      link_local_power(1);
      if (g_game_loaded)
      {
         audio_flush();
         app_pace_reset();
         /* Back into the frozen frame with a plate over it, not straight
          * into a game nobody is holding yet.  The in-game menu already is
          * that. */
         if (!ui_active())
            g_wake_menu = 1;
      }
   }
}

/* Once per refresh inside the UI's modal screens (the game list, the pause
 * overlay, the loading screen, a notice): pspcompat.c calls this after each
 * wait for the vertical blank, so what the game loop does once a frame for
 * the machine keeps happening while a screen of ui_psp.c owns the loop. */
static void modal_tick(void)
{
   plat_present_hook();
   power_service();
   if (plat_quit_requested())
      g_running = 0;
}

/* ---- the OSD -------------------------------------------------------------------- */

static char g_fps_text[40];            /* the FPS chip, as last set */

/* The three chips as the game shows them: fast forward (top right), the
 * frame rate (top left), the session (bottom right). */
static void osd_chips_restore(void)
{
   char chip[32];
   if (g_ff.on)
   {
      snprintf(chip, sizeof(chip), ">> %s", pcfg_ff_name());
      osd_chip_ff(chip);
   }
   else
      osd_chip_ff(NULL);
   osd_chip_fps(g_pcfg.show_fps && g_fps_text[0] ? g_fps_text : NULL);
   osd_session_chip_refresh();
}

/* The answer to a button the player has just pressed -- "Video: fit",
 * "State saved" -- replaces whatever toast is up instead of joining the
 * queue behind it: osd_psp.c shows its queue one at a time, 2.5 s each, so
 * the fifth press of the preset button would otherwise be answered twelve
 * seconds late.  (Toasts that report events, the link's, still queue.)
 * osd_reset forgets the chips too, so they are set again. */
static void toast_now(const char *fmt, ...)
{
   char msg[80];
   va_list ap;

   va_start(ap, fmt);
   vsnprintf(msg, sizeof(msg), fmt, ap);
   va_end(ap);
   osd_reset();
   osd_chips_restore();
   osd_toast("%s", msg);
}


/* ui_host_hooks.h: the look of the console in context goes to whoever draws
 * it -- a GBA's colours and the blend to the renderer, a Game Boy Color's
 * colours to its core (which re-converts its palettes before the next line;
 * display only, nothing a state or a link's hash holds).  At every boot and
 * whenever the Settings rows change. */
void ui_host_look_changed(void)
{
   int c = pcfg_display_console();
   vid_set_look(c == FE_CONSOLE_GBA ? g_pcfg.color : VID_COLOR_RAW,
                g_pcfg.blend);
   if (c == FE_CONSOLE_GBC)
      gbcore_set_color_correction(g_pcfg.color ? 1 : 0);
   fe_evt("display_look console=%s color=%d blend=%d wide=%d wide_fill=%d",
          pcfg_console_tag(c), g_pcfg.color, g_pcfg.blend, g_pcfg.wide,
          g_pcfg.wide_fill);
   app_wide_apply();
}

/* ui_host_hooks.h: the chip shows the link layer's status line, when the
 * player has not switched the session overlay off. */
void osd_session_chip_refresh(void)
{
   const char *info = link_session_info();
   osd_chip_session(g_pcfg.osd_wireless && info && info[0] ? info : NULL);
}

/* ---- fast forward --------------------------------------------------------------- */

static void ff_stats_evt(void)
{
   unsigned frames = fe_host_frame_count() - g_ff.frames0;
   unsigned vcs = plat_vcount() - g_ff.vc0;
   fe_evt("ff_stats mode=%s frames=%u refreshes=%u drawn=%u us=%u "
          "gate_resyncs=%u",
          g_ff.mode == PCFG_FF_3X ? "3x" :
          g_ff.mode == PCFG_FF_SMOOTH ? "smooth" : "unlimited",
          frames, vcs, g_drawn_total - g_ff.drawn0,
          (unsigned)(plat_time_us() - g_ff.t0), g_ff.gate.resyncs);
}

static void ff_set(int on)
{
   char chip[32];

   if (g_ff.on)
      ff_stats_evt();
   g_ff.on = on;
   g_ff.mode = pcfg_ff_mode();
   g_ff.mult = on && g_ff.mode == PCFG_FF_3X;
   g_ff.uncapped = on && !g_ff.mult;
   g_ff.frames0 = g_game_loaded ? fe_host_frame_count() : 0;
   g_ff.vc0 = plat_vcount();
   g_ff.drawn0 = g_drawn_total;
   g_ff.t0 = plat_time_us();
   /* Unlimited does not wait for the display: neither does the present. */
   vid_set_vsync(!g_ff.uncapped);
   if (on)
   {
      snprintf(chip, sizeof(chip), ">> %s", pcfg_ff_name());
      osd_chip_ff(chip);
      fe_evt("ff on mode=%s", pcfg_ff_name());
   }
   else
   {
      osd_chip_ff(NULL);
      fe_host_skip_override(-1);
      ff_gate_arm(&g_ff.gate, 0);
      fe_evt("ff off");
   }
   app_pace_reset();               /* fast forward is not a debt of frames */
}

/* ---- the chord guard -------------------------------------------------------
 * START and SELECT are the Game Boy's own buttons AND the first button of
 * every shortcut (SELECT+L saves, SELECT+R loads, START+SELECT opens the
 * menu...).  Passed straight through, as the PSP original does, every
 * shortcut also presses SELECT in the game -- Pokemon answers each quick
 * save with "An item in the BAG can be registered to SELECT", and the menu
 * chord opens the game's own START menu behind ours.
 *
 * So those two are held back for a moment (config.ini chord_guard = 1):
 *   - pressed and let go quickly, alone: the game gets a short press, late
 *     by the length of the tap;
 *   - held alone past CG_WAIT frames: the game gets it from then on;
 *   - a partner button of one of its chords comes down while it waits: the
 *     game never sees it, until it is released.
 * Only START and SELECT wait: nothing in a Game Boy game times them to the
 * frame, and L, R and the d-pad keep their zero latency.  A chord's other
 * buttons are taken from the game when the shortcut fires (chord_took). */
#define CG_WAIT 8                /* frames a guarded button waits alone  */
#define CG_TAP  5                /* length of the press a tap becomes    */

enum { CG_IDLE = 0, CG_PENDING, CG_PASS, CG_SPOILED, CG_TAPPING };
static struct
{
   unsigned char state, t;
} g_cg[2];
static const unsigned CG_BTN[2] = { CTL_START, CTL_SELECT };

/* The buttons that make a chord with `btn` under the current bindings. */
static unsigned chord_partners(unsigned btn)
{
   const ctl_map *m = &g_pcfg.controls;
   unsigned r = 0;
   int a;
   for (a = CTL_GAME_COUNT; a < CTL_ACTIONS; a++)
   {
      unsigned b = m->bind[a];
      if ((b & btn) && (b & ~btn))
         r |= b & ~btn;
   }
   if (m->menu_home == CTL_MENU_START_SELECT && (CTL_MENU_CHORD & btn))
      r |= CTL_MENU_CHORD & ~btn;
   return r;
}

static void chord_guard_reset(void)
{
   memset(g_cg, 0, sizeof(g_cg));
}

/* `pad` is what the game may see this frame (blocked buttons already out);
 * `raw` the whole pad.  Returns `pad` with START/SELECT as the game is to
 * see them. */
static unsigned chord_guard(unsigned pad, unsigned raw)
{
   int i;
   if (!g_pcfg.chord_guard)
      return pad;
   for (i = 0; i < 2; i++)
   {
      unsigned btn = CG_BTN[i];
      unsigned partners = chord_partners(btn);
      int down = (pad & btn) != 0;
      int deliver = 0;
      if (!partners)             /* in no chord: nothing to wait for */
      {
         g_cg[i].state = CG_IDLE;
         continue;
      }
      switch (g_cg[i].state)
      {
      case CG_TAPPING:
         if (down)               /* pressed again before the tap ended */
         {
            g_cg[i].state = CG_PENDING;
            g_cg[i].t = 0;
            break;
         }
         deliver = 1;
         if (++g_cg[i].t >= CG_TAP)
            g_cg[i].state = CG_IDLE;
         break;
      case CG_IDLE:
         if (!down)
            break;
         g_cg[i].state = CG_PENDING;
         g_cg[i].t = 0;
         /* fall through */
      case CG_PENDING:
         if (raw & partners)
            g_cg[i].state = CG_SPOILED;
         else if (!down)
         {
            g_cg[i].state = CG_TAPPING;
            g_cg[i].t = 0;
            deliver = 1;
         }
         else if (++g_cg[i].t >= CG_WAIT)
         {
            g_cg[i].state = CG_PASS;
            deliver = 1;
         }
         break;
      case CG_PASS:
         if (!down)
            g_cg[i].state = CG_IDLE;
         else
            deliver = 1;
         break;
      case CG_SPOILED:
         if (!(raw & btn))
            g_cg[i].state = CG_IDLE;
         break;
      }
      if (deliver)
         pad |= btn;
      else
         pad &= ~btn;
   }
   return pad;
}

/* A shortcut made of several buttons fired: every button of it that is held
 * stays away from the game until it is released. */
static unsigned chord_took(unsigned fired, unsigned pad)
{
   static const struct { unsigned fire; int action; } MAP[] = {
      { CTL_FIRE_SAVE, CTL_SC_SAVE },   { CTL_FIRE_LOAD, CTL_SC_LOAD },
      { CTL_FIRE_SHOT, CTL_SC_SHOT },   { CTL_FIRE_PAUSE, CTL_SC_PAUSE },
      { CTL_FIRE_REWIND, CTL_SC_REWIND } };
   const ctl_map *m = &g_pcfg.controls;
   unsigned took = 0, i;
   for (i = 0; i < sizeof(MAP) / sizeof(MAP[0]); i++)
   {
      unsigned b = m->bind[MAP[i].action];
      if ((fired & MAP[i].fire) && (b & (b - 1)))    /* two buttons or more */
         took |= b;
   }
   return took & pad;
}

/* Once per iteration, before the core runs: the shortcut, the session
 * interlock, and the draw gate's decision for the frame about to run. */
static void ff_frame(unsigned fired, int session, int local)
{
   int press = (fired & CTL_FIRE_FF_PRESS) != 0;
   int want;

   if (g_pcfg.ff_hold)
      want = (fired & CTL_FIRE_FF_HELD) != 0;
   else
   {
      if (press)
         g_ff.toggled = !g_ff.toggled;
      want = g_ff.toggled;
   }
   /* The right stick held to the right: while it is held, in either mode
    * (not in the local link, where a sideways stick switches machines). */
   if ((fired & FIRE_FF_STICK) && !local)
      want = 1;
   if ((session || local) && want)
   {
      /* Both consoles of a session run in step: there is no 3x of that.
       * Nor of the local link, where one press would drive two games. */
      if (press || !g_ff.locked_told)
         toast_now(session ? "Fast forward is off during a link session"
                           : "Fast forward is off while two games are linked");
      g_ff.locked_told = 1;
      want = 0;
      g_ff.toggled = 0;
   }
   else if (!want)
      g_ff.locked_told = 0;
   if (want != g_ff.on || (want && g_ff.mode != pcfg_ff_mode()))
      ff_set(want);

   /* THE DRAW GATE (psp/ff_gate.h): decide, before the frame is emulated,
    * whether it will be drawn -- at most 30 a second for Unlimited, 60 for
    * Smooth.  A frame turned away is emulated in full but never rendered. */
   if (g_ff.uncapped)
   {
      unsigned period = g_ff.mode == PCFG_FF_SMOOTH
                      ? FF_GATE_PERIOD_SMOOTH_US : FF_GATE_PERIOD_UNLIMITED_US;
      if (period != g_ff.gate.period_us)
         ff_gate_arm(&g_ff.gate, period);
      fe_host_skip_override(!ff_gate_frame(&g_ff.gate, plat_time_us()));
   }
}

/* ---- the FPS chip and the telemetry window --------------------------------------- */

static void fps_chip_frame(void)
{
   uint64_t now;
   if (!g_pcfg.show_fps)
   {
      if (g_fps_t0 || g_fps_text[0])
      {
         g_fps_t0 = 0;
         g_fps_text[0] = '\0';
         osd_chip_fps(NULL);
      }
      return;
   }
   now = plat_time_us();
   if (!g_fps_t0)
   {
      g_fps_t0 = now ? now : 1;
      g_fps_emu = g_fps_drawn = 0;
   }
   else if (now - g_fps_t0 >= 1000000u)
   {
      /* Emulated frames a second (a healthy reading is the GBA's own 59.7,
       * or 60.0 locked to the panel) and, in brackets, new pictures drawn.
       * A window with no frames (the menu) re-arms without flashing 0.0. */
      if (g_fps_emu)
      {
         uint64_t el = now - g_fps_t0;
         unsigned fps10 = (unsigned)((uint64_t)g_fps_emu * 10000000ull / el);
         unsigned drawn = (unsigned)(((uint64_t)g_fps_drawn * 1000000ull +
                                      el / 2) / el);
         snprintf(g_fps_text, sizeof(g_fps_text), "%u.%u fps (%u)",
                  fps10 / 10, fps10 % 10, drawn);
         osd_chip_fps(g_fps_text);
      }
      g_fps_t0 = now;
      g_fps_emu = g_fps_drawn = 0;
   }
}

/* Every WINDOW iterations of the game loop, next to fe_host's own heartbeat:
 * what the first hardware runs need to answer -- does a frame fit its slot,
 * does the sound hold its cushion.  A window runs from the start of an
 * iteration to the start of the one WINDOW later, so the rate it reports is
 * the pacer's and not short by the last frame's wait. */
#define WINDOW 600

static struct { uint64_t t0; uint32_t work_sum, work_max; unsigned n, frames0;
                uint32_t swap_sum, swap_max; } g_win;

/* At the top of an iteration (g_app_frame_t0 is set). */
static void window_begin(void)
{
   audio_stats a;
   uint64_t now = g_app_frame_t0;
   unsigned frames = fe_host_frame_count();

   if (!g_win.t0)
   {
      g_win.t0 = now ? now : 1;
      g_win.frames0 = frames;
      g_win.n = 0;
      return;
   }
   if (g_win.n < WINDOW)
      return;
   audio_get_stats(&a);
   /* fps in thousandths, integers only: newlib's %f keeps a heap cache for
    * the life of the process (fe_host.c's av_info line explains). */
   fe_evt("vita_win loops=%u frames=%u fps_x1000=%u work_us=%u/%u late=%u "
          "forgiven=%u caught=%u caught_us=%u swap_us=%u/%u q=%u/%u/%u",
          g_win.n,
          frames - g_win.frames0,
          (unsigned)((uint64_t)(frames - g_win.frames0) * 1000000000ull /
                     (now - g_win.t0 ? now - g_win.t0 : 1)),
          g_win.work_sum / g_win.n, g_win.work_max, g_pace_late,
          g_pace_forgiven, g_caught, g_caught ? g_caught_us / g_caught : 0,
          g_win.swap_sum / g_win.n, g_win.swap_max, g_q_taken, g_q_glass,
          g_q_waits);
   g_caught = g_caught_us = 0;
   g_q_taken = g_q_glass = g_q_waits = 0;
   g_win.swap_sum = g_win.swap_max = 0;
   {
      /* The picture drawn on another core this window (render_thread.h):
       * frames, the thread's time, the emulator's copy at line 160, its
       * wait at the end of the frame; with render_thread = 2 the frames
       * and pixels that differed from the core's own picture. */
      {
         rt_stats_t rs;
         rt_stats(&rs);
         if (rs.frames)
            fe_evt("vita_render frames=%u render_us=%u/%u copy_us=%u "
                   "pages=%u wait_us=%u/%u checked=%u bad_frames=%u "
                   "bad_px=%u log=%u/%u", rs.frames, rs.render_avg_us,
                   rs.render_max_us, rs.copy_avg_us, rs.vram_pages,
                   rs.wait_avg_us, rs.wait_max_us, rs.checked,
                   rs.bad_frames, rs.bad_px, rs.log_entries, rs.log_overflows);
      }
      /* xBRZ drawn by the GPU this window (pictures), and the GPU's time
       * over a frame as last measured (its guard's samples, or every frame
       * with gpu_time = 1). */
      {
         static unsigned gx_last;
         unsigned gx, gn, gavg, gmax;
         int gon = vid_gpu_xbrz(&gx);
         vid_gpu_time_stats(&gn, &gavg, &gmax);
         if (gon || gx != gx_last || gn)
            fe_evt("vita_gpu xbrz=%d pictures=%u samples=%u gpu_us=%u/%u",
                   gon, gx - gx_last, gn, gavg, gmax);
         gx_last = gx;
      }
      /* The slowest of each part of drawing the picture this window
       * (video_vita.h vid_draw_parts): what makes a frame's `blt` long. */
      {
         unsigned dp[VID_DP_N];
         vid_draw_parts(dp);
         fe_evt("vita_draw look=%u remake=%u lock=%u collect=%u submit=%u "
                "stage=%u upload=%u draw=%u", dp[VID_DP_LOOK],
                dp[VID_DP_REMAKE], dp[VID_DP_LOCK], dp[VID_DP_COLLECT],
                dp[VID_DP_SUBMIT], dp[VID_DP_STAGE], dp[VID_DP_UPLOAD],
                dp[VID_DP_DRAW]);
      }
      /* The CPU upscalers' work in this window (nothing when none is
       * chosen): pictures finished and how long each took beside the game,
       * pictures dropped because the one before was not finished, the share
       * of stripes that had changed, the factor it settled on. */
      upscale_stats_t up;
      vid_upscale_stats(&up);
      if (up.jobs || up.dropped)
      {
         int ids[2], k, n = upscale_thread_ids(ids, 2);
         static const char *const tag[2] = { "up0", "up1" };
         for (k = 0; k < n; k++)
            plat_thread_watch(tag[k], ids[k]);
         fe_evt("vita_upscale filter=%s jobs=%u us=%u/%u dropped=%u "
                "scaled_pct=%u slid_pct=%u prep_us=%u helped_pct=%u "
                "factor=%u threads=%u async=%u prep=%u/%u/%u/%u/%u/%u "
                "job=%u/%u late=%u/%u/%u late_f=%u/%u/%u stepped=%u",
                vid_filter_name(vid_filter()), up.jobs, up.avg_us, up.max_us,
                up.dropped, up.scaled_pct, up.slid_pct, up.prep_us,
                up.helped_pct, up.factor, up.threads, up.async,
                up.prep_part[0], up.prep_part[1], up.prep_part[2],
                up.prep_part[3], up.prep_part[4], up.prep_part[5],
                up.job_part[0], up.job_part[1], up.late,
                up.late_slid, up.late_pct, up.late_f[0], up.late_f[1],
                up.late_f[2], up.stepped);
      }
      /* The wide picture: frames its profile took a snapshot of the game's
       * map, how many of them the picture on screen did not bear out (their
       * margins stayed black), and how well the snapshot matched. */
      if (wide_margin_px())
      {
         wide_stats_t ws;
         wide_stats(&ws);
         unsigned spx = 0, sbad = 0;
         wide_selftest_get(&spx, &sbad);
         unsigned sruns = 0, sframes = 0, sclone = 0, sticks = 0, sus = 0;
         fe_gbadual_scout_stats(&sruns, &sframes, &sclone, &sticks, &sus);
         unsigned sser = 0, sunser = 0, srate = 0;
         fe_gbadual_scout_stats2(&sser, &sunser, &srate);
         fe_evt("vita_wide profile=%d margin=%d w=%u latched=%u refused=%u "
                "held=%u checked=%u mismatched=%u worst=%u selftest=%u/%u "
                "auto=%u/%u cuts=%u oracle=%u/%u scout=%u/%u/%u learned=%u "
                "clone_us=%u scout_ticks=%u scout_us=%u ser_us=%u unser_us=%u "
                "rate_x16=%u",
                wide_profile_id(), wide_margin_px(), g_show.w, ws.latched,
                ws.refused, ws.held, ws.checked, ws.mismatched, ws.worst,
                sbad, spx, ws.auto_known, ws.auto_known + ws.auto_unknown,
                ws.cuts, ws.auto_wrong, ws.auto_right + ws.auto_wrong,
                ws.scouts, sruns, sframes, ws.scout_learned, sclone, sticks, sus,
                sser, sunser, srate);
         /* core_phase = 3: where the margins' time goes, per frame */
         if (g_sw.core_phase >= 3 && g_win.n)
            fe_evt("vita_wide_prof hook_us=%u strip_us=%u rest_us=%u",
                   wide_prof_hook_us / g_win.n, wide_prof_strip_us / g_win.n,
                   wide_prof_rest_us / g_win.n);
         wide_prof_hook_us = wide_prof_strip_us = wide_prof_rest_us = 0;
      }
      /* vita.ini up_bench = N: N times, a window apart, every filter is
       * timed on the frame on screen (a tenth of a second each time). */
      if (g_sw.up_bench > 0 && g_last.pix && !link_session_up() &&
          fe_host_frame_count() > 900)
      {
         char line[320];
         g_sw.up_bench--;
         upscale_bench(g_last.pix, g_last.w, g_last.h, g_last.pitch, line,
                       sizeof(line));
         fe_evt("upscale_bench %ux%u us: %s", g_last.w, g_last.h, line);
         app_pace_reset();
      }
   }
   fe_evt("vita_audio level=%u avg=%u target=%u follow_ppm=%d under=%u "
          "starved=%u dropped=%u trims=%u port_err=%u",
          a.level, a.level_avg, a.target, a.follow_ppm, a.underruns,
          a.starved, a.dropped, a.trims, a.port_errors);
   {
      /* Where the threads ran this window: core/affinity/us run/preempted
       * by threads/by interrupts/moved between cores (plat.h). */
      char line[400];
      plat_thread_report(line, sizeof(line));
      if (line[0])
         fe_evt("vita_threads %s", line);
   }
   g_win.t0 = now ? now : 1;
   g_win.work_sum = g_win.work_max = 0;
   g_win.n = 0;
   g_win.frames0 = frames;
}

/* At its end: what the iteration's work cost, the wait not included. */
static void window_note(uint32_t work_us)
{
   g_win.work_sum += work_us;
   if (work_us > g_win.work_max)
      g_win.work_max = work_us;
   g_win.n++;
}

/* ---- boot --------------------------------------------------------------------- */

/* The GBA core takes its ROM cache in 1 MiB blocks up to this cap; the
 * default is 32, i.e. 32 MiB for every game.  A Vita holds any cartridge
 * whole, so the cap is simply the cartridge: an 8 MiB game then leaves the
 * other 24 MiB to the heap (a cable session's second cartridge, artwork).
 * A core global, like the PSP frontend's use of it. */
extern uint32_t gamepak_buffer_cap;
/* The GBA cartridge clock's time source (gba_memory.c): the console's own
 * clock instead of the C library's idea of it, as the PSP frontend sets. */
extern time_t (*gpsp_wallclock)(void);
/* The GBA core's ROM-read observer (gba_memory.c): the loading screen. */
extern void (*gpsp_rom_load_progress)(unsigned loaded, unsigned total);

/* Who the game is, for the link menus and the cheat files: the GBA header
 * code (0xAC), the GB cartridge title (0x134), its name under roms/ (the
 * key of linkmode.txt) and a name for screens. */
static void game_identity(void)
{
   const char *base = base_name(g_game.rom_path), *t;
   size_t n = strlen(g_app_rom_dir);
   FILE *f = fopen(g_game.rom_path, "rb");
   char *dot;

   g_game.code[0] = g_game.gb_title[0] = '\0';
   if (f)
   {
      if (g_game.console == FE_CONSOLE_GBA)
      {
         if (fseek(f, 0xAC, SEEK_SET) == 0 && fread(g_game.code, 1, 4, f) == 4)
            g_game.code[4] = '\0';
         else
            g_game.code[0] = '\0';
      }
      else if (fseek(f, 0x134, SEEK_SET) == 0 &&
               fread(g_game.gb_title, 1, 16, f) == 16)
         g_game.gb_title[16] = '\0';
      else
         g_game.gb_title[0] = '\0';
      fclose(f);
   }
   /* A game of roms/ is keyed by its name under it; any other (an extra
    * folder, a full path in autoboot.txt) by its file's name -- the browser
    * keys its Link mode edit the same way (ui_psp.c rom_link_key). */
   if (n && !strncmp(g_game.rom_path, g_app_rom_dir, n) &&
       g_game.rom_path[n] == '/')
      snprintf(g_game.key, sizeof(g_game.key), "%s", g_game.rom_path + n + 1);
   else
      snprintf(g_game.key, sizeof(g_game.key), "%s", base);
   t = g_game.code[0] ? link_game_title(g_game.code) : NULL;
   snprintf(g_game.name, sizeof(g_game.name), "%s", t ? t : base);
   dot = t ? NULL : strrchr(g_game.name, '.');
   if (dot && dot != g_game.name)
      *dot = '\0';
}

/* Everything a fresh game starts from, whatever the last one left. */
static void game_state_reset(void)
{
   g_quit = QUIT_NONE;
   g_quit_why[0] = '\0';
   memset(&g_ff, 0, sizeof(g_ff));
   g_blit_suppress = 0;
   g_app_drew = 0;
   g_app_keys = 0;
   g_prev_pad = pspcompat_pad();   /* what is held now is not a new press */
   g_pad_block = g_prev_pad;
   g_chord_frames = 0;
   chord_guard_reset();
   g_menu_was_open = 0;
   g_view_error = NULL;
   g_view_err_part = ERR_NONE;
   memset(&g_tap, 0, sizeof(g_tap));
   g_tap.wait = 1;
   g_rs_side = 0;
   g_rs_left = g_rs_right = 0;
   g_cheat_btn = 0;
   g_wake_menu = 0;
   g_shot_pending = 0;
   g_faults_seen = 0;
   g_snap_have = 0;
   g_fps_t0 = 0;
   g_fps_emu = g_fps_drawn = 0;
   memset(&g_win, 0, sizeof(g_win));
   app_frame_forget();
   vid_set_vsync(1);
}

/* Boot `rom_path` as `console`.  0 = the game is running. */
static int game_boot(const char *rom_path, fe_console_t console)
{
   fe_host_config cfg;
   long size = 0;
   int rc;

   memset(&g_game, 0, sizeof(g_game));
   game_state_reset();
   if (file_size(rom_path, &size) != 0 ||
       path_fmt(g_game.rom_path, sizeof(g_game.rom_path), "%s", rom_path) != 0)
   {
      fe_evt("boot_refused reason=no_rom path=%s", rom_path);
      return -1;
   }
   /* A GBA cartridge begins with a 192-byte header.  A shorter file is not
    * one, and the GBA core, which takes any file, would run its zero-filled
    * remainder for ever. */
   if (console == FE_CONSOLE_GBA && size < 0xC0)
   {
      fe_evt("boot_refused reason=not_a_cartridge size=%ld path=%s", size,
             rom_path);
      return -1;
   }
   /* ...and one that is long enough but is not a cartridge either (a
    * download cut short and padded, another kind of file under this name)
    * does worse than run nothing: the core follows its bytes as code, and
    * on the twins that ends in a crash (x86) or in a frame that never
    * finishes (ARM).  Every cartridge, homebrew included, begins with a
    * branch (0xEA in the entry word's top byte) and carries 0x96 at 0xB2;
    * a file with NEITHER is refused -- one sign is enough to be let in. */
   if (console == FE_CONSOLE_GBA)
   {
      unsigned char hdr[0xC0];
      FILE *f = fopen(rom_path, "rb");
      size_t got = f ? fread(hdr, 1, sizeof(hdr), f) : 0;
      if (f)
         fclose(f);
      if (got != sizeof(hdr) || (hdr[3] != 0xEA && hdr[0xB2] != 0x96))
      {
         fe_evt("boot_refused reason=not_a_cartridge header=%02x/%02x "
                "path=%s", got > 3 ? hdr[3] : 0, got > 0xB2 ? hdr[0xB2] : 0,
                rom_path);
         return -1;
      }
   }
   g_game.console = console;
   g_game.rom_bytes = size;

   /* PER-CONSOLE DISPLAY PROFILE (docs/DISPLAY-FEATURES.md): the running
    * ROM's console decides scale, filter, ambient bars and DMG palette.
    * Before ui_loading_begin, which bakes the ambient texture. */
   g_pcfg.console = (int)console;
   pcfg_display_select((int)console);
   vid_set_mode(g_pcfg.scale, g_pcfg.filter);
   ui_host_look_changed();
   vid_ambient_mode(g_pcfg.ambient);
   vid_ambient_fallback(ui_console_shade((int)console));
   fe_evt("display_profile console=%s scale=%s filter=%s ambient=%d "
          "gb_palette=%d", pcfg_console_tag((int)console),
          vid_scale_name(g_pcfg.scale), vid_filter_name(g_pcfg.filter),
          g_pcfg.ambient, g_pcfg.gb_palette);

   /* The battery save and the states sit beside the ROM, named as the PSP
    * names them: the ROM's extension replaced, except that a GB or GBC game
    * keeps it in the name, so "Game.gb" and "Game.gba" never share a file. */
   if (psp_rom_path_suffix(g_game.save_path, sizeof(g_game.save_path),
                           rom_path,
                           console == FE_CONSOLE_GBA ? ".sav" :
                           console == FE_CONSOLE_GBC ? ".gbc.sav" : ".gb.sav")
          != 0 ||
       psp_rom_path_suffix(g_game.state_path, sizeof(g_game.state_path),
                           rom_path, psp_state_slot1_suffix(console)) != 0)
   {
      fe_evt("boot_refused reason=sidecar_path_too_long rom=%s", rom_path);
      return -1;
   }
   ui_set_state_base(g_game.state_path);
   ui_loading_begin(rom_path);
   game_identity();
   fe_evt("game rom=%s size=%ld console=%s code=%s save=%s", rom_path, size,
          console_name(console),
          g_game.code[0] ? g_game.code : g_game.gb_title, g_game.save_path);

   memset(&cfg, 0, sizeof(cfg));
   cfg.console        = console;
   cfg.rom_path       = g_game.rom_path;
   cfg.system_dir     = app_bios_dir();  /* gba_bios.bin, if there is one   */
   cfg.save_path      = g_game.save_path;
   cfg.video_frame    = sink_video;
   cfg.audio_frames   = sink_audio;
   cfg.input_bitmask  = sink_input;
   cfg.time_us        = plat_time_us;
   cfg.yield_thread   = sink_yield;
   if (g_sw.core_phase)
   {
      /* the clock first: the core refuses a level without one */
      smc_prof_clock = app_prof_clock;
      core_phase_set_level((unsigned)g_sw.core_phase);
      cfg.core_phase = app_core_phase;
   }
   cfg.wallclock      = plat_wallclock;
   cfg.gb_palette     = g_pcfg.gb_palette;
   cfg.gbc_raw_colors = !g_pcfg.gbc_color_correction;
   cfg.boot_status    = loading_stage;

   if (console == FE_CONSOLE_GBA)
   {
      uint32_t blocks = (uint32_t)((size + (1 << 20) - 1) >> 20);
      gamepak_buffer_cap = blocks < 1 ? 1 : blocks > 32 ? 32 : blocks;
      gpsp_wallclock = plat_wallclock;
#ifdef GBA_LINK
      /* The link partner's machine (instance B, docs/GBA-LINK-MIRROR.md) is
       * loaded with the game and stays loaded: the simplest of fe_host's
       * arrangements, and the one the local link needs.  The PSP lends B's
       * memory to the ROM cache between sessions and reloads the game
       * around each one (section 19) because it has 24 MiB; none of that
       * is needed here.
       *   gba_dual          B exists
       *   gba_dual_session  ...and waits for a session (or the local link)
       *                     instead of running as the bench pair
       *   gba_dual_late 0   loaded at boot, not at session start
       *   gba_link_nolend   B's memory is never the ROM cache's */
      cfg.gba_dual         = 1;
      cfg.gba_dual_session = 1;
      cfg.gba_dual_late    = 0;
      cfg.gba_link_nolend  = 1;
      /* faster link play (g_sw above) */
      cfg.gba_dual_cable_fast = g_sw.gba_link_fast >= 0 &&
                                g_sw.gba_link_fast <= 3 ? g_sw.gba_link_fast
                                                        : 0;
      cfg.gba_dual_partner_audio = g_sw.gba_link_headless_audio == 2 ? -1 :
                                   g_sw.gba_link_headless_audio ? 0 : 1;
      if (g_sw.link_bench)
      {
         static char hash_path[APP_PATH_CAP];
         cfg.gba_dual_session = 0;      /* the bench pair, not a session */
         cfg.gba_dual_mirror  = 1;
         if (path_fmt(hash_path, sizeof(hash_path), "%s/log/linkhash.txt",
                      g_app_root) == 0)
            cfg.gba_dual_hash = hash_path;
      }
#endif
   }
   /* The link layer's hooks for this boot: the stall absorption, the bulk
    * lane, the partner-cartridge lookup. */
   link_host_config(&cfg);
   ui_linkmode_load(g_app_rom_dir);

   if (!g_sw.drc)
      fe_host_option_set("gpsp_drc", "disabled");
   gpsp_rom_load_progress = loading_progress;
   rc = fe_host_boot(&cfg);
   gpsp_rom_load_progress = NULL;
   /* The link menus' view of this game: can it host a Link Cable room in
    * this boot (instance B loaded)? */
   mgift_vita_game(rc == 0 && console == FE_CONSOLE_GBA ? g_game.code
                                                        : NULL);
   g_game_console = rc == 0 ? (int)console : -1;
   ui_set_game(console == FE_CONSOLE_GBA ? g_game.code : NULL,
               console == FE_CONSOLE_GBA ? NULL : g_game.gb_title,
               g_game.key, rc == 0 ? fe_host_gba_cable_ready() : 0);
   if (rc != 0)
   {
      ui_loading_finish(0);
      fe_host_session_reset();
      fe_evt("boot_failed rc=%d rom=%s", rc, rom_path);
      return -1;
   }
   ui_loading_finish(1);
   g_game_loaded = 1;
   g_faults_seen = fe_host_guest_faults();   /* the count is the process's */
   fe_evt("gba_cable_ready=%d", fe_host_gba_cable_ready());
   plat_memory_report();        /* now the core has taken its memory */

   g_audio_ok = audio_start(fe_host_sample_rate(), pace_speed_x1e6()) == 0;
   fe_evt("audio on=%d in_rate=%u out_rate=%u grain=%u speed_x1e6=%u",
          g_audio_ok, fe_host_sample_rate(), AUDIO_OUT_RATE, AUDIO_GRAIN,
          pace_speed_x1e6());
   if (!g_audio_ok)
      osd_toast("Audio output unavailable");
   io_start();

   {
      int n = cheats_load(g_app_root, g_game.rom_path, console);
      fe_evt("cheats_load n=%d enabled=%d match=%d db=\"%s\"", n,
             cheats_enabled_count(), cheats_db_match(), cheats_db_game());
   }
   link_game_begin();
   link_local_game_begin();
   osd_session_chip_refresh();
   rewind_configure(g_pcfg.rewind, g_pcfg.rewind_mb);
   rewind_game_begin();
   /* the game list's preview of this game (clip_vita.h) */
   clip_game_begin(g_pcfg.previews && g_sw.clips);
   fe_evt("clip_begin on=%d", g_pcfg.previews && g_sw.clips);

   app_pace_reset();
   return 0;
}

/* The game goes away: cheats and the link first (while the core still
 * exists), then the sound, the writer thread, and only then fe_host flushes,
 * closes the save and unloads the core -- the order the save's safety
 * depends on.  Afterwards the process is where a fresh boot's is, for the
 * next game: `hold` keeps the "Returning to game list" picture moving while
 * all that happens (docs/INPROC-RETURN.md). */
static void game_unload(int hold)
{
   unsigned frames = fe_host_frame_count();

   if (hold)
   {
      app_backdrop_from_snapshot();
      ui_return_hold();
   }
   if (g_ff.on)
      ff_set(0);
   ctl_turbo_stop(&g_turbo);    /* the game was left: nothing auto-fires */
   mgift_vita_stop(1);          /* the cart leaves with the game */
   mgift_vita_game(NULL);
   g_game_console = -1;
   cheats_flush();
   cheats_button(0);
   link_local_game_end();       /* the pair first: it runs on this game */
   link_game_end();
   if (hold) ui_return_tick();
   audio_stop();
   io_stop();
   rewind_game_end();           /* its memory back, before the next game's */
   clip_game_end();
   if (hold) ui_return_tick();
   fe_evt("game_unload frames=%u", frames);
   /* The second GBA machine goes with the game.  fe_host_shutdown leaves it
    * alone (on the PSP it comes and goes with a session); here every GBA
    * game brings it up at boot, so without this each game played would
    * leave its buffers behind.  Before the first machine's: the two share
    * the cartridge. */
   if (fe_gbadual_loaded())
      fe_gbadual_unload();
   fe_host_shutdown();
   g_game_loaded = 0;
   cheats_unload();
   if (hold) ui_return_tick();

   /* A fresh boot's per-game state. */
   fe_host_session_reset();
   osd_reset();
   ui_close();
   vid_ambient_drop();
   vid_set_vsync(1);
   app_frame_forget();
}

void app_quit_game(const char *why)
{
   if (g_quit == QUIT_NONE)
      g_quit = QUIT_GAMELIST;
   snprintf(g_quit_why, sizeof(g_quit_why), "%s", why ? why : "");
   fe_evt("quit_game why=\"%s\"", g_quit_why);
}

/* ---- the in-game menu's actions -------------------------------------------------
 * ui_frame() returns what the player asked for; the loop does it.  One case
 * per action: a new action is a new case. */
static void menu_action(ui_action act)
{
   if (act != UI_ACT_NONE)
   {
      g_view_error = NULL;       /* a reason lasts until the next action */
      g_view_err_part = ERR_NONE;
   }
   switch (act)
   {
   case UI_ACT_NONE:
   case UI_ACT_RESUME:           /* ui_frame closed the menu itself */
      break;

   case UI_ACT_SAVESTATE:
   {
      /* The Save states page stays open and reports on its own plate
       * (docs/UI-OVERLAY.md section 13); the toast is for any other page. */
      int rc = link_locks_states() ? -1
             : state_save_slot((unsigned)ui_state_slot());
      if (!ui_state_save_done(rc))
         osd_toast(rc == 0 ? "State saved" : "State save FAILED");
      break;
   }
   case UI_ACT_LOADSTATE:
   {
      /* Loaded: the menu closes, back into the game. */
      int rc = link_locks_states() ? -1
             : state_load_slot((unsigned)ui_state_slot(), "menu");
      if (!ui_state_load_done(rc))
         osd_toast(rc == 0 ? "State loaded" : "No state to load");
      break;
   }

   /* ---- Play together: the network (app_vita.h part A, link_vita.c).
    * What each expects is ui_host_hooks.h section 7.  The two ways of
    * linking exclude each other, and that is decided HERE, so neither link
    * file needs to know the other. */
   case UI_ACT_NET_HOST:
      /* The menu stays open on its waiting page and reads the outcome from
       * the view (link_view_set): the room, or the reason. */
      if (link_local_active())
         g_view_error = "Stop the two-player game on this Vita first.";
      else if (mgift_vita_active())
         g_view_error = "Stop Mystery Gift first.";
      else
      {
         g_view_err_part = ERR_NET;
         link_action(1);
      }
      break;
   case UI_ACT_NET_JOIN:
      /* The menu has closed; what to join is *ui_join_target(). */
      if (link_local_active())
         toast_now("Stop the two-player game on this Vita first");
      else if (mgift_vita_active())
         toast_now("Stop Mystery Gift first");
      else
         link_action(0);
      break;
   case UI_ACT_NET_DISCONNECT:
      /* Stop hosting, cancel a Host still coming up, or end the session. */
      link_disconnect();
      break;

   /* ---- Play together: two players on this Vita (part B, local_vita.c).
    * The menu stays open on its page for all three. */
   case UI_ACT_NET_LOCAL:
      if (link_net_busy())
         g_view_error = "End the network game first.";
      else
      {
         g_view_err_part = ERR_LOCAL;
         link_local_start();
         app_wide_apply();
      }
      break;
   case UI_ACT_NET_LOCAL_STOP:
      link_local_stop();
      app_wide_apply();
      break;
   case UI_ACT_NET_LOCAL_SWAP:
      link_local_next_view();
      break;

   /* ---- Mystery Gift (vita/mgift_vita.c): the screen stays open and
    * reads its two lines from mgift_ui_line1/2. */
   case UI_ACT_NET_MGIFT:
      mgift_vita_start();
      break;
   case UI_ACT_NET_MGIFT_STOP:
      mgift_vita_stop(0);
      break;

   case UI_ACT_GAMELIST:
      /* The full clean unload (save flushed, threads stopped), then the
       * game list again, in this process. */
      pcfg_save();
      g_quit = QUIT_GAMELIST;
      break;
   case UI_ACT_RELAUNCH:
      /* On the PSP: a setting that is only read at boot changed, restart
       * the app.  The Vita's UI never returns it (nothing there is latched
       * that way); were it to, the game list reads its settings each time
       * it opens, so going back to it is the restart. */
      pcfg_save();
      g_quit = QUIT_GAMELIST;
      break;
   case UI_ACT_EXIT:
      g_quit = QUIT_EXIT;
      break;
   }
}

/* ---- the game loop ---------------------------------------------------------------- */

/* The tail of every iteration: the OSD over whatever was drawn, the
 * present, the wait for this frame's refresh.
 *
 * PRESENT, THEN WAIT -- the reverse of the PSP's loop, which waits for the
 * blank and swaps inside it because its swap is immediate and would tear.
 * The Vita's is a flip queued for the next blank: queued as soon as the
 * frame is drawn it is on the glass at the very next refresh, one whole
 * refresh sooner than a flip queued after waiting.  That refresh is input
 * latency, and it costs nothing to give back.  The pacer then waits for the
 * blank the flip happens on, so the next frame's pad is read just after
 * it. */
static void frame_present(int paced)
{
   if (g_app_drew)
      osd_draw();
   if (paced)
   {
      /* The frame is done and its refresh is not due: lend the time to the
       * CPU upscalers (upscale_vita.h), up to the point from which a stripe
       * could make this frame late.  Nothing when no upscaler is at work,
       * or when the frame used its time itself. */
      uint64_t spent = plat_time_us() - g_app_frame_t0;
      /* ...and before that, to the wide view's scout (gpsp/wide.h): the
       * second machine looks a second ahead of the player's, a few of its
       * frames in each of ours, when the wide view asks and nothing else
       * has that machine. */
      const unsigned scout_until = SCOUT_UNTIL_US;
      if (g_wide_mode == WIDE_AUTO && spent < scout_until && !g_ff.uncapped)
      {
#ifdef VITA
         fe_gbadual_scout_tick((unsigned)(scout_until - spent), plat_time_us,
                               g_app_keys);
#else
         /* (a twin's clock stands still while it computes: a fixed share) */
         fe_gbadual_scout_tick(0, NULL, g_app_keys);
#endif
         spent = plat_time_us() - g_app_frame_t0;
      }
      if (spent < HELP_UNTIL_US)
         upscale_help((unsigned)(HELP_UNTIL_US - spent));
   }
   if (paced || g_app_drew)      /* unlimited: only when there is a picture */
   {
      /* (how long the present waits: the display queue full) */
      const uint64_t ts = plat_time_us();
      uint32_t d;
      plat_present_hook();
      vid_swap();                /* presents only if a whole frame was drawn */
      d = (uint32_t)(plat_time_us() - ts);
      g_win.swap_sum += d;
      if (d > g_win.swap_max)
         g_win.swap_max = d;
   }
   if (paced)
      pace_frame();
}

/* The session as the menus draw it: the network's view, the local link's
 * word on top (its error), and main_vita.c's own refusals. */
static void link_view_set(void)
{
   ui_link_view v;
   const char *err;

   memset(&v, 0, sizeof(v));
   v.conn = g_pcfg.net_conn;
   v.rtt_ms = -1;
   v.signal_pct = -1;
   link_ui_view(&v);
   if (g_view_err_part != ERR_NET)
      v.error = NULL;
   err = v.error;
   link_local_ui_view(&v);
   if (g_view_err_part != ERR_LOCAL)
      v.error = err;
   if (v.state == UI_LINK_OFF && (!v.error || !v.error[0]))
      v.error = g_view_error;
   ui_set_link(&v);
}

/* A tap on the screen while the menu is closed: 1 once, with the native
 * (960x544) point, when a finger that came down has gone up again within a
 * third of a second without wandering.  Nothing when config.ini says
 * touch = 0 (the UI's switch for a panel that misbehaves). */
/* A deliberate press of a finger, not only a flick of one: up to 0.6 s on
 * the glass, rolling up to 5 mm (the first limits, 0.33 s and 3.5 mm, were a
 * practised tap's; the owner's first taps on a console opened nothing). */
#define TAP_FRAMES 36
#define TAP_SLOP   48

static int touch_tap(int *nx, int *ny)
{
   pc_touch t;

   if (!g_pcfg.touch)
      return 0;
   pspcompat_touch(&t);
   if (g_tap.wait)
   {
      g_tap.wait = t.down;
      g_tap.down = 0;
      return 0;
   }
   if (t.down)
   {
      if (!g_tap.down)
      {
         g_tap.down = 1;
         g_tap.moved = 0;
         g_tap.frames = 0;
         g_tap.x = t.nx;
         g_tap.y = t.ny;
      }
      else
      {
         g_tap.frames++;
         if (abs(t.nx - g_tap.x) > TAP_SLOP || abs(t.ny - g_tap.y) > TAP_SLOP)
            g_tap.moved = 1;
      }
      return 0;
   }
   if (!g_tap.down)
      return 0;
   g_tap.down = 0;
   if (g_tap.moved || g_tap.frames > TAP_FRAMES)
   {
      /* In the log: "the menu does not open" has to be told apart from a
       * panel that reports nothing (a touch that is not a tap is rare). */
      fe_evt("touch_not_tap x=%d y=%d frames=%d moved=%d", g_tap.x, g_tap.y,
             g_tap.frames, g_tap.moved);
      return 0;
   }
   *nx = g_tap.x;
   *ny = g_tap.y;
   return 1;
}

/* One iteration with the in-game menu open: the core is paused, the link
 * keeps being served, the UI draws and says what to do. */
static void menu_frame(unsigned pad)
{
   ui_action act;

   link_view_set();
   act = ui_frame(pad, link_locks_states(), link_session_info());
   g_app_drew = 1;
   menu_action(act);
   /* The chips are the game's: over a menu they would sit on its header
    * and its footer (which carry the session themselves: HOSTING / LINKED).
    * Toasts still show. */
   osd_chip_session(NULL);
   osd_chip_ff(NULL);
   osd_chip_fps(NULL);
   frame_present(1);
   osd_chips_restore();
}

/* The pause overlay (its shortcut) and the wake overlay (after a suspend)
 * are one screen: the frozen frame with a plate over it, modal, "Continue"
 * or "Quit to game list". */
static void wake_overlay(void)
{
   int choice;

   app_snapshot_take();
   choice = ui_wake_menu(g_snap_have ? g_snap : NULL, (int)g_snap_w,
                         (int)g_snap_h, base_name(g_game.rom_path));
   g_wake_menu = 0;                /* also one asked for while it was up */
   fe_evt("wake_menu choice=%s", choice ? "game_list" : "continue");
   if (choice)
      g_quit = QUIT_GAMELIST;
   g_prev_pad = pspcompat_pad();   /* the button that chose is not the game's */
   g_pad_block = g_prev_pad;
   g_tap.wait = 1;
   g_fps_t0 = 0;
   audio_flush();
   app_pace_reset();
}

static void game_loop(void)
{
   while (g_running && g_quit == QUIT_NONE)
   {
      unsigned pad, pad_new, fired, frames;
      int session, local, runs, i, rs_side, rs_flick, rw_want, tx = 0, ty = 0;
      plat_pad pp;
      const char *rw_note;

      g_app_frame_t0 = plat_time_us();
      g_app_drew = 0;
      g_catchup_ok = 0;           /* (set again where the core runs) */
      window_begin();
      power_service();

      pad = pspcompat_pad();
      pad_new = pad & ~g_prev_pad;
      g_prev_pad = pad;
      g_app_pad = pad;
      /* Every remappable shortcut, evaluated once from this frame's pad
       * (psp/ctl_map.c); the context gates stay at each use below. */
      fired = ctl_shortcuts(&g_pcfg.controls, pad, pad_new);
      g_pad_block &= pad;
      g_pad_block |= chord_took(fired, pad);
      g_app_keys = ctl_game_mask(&g_pcfg.controls,
                                 chord_guard(pad & ~g_pad_block, pad));

      /* The right stick is no control of a Game Boy: pulled down it is
       * the "button" of a cheat cartridge (the few codes that act only
       * while it is held; cheats_vita.h), flicked sideways it switches
       * which machine of the local link the pad drives. */
      plat_pad_read(&pp);
      if (g_sw.pad_trace > 0)
      {
         static unsigned raw_was = ~0u, pad_was = ~0u;
         if (pp.buttons != raw_was || pad != pad_was)
         {
            g_sw.pad_trace--;
            fe_evt("pad_trace frame=%u raw=%08x pad=%08x keys=%04x ui=%d",
                   fe_host_frame_count(), pp.buttons, pad, g_app_keys,
                   ui_active());
            raw_was = pp.buttons;
            pad_was = pad;
         }
      }
      rs_side = pp.rx < 128 - RSTICK_PUSH || pp.rx > 128 + RSTICK_PUSH;
      rs_flick = rs_side && !g_rs_side && !ui_active();
      g_rs_side = rs_side;
      /* ...and held to the left it rewinds, once it has been there a tenth
       * of a second (a thumb that brushes the stick does not undo a jump),
       * for as long as it stays. */
      g_rs_left = pp.rx < 128 - RSTICK_PUSH ? g_rs_left + 1 : 0;
      g_rs_right = pp.rx > 128 + RSTICK_PUSH ? g_rs_right + 1 : 0;
      if (g_rs_right >= RSTICK_REWIND_HOLD)
         fired |= FIRE_FF_STICK;
      rw_want = (fired & CTL_FIRE_REWIND) || g_rs_left >= RSTICK_REWIND_HOLD;
      if (rw_want)               /* its chord contains fast forward's */
      {
         fired &= ~(CTL_FIRE_FF_HELD | CTL_FIRE_FF_PRESS | FIRE_FF_STICK);
         g_ff.toggled = 0;
      }
      if ((pp.ry > 128 + RSTICK_PUSH) != g_cheat_btn)
      {
         g_cheat_btn = !g_cheat_btn;
         cheats_button(g_cheat_btn);
      }

      /* ---- the two link layers (app_vita.h has the order) ----------------- */
      app_wide_apply();           /* the pair may have ended by itself */
      local = link_local_active();
      if (local)
      {
         /* The partner's machine has a pad of its own where a second
          * controller exists (plat.h: which port); else local_vita.c
          * switches the one pad between the two machines, and takes it
          * from the player's (g_app_keys) while it drives the partner's. */
         plat_pad p2;
         int have2 = plat_pad_read2(&p2);
         /* (with two pads too: the view is then whose sound is heard and
          * which picture is the large one) */
         if (rs_flick)
            link_local_next_view();
         link_local_pads(g_app_keys,
                         have2 ? ctl_game_mask(&g_pcfg.controls,
                                               pad_psp_bits(&p2)) : 0, have2);
      }
      link_frame();
      link_local_frame();
      session = link_session_up();
      if (g_quit != QUIT_NONE)
         break;                  /* app_quit_game from inside a link layer */

      /* Turbo: one step per pad sample, for the frame about to run.  Off
       * in a link (both consoles must get the same buttons from the same
       * presses; upstream turns it off there too), in the menu and while
       * rewinding; a Hold button still down starts afresh afterwards. */
      /* Mystery Gift: the phone's parcel and the cart, every turn (the
       * menu may be open on its page); the shortcut (Controls > Mystery
       * Gift, SELECT+DOWN) toggles it when no other button is held, and
       * its buttons reach neither the game nor another shortcut until they
       * are let go (psp/mgift_shortcut.h's rule). */
      mgift_vita_frame();
      {
         static unsigned mg_consumed;
         unsigned chord = g_pcfg.controls.bind[CTL_SC_MGIFT];
         unsigned conflicts = 0xF3F9u & ~chord;
         if (mg_consumed)
         {
            if (!(pad & mg_consumed))
               mg_consumed = 0;
            else
               g_app_keys &= ~ctl_game_mask(&g_pcfg.controls, mg_consumed);
         }
         else if (chord && !ui_active() && g_game_console == FE_CONSOLE_GBA &&
                  (pad & chord) == chord && !(pad & conflicts))
         {
            mg_consumed = chord;
            g_pad_block |= chord;
            g_app_keys &= ~ctl_game_mask(&g_pcfg.controls, chord);
            if (mgift_vita_active())
               mgift_vita_stop(0);
            else
               mgift_vita_start();
         }
      }
      if (ui_active() || rw_want)
         ctl_turbo_stop(&g_turbo);
      else if (g_turbo.live || ctl_turbo_bound(&g_pcfg.controls))
         ctl_turbo_pad(&g_pcfg.controls, &g_turbo, pad & ~g_pad_block,
                       pad_new & ~g_pad_block, fe_host_frame_count(),
                       !session && !local);

      /* ---- the in-game menu ---------------------------------------------- */
      if (ui_active())
      {
         g_menu_was_open = 1;
         menu_frame(pad);
         if (plat_quit_requested())
            g_running = 0;
         continue;
      }
      if (g_menu_was_open)
      {
         /* It closed (Resume, a loaded state, O on the top page). */
         g_menu_was_open = 0;
         g_pad_block = pad;
         g_app_keys = 0;
         g_tap.wait = 1;         /* nor is the finger that closed it a tap */
         g_fps_t0 = 0;           /* the time in the menu is not a frame rate */
         rewind_configure(g_pcfg.rewind, g_pcfg.rewind_mb);   /* Settings */
         cheats_flush();
         app_pace_reset();
         fe_evt("menu_closed frame=%u", fe_host_frame_count());
      }
      {
         /* START and SELECT going down or up, in the log (a person's
          * presses: a few lines a minute). */
         static unsigned ss_was;
         unsigned ss = pad & (PSP_CTRL_START | PSP_CTRL_SELECT);
         if (ss != ss_was)
            fe_evt("pad_menu_keys pad=%04x was=%x", pad, ss_was);
         ss_was = ss;
      }
      if ((pad & (PSP_CTRL_START | PSP_CTRL_SELECT)) ==
          (PSP_CTRL_START | PSP_CTRL_SELECT))
      {
         /* A quarter of a second by the frame count, or by the clock when
          * frames are slow (two machines at 26 a second made it a second's
          * hold on a console). */
         if (!g_chord_frames)
            g_chord_t0 = g_app_frame_t0;
         if (++g_chord_frames >= MENU_CHORD_FRAMES ||
             g_app_frame_t0 - g_chord_t0 >= MENU_CHORD_US)
         {
            menu_open("chord");
            continue;            /* its first frame is the next iteration */
         }
      }
      else
      {
         /* Let go too soon: said in the log, so a report of "the menu does
          * not open" can be told from a chord that never reached us. */
         if (g_chord_frames)
            fe_evt("menu_chord_short frames=%u", g_chord_frames);
         g_chord_frames = 0;
      }

      /* ---- the finger ------------------------------------------------------ */
      if (touch_tap(&tx, &ty))
      {
         vid_rect_t r;
         vid_picture_rect(g_show.w, g_show.h, &r);
         if (local)
         {
            /* The other machine's picture: the pad drives that one now.
             * The one already in hand: the menu, as with one game (found on
             * a console: with taps taken for switching there was no way to
             * the menu but the chord). */
            if (link_local_tap(tx, ty) == 2 && g_sw.touch_menu && !pad)
            {
               menu_open("touch");
               continue;
            }
         }
         else if (g_sw.touch_menu && !pad && !link_net_busy() && g_last.pix &&
                  tx >= r.x && tx < r.x + r.w && ty >= r.y && ty < r.y + r.h)
         {
            /* A tap on the game's picture opens the menu, which is the
             * touch screen's from there on.  Conservative: a short still
             * tap, on the picture itself, no button held (a thumb that
             * brushes the glass in the middle of play is not one), and
             * never while the network is in use. */
            menu_open("touch");
            continue;
         }
         else if (!local)
            fe_evt("tap_ignored x=%d y=%d pad=%04x busy=%d frame=%d "
                   "rect=%d,%d,%d,%d touch_menu=%d", tx, ty, pad,
                   link_net_busy(), g_last.pix != NULL, r.x, r.y, r.w, r.h,
                   g_sw.touch_menu);
      }

      /* ---- shortcuts ------------------------------------------------------- */
      if ((fired & CTL_FIRE_SHOT) && !ui_capturing())
         g_shot_pending = 1;
      if (fired & CTL_FIRE_PAUSE)
      {
         if (session)
            toast_now("Pause is off during a link session");
         else
            g_wake_menu = 1;
      }
      /* Quick save / quick load: slot 1, the menu has all five. */
      if (fired & CTL_FIRE_SAVE)
      {
         if (link_locks_states())
            toast_now("Cannot save a state during a link session");
         else
            toast_now(state_save_slot(1) == 0 ? "State saved" : "Save failed");
      }
      else if (fired & CTL_FIRE_LOAD)
      {
         if (link_locks_states())
            toast_now("Cannot load a state during a link session");
         else
            toast_now(state_load_slot(1, "chord") == 0 ? "State loaded"
                                                        : "No saved state");
      }
      /* A state picked on the game list's shelf: once, as soon as the core
       * has run a frame. */
      if (g_autoload_slot && fe_host_frame_count() >= 1)
      {
         int slot = g_autoload_slot;
         g_autoload_slot = 0;
         if (state_load_slot((unsigned)slot, "shelf") != 0)
            toast_now("Could not load state %d", slot);
      }
      /* Not on the press that asked for the pause screen: with the default
       * bindings (TRIANGLE, and SELECT+TRIANGLE) that press is both, and a
       * player who pauses does not mean to change the picture.  (The PSP
       * does both, for the sake of its 3.0 behaviour.) */
      if ((fired & CTL_FIRE_VIDEO) && !(fired & CTL_FIRE_PAUSE))
      {
         const char *name = vid_cycle_preset();
         g_pcfg.scale  = vid_scale_mode();
         g_pcfg.filter = vid_filter();
         pcfg_display_save();    /* this console's profile only */
         fe_evt("video_mode scale=%s filter=%s", vid_scale_name(g_pcfg.scale),
                vid_filter_name(g_pcfg.filter));
         toast_now("Video: %s", name);
      }
      /* (not `local`: the local link fast-forwards both of its machines) */
      ff_frame(fired, session, 0);

      /* ---- pause / wake: modal, over the frozen frame ---------------------- */
      if (g_wake_menu)
      {
         wake_overlay();
         continue;
      }
      fps_chip_frame();

      /* ---- rewind (rewind_vita.h): while its control is held the game runs
       * backwards -- the step's own frame is run and drawn in there -- and
       * continues from the frame on screen when it is let go.  Not in a
       * link of either kind, nor in the bench pair: a state is one machine. */
      i = rewind_frame(rw_want && !local, session || local ||
                       link_locks_states() || g_sw.link_bench);
      if ((rw_note = rewind_notice()) != NULL)
         toast_now("%s", rw_note);
      if (i)
      {
         g_pad_block = pad;      /* its buttons are not the game's afterwards */
         g_fps_t0 = 0;
         if (plat_quit_requested())
            g_running = 0;
         frame_present(1);
         continue;
      }

      /* ---- where the GBA's picture is drawn (render_thread.h): on another
       * core, unless a link has two machines to keep in step */
      rt_frame_begin(g_game.console == FE_CONSOLE_GBA && !session && !local &&
                     !g_sw.link_bench && !link_owns_picture()
                     ? g_sw.render_thread : RT_OFF);

      /* ---- catching up: the frames the last one fell behind by, not drawn */
      g_catchup_ok = g_pcfg.full_speed && !g_ff.on && !session && !local &&
                     !g_sw.link_bench && !link_owns_picture();
      if (g_catchup && g_catchup_ok)
      {
         const unsigned n = g_catchup < CATCHUP_PER_FRAME ? g_catchup
                                                          : CATCHUP_PER_FRAME;
         const uint64_t t0 = plat_time_us();
         unsigned k;
         for (k = 0; k < n; k++)
         {
            fe_host_skip_override(1);
            g_blit_suppress = 1;
            fe_host_run_frame();
            rewind_capture();
            plat_frame_ran();
            audio_frame();
         }
         fe_host_skip_override(-1);
         g_blit_suppress = 0;
         /* their refreshes are paid for */
         if (g_sw.pace_mode == PACE_VSYNC)
            g_app_vc_target += n;
         else
            g_next_ns += (uint64_t)n * GBA_FRAME_NS;
         g_caught += n;
         g_caught_us += (unsigned)(plat_time_us() - t0);
      }
      g_catchup = 0;

      /* ---- run the core ------------------------------------------------------ */
      runs = g_ff.mult ? 3 : 1;
      for (i = 0; i < runs; i++)
      {
         g_blit_suppress = (i != runs - 1);
         /* 3x: the two frames before the one shown are not drawn at all
          * (they were drawn and thrown away: the picture's whole cost) */
         if (g_blit_suppress)
            fe_host_skip_override(1);
         fe_host_run_frame();    /* -> sink_video, sink_audio */
         if (g_blit_suppress)
            fe_host_skip_override(-1);
         rewind_capture();
         plat_frame_ran();
         /* A game being played keeps the console awake.  The menus and
          * the game list do not: left alone there it dims and sleeps, as
          * the owner set it to. */
         plat_power_tick();
         if (!g_ff.on || g_pcfg.ff_audio)
            audio_frame();
      }
      g_blit_suppress = 0;
      frames = fe_host_frame_count();
      /* The preview keeps the game as it is played: not fast forward, not
       * a link of either kind (their timing is not to be touched), not the
       * LINKING screen. */
      if (!g_ff.on && !session && !local && !g_sw.link_bench &&
          !link_owns_picture())
         clip_frame(g_last.pix, g_last.w, g_last.h, g_last.pitch,
                    g_game_keys);
      else if (session || local)
         clip_game_end();     /* a link: its memory back, no preview */

      /* The LINKING screen covers the game while a session is set up. */
      if (link_draw())
         g_app_drew = 1;
      /* A battery save made during fast forward must not wait for it to end
       * to reach the card: the writer runs below this thread. */
      (void)fe_host_sram_ff_nudge(g_ff.on);
      {
         unsigned faults = fe_host_guest_faults();
         if (faults != g_faults_seen)
         {
            /* The core reset the game after a jump into nothing. */
            g_faults_seen = faults;
            cheats_apply();
            toast_now("Emulation fault - game was reset");
         }
      }
      keys_script_frame(fe_host_frame_count());
      if (g_shot_pending)
      {
         g_shot_pending = 0;
         screenshot_write();
      }

      window_note((uint32_t)(plat_time_us() - g_app_frame_t0));
      if (plat_quit_requested() ||
          (g_sw.autoexit_frames && frames >= g_sw.autoexit_frames))
         g_running = 0;
      frame_present(!g_ff.uncapped);
   }
}

/* ---- main ------------------------------------------------------------------------- */

int main(int argc, char **argv)
{
   static char rom_path[APP_PATH_CAP];
   char path[APP_PATH_CAP];
   int first = 1, games = 0;

   if (plat_init(argc, argv) != 0)
      plat_exit(1);
   if (roots_init() != 0)
   {
      plat_console("GBAdhoc: the data folder's path is not usable\n");
      plat_exit(1);
   }

   path_fmt(path, sizeof(path), "%s/log/gbadhoc.log", g_app_root);
   if (fe_evt_init(path, 0) != 0)
      plat_console("GBAdhoc: cannot open the log file\n");
   fe_evt_set_clock(evt_clock);
   fe_evt("boot app=GBAdhoc version=%s platform=vita root=%s", GBADHOC_VERSION,
          g_app_root);
   read_switches();
   rom_roots_init();
   fe_evt("switches pace_mode=%d drc=%d autoexit_frames=%u link_bench=%d "
          "io_thread=%d refresh_mhz=%u", g_sw.pace_mode, g_sw.drc,
          g_sw.autoexit_frames, g_sw.link_bench, g_sw.io_thread,
          plat_refresh_mhz());
   plat_memory_report();
   if (g_sw.xbrz_dist)
      upscale_xbrz_dist(g_sw.xbrz_dist);
   if (g_sw.up_slide != 2)
      upscale_slide_mode(g_sw.up_slide);
   vid_set_tex_mem(g_sw.tex_mem);
   vid_set_gpu_xbrz(g_sw.gpu_xbrz);
   vid_set_gpu_time(g_sw.gpu_time);
   upscale_fade_drop(g_sw.fade_drop);
   plat_thread_watch("main", 0);
   if (g_sw.main_cpu >= 0)
      fe_evt("main_cpu core=%d rc=%d", g_sw.main_cpu,
             plat_thread_pin(g_sw.main_cpu));
   wide_selftest_set(g_sw.wide >= 0 && (g_sw.wide & 8));
   wide_set_profiles(!(g_sw.wide >= 0 && (g_sw.wide & 32)));
   fe_gbadual_scout_log(g_sw.wide >= 0 && (g_sw.wide & 16));
   app_wide_apply();
   fe_evt("switches2 up_bench=%d xbrz_dist=%d wide=%d vm_probe=%d pad_trace=%d "
          "keys_script=%d up_slide=%d main_cpu=%d tex_mem=%d drain=%d "
          "core_phase=%d fade_drop=%d render_thread=%d gpu_xbrz=%d "
          "gpu_time=%d mhz=%u mhz_measured=%u",
          g_sw.up_bench, g_sw.xbrz_dist, g_sw.wide, g_sw.vm_probe,
          g_sw.pad_trace, g_sw.keys_script, g_sw.up_slide, g_sw.main_cpu,
          g_sw.tex_mem, g_sw.drain, g_sw.core_phase, g_sw.fade_drop,
          g_sw.render_thread, g_sw.gpu_xbrz, g_sw.gpu_time, plat_arm_mhz(),
          plat_arm_mhz_measured());
   if (g_sw.gpu_probe == 1)
      vid_gpu_probe();
   vid_set_gpu_debug(g_sw.gpu_probe >= 2 ? g_sw.gpu_probe : 0);
   vid_set_gpu_view(g_sw.gpu_view);
   if (g_sw.vm_probe > 0)
   {
      static char path[APP_PATH_CAP];
      if (path_fmt(path, sizeof(path), "%s/log/vm_probe.txt", g_app_root) == 0)
         plat_vm_probe(g_sw.vm_probe, path);
   }

   pcfg_load(g_app_config);
   /* What every system dialog needs once per process -- the keyboard a
    * cheat's name is typed on, the ad hoc connection screen: here, before
    * either can be asked for (pspcompat.c; nothing on the twin). */
   pspcompat_dialogs_init();
   /* The twin's finger and keyboard come from its script (nothing on the
    * console, where pspcompat.c reads the panel itself). */
   pspcompat_host_touch(plat_script_touch);
   pspcompat_host_ime(plat_script_ime);
   vid_init();
   vid_set_mode(g_pcfg.scale, g_pcfg.filter);
   fe_evt("video backend=%s", vid_backend_name());
   ui_set_thumb_buffer(g_thumbs);
   pspcompat_host_vblank_hook(modal_tick);

   if (path_fmt(path, sizeof(path), "%s%scheats.db", plat_app_dir(),
                plat_app_dir()[strlen(plat_app_dir()) - 1] == ':' ? "" : "/")
          == 0)
   {
      int n = cheats_db_open(path);
      fe_evt("cheats_db games=%d path=%s", n, path);
   }
   if (path_fmt(path, sizeof(path), "%s%sgamedb.txt", plat_app_dir(),
                plat_app_dir()[strlen(plat_app_dir()) - 1] == ':' ? "" : "/")
          == 0)
      fe_evt("gamedb records=%d path=%s", gamedb_open(path), path);
   link_boot();
   art_start();

   while (g_running)
   {
      fe_console_t console = (fe_console_t)g_pcfg.console;
      int slot = 0;

      /* ---- which game ---- */
      if (first && rom_from_autoboot(rom_path, sizeof(rom_path)) == 0)
      {
         console = console_for(rom_path);
         fe_evt("autoboot rom=%s", rom_path);
      }
      else
      {
         int br = ui_browser(g_app_rom_dir, rom_path, sizeof(rom_path), &slot,
                             &console);
         if (br > 0)
            continue;            /* a setting the list reads when it opens */
         if (br != 0)
         {
            fe_evt("browser exit rc=%d", br);
            break;
         }
      }
      first = 0;
      if (!g_running)
         break;

      /* ---- load ---- */
      artfetch_pause(1);         /* the game has the card and the network */
      if (game_boot(rom_path, console) != 0)
      {
         artfetch_pause(0);
         app_notice("This game could not be started",
                    base_name(rom_path),
                    "The file may be damaged or not a game for this console.",
                    6000);
         continue;
      }
      games++;
      g_autoload_slot = slot >= 1 && slot <= PSP_STATE_SLOT_COUNT ? slot : 0;
      fe_evt("session game=%d slot=%d", games, g_autoload_slot);
      keys_script_start();
      g_hint_owed = 0;
      if (g_pcfg.hints > 0)
      {
         /* A new player's first games: the two things nothing on screen
          * says once the game is up (config.ini hints counts them down,
          * menu_open). */
         g_hint_owed = 1;
         osd_toast(g_pcfg.touch && g_sw.touch_menu
                   ? "Menu: tap the screen, or hold START + SELECT together"
                   : "Menu: hold START + SELECT together");
         osd_toast(g_pcfg.rewind
                   ? "Right stick: hold left to rewind, right to fast-forward"
                   : "Right stick: hold right to fast-forward");
         fe_evt("hints shown have=%d", g_pcfg.hints);
      }

      /* ---- play ---- */
      game_loop();
      rt_frame_begin(RT_OFF);      /* the next game starts drawing as before */

      /* ---- unload ---- */
      {
         int to_list = g_running && g_quit == QUIT_GAMELIST;
         char why[sizeof(g_quit_why)];
         snprintf(why, sizeof(why), "%s", g_quit_why);
         fe_evt("game_end reason=%s",
                !g_running ? "exit" : g_quit == QUIT_EXIT ? "menu_exit"
                                                           : "game_list");
         if (!g_snap_have || !ui_active())
            app_snapshot_take();   /* the menu's own is the clean picture */
         scene_save();
         clip_save_game();
         keys_script_end();
         game_unload(to_list);
         artfetch_pause(0);
         if (!to_list)
            break;
         if (why[0])
            app_notice("The game was closed", why, NULL, 6000);
      }
   }

   fe_evt("shutdown games=%d", games);
   rt_shutdown();
   artfetch_stop();
   link_shutdown();
   pcfg_save();
   vid_term();
   fe_evt("exit ok");
   fe_evt_close();
   plat_exit(0);
   return 0;
}
