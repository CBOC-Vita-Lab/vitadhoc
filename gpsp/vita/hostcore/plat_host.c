/* plat_host.c -- plat.h for a Linux process: the headless twin of the Vita
 * app (x86-64, and armhf under qemu-arm).
 *
 * TWO CLOCKS.
 *   simulated (the default)  The clock only moves when the app waits
 *       (plat_wait_vblank, plat_sleep_us) or reports an emulated frame
 *       (plat_frame_ran adds --work-us), so a run takes as long as the
 *       emulation needs and is reproducible bit for bit: the display's
 *       refresh, the audio port's 48 kHz consumption and the pad script are
 *       all functions of that clock, never of the wall.  The "audio thread"
 *       is the app's step function called once per grain of simulated time.
 *   --realtime               CLOCK_MONOTONIC.  Waits sleep, the audio step
 *       runs on a real thread paced by the port's rate, a refresh is 1/60 s
 *       of the wall.  Two twins on one machine then share one time -- what a
 *       link session over loopback needs.  Not reproducible.
 *
 * A FRAME, everywhere below, is one refresh of the display: the script and
 * the screenshots are addressed by the refresh counter (plat_vcount), which
 * is what the game loop's pacer and the UI's modal screens both advance by
 * one per picture.  (Fast forward runs several emulated frames in one.)  A
 * simulated suspend does not move the script: the refreshes slept through
 * are not counted.
 *
 *   picture   --shot F:file.png writes the composed 960x544 frame
 *             (vid_dump_ge) at the first presentation at or after frame F
 *   audio     --wav file: everything the port was given, the whole run
 *   pad       --input SCRIPT / --input-file FILE, see below
 *   worker    synchronous by default; --worker runs a real pthread
 *
 * --app DIR is where the package's own files are (the Vita's app0:, i.e.
 * cheats.db); by default the tree's gpsp/vita/assets.
 *
 * Usage: gbadhoc_host --root DIR [--app DIR] [--frames N] [--realtime]
 *        [--shot F:file.png]... [--shot-dir DIR] [--wav file.wav]
 *        [--input SCRIPT] [--input-file FILE]
 *        [--refresh-mhz 60000] [--work-us 3000] [--slow F:N:US] [--flip-queue N]
 *        [--gpu-progs] [--gpu-us US]
 *        [--suspend F] [--worker]
 *        [--echo] [--heap-free-mb N]
 *
 * THE SCRIPT: entries separated by commas or newlines ('#' starts a comment
 * in a file):
 *
 *     FRAME:BUTTONS[:DURATION]     hold BUTTONS for DURATION frames (2)
 *     FRAME:touch=X/Y[:DURATION]   a finger on the screen at native X, Y
 *                                  (960x544) for DURATION frames (3: a tap)
 *     FRAME:drag=X0/Y0/X1/Y1:DURATION   ...moving from one point to another
 *     FRAME:type=TEXT              what the system keyboard returns the
 *                                  next time it is opened (a few frames
 *                                  after it opens); "type=" alone cancels
 *     FRAME:shot=FILE              a screenshot (as --shot; a relative FILE
 *                                  goes under --shot-dir)
 *     FRAME:suspend                the console sleeps for ten seconds
 *     FRAME:pad2=on | pad2=off     the second controller is connected /
 *                                  taken away (hot-plug: with these in the
 *                                  script it exists as the latest one says,
 *                                  and before the first, the opposite)
 *     FRAME:quit                   end the run (as --frames)
 *
 * FRAME is absolute, or "+N": N frames after the previous entry ENDED.
 * BUTTONS are the Vita's, joined by '+', in any case:
 *     cross circle square triangle  up down left right  l r  start select
 *     lstick-up lstick-down lstick-left lstick-right     (the left stick)
 *     rstick-up rstick-down rstick-left rstick-right     (the right stick)
 * and, for the cores' own tests, the GBA's A and B (= cross and circle, the
 * default binding).  "p2." before the first name puts the whole entry on a
 * second controller (plat_pad_read2: the local link's partner), which
 * exists from the moment a script names it.  Example:
 *     --input "120:cross,+30:down:1,+10:start+select:20,+40:shot=menu.png"
 */
#define _GNU_SOURCE
#include "../plat.h"
#include "../video_vita.h"
#include "fe_evt.h"
#include "fe_util.h"

#include <ctype.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#define MAX_SHOTS   400
#define MAX_PRESS   1024
#define SHOT_PATH   512

/* Stick deflections ride in the high bits of a press's button word. */
#define HOST_LS_UP    0x10000u
#define HOST_LS_DOWN  0x20000u
#define HOST_LS_LEFT  0x40000u
#define HOST_LS_RIGHT 0x80000u
#define HOST_RS_UP    0x100000u
#define HOST_RS_DOWN  0x200000u
#define HOST_RS_LEFT  0x400000u
#define HOST_RS_RIGHT 0x800000u

#define MAX_TOUCH   128
#define MAX_TYPE    32
#define TYPE_CAP    80
/* The keyboard "types" for this many frames before it returns its text. */
#define IME_FRAMES  12

#ifndef HOST_APP_DIR
#define HOST_APP_DIR NULL        /* hostcore/Makefile: the tree's assets/ */
#endif

static struct
{
   const char *root, *app, *shot_dir;
   unsigned    frames;            /* 0 = run until the script quits */
   unsigned    refresh_mhz;
   unsigned    work_us;
   int         worker, echo, realtime;
   const char *wav;
   struct { unsigned frame; char path[SHOT_PATH]; int done; } shot[MAX_SHOTS];
   unsigned    nshot;
   struct { unsigned frame, len; uint32_t buttons; int pad2; } press[MAX_PRESS];
   unsigned    npress;
   int         have_pad2;
   struct { unsigned frame, len; int x0, y0, x1, y1; } touch[MAX_TOUCH];
   unsigned    ntouch;
   struct { unsigned frame; char text[TYPE_CAP]; int cancel; } type[MAX_TYPE];
   unsigned    ntype;
   unsigned    suspend_at[8], nsuspend;
   struct { unsigned frame; int on; } plug[16];
   unsigned    nplug;
   unsigned    quit_at;           /* 0 = none */
   unsigned    heap_free_mb;      /* --heap-free-mb: plat_heap_free's answer
                                   * (0 = no figure) */
   /* --slow F:N:US: emulated frames F .. F+N-1 cost US each instead of
    * --work-us (a console's slow moment: the recompiler meeting new code) */
   unsigned    slow_at, slow_n, slow_us;
   /* --flip-queue N: a present waits while N flips are already queued, one
    * flip a refresh -- the console's display queue (libvita2d: three
    * buffers, two swaps pending at most).  0 (the default): a present never
    * waits, as the twin always did. */
   unsigned    flip_queue;
} opt = { .root = "gbadhoc-root", .refresh_mhz = 60000, .work_us = 3000 };

static uint64_t sim_ns;           /* the simulated clock */
static uint64_t real_t0_ns;       /* --realtime: CLOCK_MONOTONIC at start */
static uint64_t slept_ns;         /* simulated suspends, in total */
static unsigned power_pending;
static int      suspend_phase;    /* 1 = SUSPEND delivered, RESUME is next */
static unsigned frames_ran;       /* emulated frames (plat_frame_ran) */
static unsigned presents;         /* plat_present_hook calls */

/* ---- the clock -------------------------------------------------------------- */

static uint64_t real_ns(void)
{
   struct timespec ts;
   clock_gettime(CLOCK_MONOTONIC, &ts);
   return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static uint64_t now_ns(void)
{
   return opt.realtime ? real_ns() - real_t0_ns : sim_ns;
}

static uint64_t refresh_ns(void)
{
   return 1000000000000ull / opt.refresh_mhz;
}

/* The refresh the script and the screenshots count: sleeps taken out. */
static unsigned script_frame(void)
{
   return (unsigned)((now_ns() - slept_ns) / refresh_ns());
}

/* ---- audio: the port and its thread ------------------------------------------ */

static FILE    *wav_fp;
static unsigned wav_rate, wav_grain;
static uint64_t wav_frames;
static uint32_t wav_crc;
static int64_t  wav_peak;
static int    (*audio_step)(void);
static uint64_t grains_done;      /* simulated: grains the port has taken */

static pthread_t     au_thread;
static volatile int  au_run;
static uint64_t      au_next_ns;  /* realtime: when the port takes the next */

static void put32(FILE *f, uint32_t v) { fwrite(&v, 4, 1, f); }
static void put16(FILE *f, uint16_t v) { fwrite(&v, 2, 1, f); }

static void wav_header(FILE *f, uint32_t rate, uint32_t frames)
{
   fseek(f, 0, SEEK_SET);
   fwrite("RIFF", 1, 4, f); put32(f, 36 + frames * 4);
   fwrite("WAVEfmt ", 1, 8, f); put32(f, 16); put16(f, 1); put16(f, 2);
   put32(f, rate); put32(f, rate * 4); put16(f, 4); put16(f, 16);
   fwrite("data", 1, 4, f); put32(f, frames * 4);
   fseek(f, 0, SEEK_END);
}

/* The file is the run's, not one game's: the app opens and closes its port
 * around every game, and they all land in one WAV. */
int plat_audio_open(unsigned rate, unsigned grain)
{
   wav_rate = rate;
   wav_grain = grain;
   if (opt.wav && !wav_fp)
   {
      wav_fp = fopen(opt.wav, "wb");
      if (wav_fp)
         wav_header(wav_fp, rate, 0);
   }
   return 0;
}

int plat_audio_write(const int16_t *stereo)
{
   unsigned i;
   wav_crc = fe_crc32(wav_crc, stereo, wav_grain * 4);
   wav_frames += wav_grain;
   for (i = 0; i < wav_grain * 2; i++)
   {
      int v = stereo[i] < 0 ? -stereo[i] : stereo[i];
      if (v > wav_peak)
         wav_peak = v;
   }
   if (wav_fp)
      fwrite(stereo, 4, wav_grain, wav_fp);
   if (opt.realtime)
   {
      /* The port takes a grain every grain/rate seconds: this is the audio
       * thread's clock, as sceAudioOutOutput is on the console. */
      uint64_t now = real_ns(), period = 1000000000ull * wav_grain / wav_rate;
      if (!au_next_ns || now > au_next_ns + 4 * period)
         au_next_ns = now;
      au_next_ns += period;
      if (au_next_ns > now)
      {
         struct timespec ts;
         uint64_t d = au_next_ns - now;
         ts.tv_sec = (time_t)(d / 1000000000ull);
         ts.tv_nsec = (long)(d % 1000000000ull);
         nanosleep(&ts, NULL);
      }
   }
   return 0;
}

void plat_audio_close(void)
{
   if (wav_fp)
   {
      wav_header(wav_fp, wav_rate, (uint32_t)wav_frames);
      fflush(wav_fp);
   }
}

static void *au_main(void *arg)
{
   (void)arg;
   while (au_run && audio_step() == 0)
      ;
   return NULL;
}

int plat_audio_thread_start(int (*step)(void))
{
   audio_step = step;
   if (opt.realtime)
   {
      au_run = 1;
      au_next_ns = 0;
      if (pthread_create(&au_thread, NULL, au_main, NULL) != 0)
      {
         au_run = 0;
         audio_step = NULL;
         return -1;
      }
      return 0;
   }
   grains_done = sim_ns * wav_rate / wav_grain / 1000000000ull;
   return 0;
}

void plat_audio_thread_stop(void)
{
   if (opt.realtime && au_run)
   {
      au_run = 0;
      pthread_join(au_thread, NULL);
   }
   audio_step = NULL;
}

/* Simulated time: the port consumes one grain every grain/rate seconds. */
static void sim_advance(uint64_t to_ns)
{
   if (to_ns <= sim_ns)
      return;
   sim_ns = to_ns;
   if (audio_step)
   {
      uint64_t due = sim_ns * wav_rate / wav_grain / 1000000000ull;
      while (grains_done < due && audio_step)
      {
         grains_done++;
         if (audio_step() != 0)
            break;
      }
   }
}

/* ---- the command line --------------------------------------------------------- */

static uint32_t button_named(const char *s, size_t n)
{
   static const struct { const char *name; uint32_t bit; } tbl[] = {
      { "cross", PLAT_BTN_CROSS }, { "circle", PLAT_BTN_CIRCLE },
      { "square", PLAT_BTN_SQUARE }, { "triangle", PLAT_BTN_TRIANGLE },
      { "up", PLAT_BTN_UP }, { "down", PLAT_BTN_DOWN },
      { "left", PLAT_BTN_LEFT }, { "right", PLAT_BTN_RIGHT },
      { "l", PLAT_BTN_L }, { "r", PLAT_BTN_R },
      { "start", PLAT_BTN_START }, { "select", PLAT_BTN_SELECT },
      { "lstick-up", HOST_LS_UP }, { "lstick-down", HOST_LS_DOWN },
      { "lstick-left", HOST_LS_LEFT }, { "lstick-right", HOST_LS_RIGHT },
      { "rstick-up", HOST_RS_UP }, { "rstick-down", HOST_RS_DOWN },
      { "rstick-left", HOST_RS_LEFT }, { "rstick-right", HOST_RS_RIGHT },
      /* the GBA's, under the default binding */
      { "a", PLAT_BTN_CROSS }, { "b", PLAT_BTN_CIRCLE } };
   size_t i;
   for (i = 0; i < sizeof(tbl) / sizeof(tbl[0]); i++)
      if (strlen(tbl[i].name) == n && !strncasecmp(tbl[i].name, s, n))
         return tbl[i].bit;
   fprintf(stderr, "unknown button '%.*s'\n", (int)n, s);
   exit(2);
}

static void add_shot(unsigned frame, const char *path, size_t n)
{
   if (opt.nshot >= MAX_SHOTS || n >= SHOT_PATH)
   {
      fprintf(stderr, "too many screenshots, or a path too long\n");
      exit(2);
   }
   opt.shot[opt.nshot].frame = frame;
   memcpy(opt.shot[opt.nshot].path, path, n);
   opt.shot[opt.nshot].path[n] = '\0';
   opt.shot[opt.nshot].done = 0;
   opt.nshot++;
}

/* `cursor`: where the previous entry ended, for "+N". */
static void parse_input(const char *script, unsigned *cursor)
{
   const char *p = script;
   for (;;)
   {
      char *end;
      unsigned frame, len = 0;
      int rel = 0;
      size_t n;

      while (*p == ',' || *p == '\n' || *p == '\r' || *p == ' ' || *p == '\t')
         p++;
      if (*p == '#')
      {
         p += strcspn(p, "\n");
         continue;
      }
      if (!*p)
         return;
      if (*p == '+') { rel = 1; p++; }
      frame = (unsigned)strtoul(p, &end, 10);
      if (end == p || *end != ':')
         break;
      if (rel)
         frame += *cursor;
      p = end + 1;
      n = strcspn(p, ",\n\r#");
      while (n && (p[n - 1] == ' ' || p[n - 1] == '\t'))
         n--;
      if (n > 5 && !strncasecmp(p, "shot=", 5))
         add_shot(frame, p + 5, n - 5);
      else if ((n > 6 && !strncasecmp(p, "touch=", 6)) ||
               (n > 5 && !strncasecmp(p, "drag=", 5)))
      {
         int drag = (p[0] | 0x20) == 'd', v[4] = { 0, 0, 0, 0 }, k;
         const char *q = strchr(p, '=') + 1;
         for (k = 0; k < (drag ? 4 : 2); k++)
         {
            v[k] = (int)strtol(q, &end, 10);
            if (end == q || (k + 1 < (drag ? 4 : 2) && *end != '/'))
               break;
            q = end + 1;
         }
         if (k < (drag ? 4 : 2) || opt.ntouch >= MAX_TOUCH)
            break;
         len = 3;
         if (*end == ':')
            len = (unsigned)strtoul(end + 1, &end, 10);
         if (end != p + n || !len)
            break;
         opt.touch[opt.ntouch].frame = frame;
         opt.touch[opt.ntouch].len = len;
         opt.touch[opt.ntouch].x0 = v[0];
         opt.touch[opt.ntouch].y0 = v[1];
         opt.touch[opt.ntouch].x1 = drag ? v[2] : v[0];
         opt.touch[opt.ntouch].y1 = drag ? v[3] : v[1];
         opt.ntouch++;
      }
      else if (n >= 5 && !strncasecmp(p, "type=", 5))
      {
         if (opt.ntype >= MAX_TYPE || n - 5 >= TYPE_CAP)
            break;
         opt.type[opt.ntype].frame = frame;
         memcpy(opt.type[opt.ntype].text, p + 5, n - 5);
         opt.type[opt.ntype].text[n - 5] = '\0';
         opt.type[opt.ntype].cancel = n == 5;
         opt.ntype++;
      }
      else if (n == 4 && !strncasecmp(p, "quit", 4))
         opt.quit_at = frame ? frame : 1;
      else if (n == 7 && !strncasecmp(p, "suspend", 7))
      {
         if (opt.nsuspend < 8)
            opt.suspend_at[opt.nsuspend++] = frame;
      }
      else if ((n == 7 && !strncasecmp(p, "pad2=on", 7)) ||
               (n == 8 && !strncasecmp(p, "pad2=off", 8)))
      {
         if (opt.nplug < 16)
         {
            opt.plug[opt.nplug].frame = frame;
            opt.plug[opt.nplug++].on = n == 7;
         }
      }
      else
      {
         uint32_t buttons = 0;
         const char *q = p, *stop = p + n;
         int pad2 = 0;
         len = 2;
         if (n > 3 && !strncasecmp(q, "p2.", 3))
         {
            pad2 = opt.have_pad2 = 1;
            q += 3;
         }
         for (;;)
         {
            size_t k = 0;
            while (q + k < stop && q[k] != '+' && q[k] != ':')
               k++;
            buttons |= button_named(q, k);
            q += k;
            if (q >= stop || *q != '+')
               break;
            q++;
         }
         if (q < stop && *q == ':')
         {
            len = (unsigned)strtoul(q + 1, &end, 10);
            if (end != stop)
               break;
         }
         if (opt.npress >= MAX_PRESS)
         {
            fprintf(stderr, "the script has more than %d presses\n", MAX_PRESS);
            exit(2);
         }
         opt.press[opt.npress].frame = frame;
         opt.press[opt.npress].len = len;
         opt.press[opt.npress].buttons = buttons;
         opt.press[opt.npress].pad2 = pad2;
         opt.npress++;
      }
      *cursor = frame + len;
      p += strcspn(p, ",\n");
   }
   fprintf(stderr, "bad script near '%.40s'\n", p);
   exit(2);
}

static void parse_input_file(const char *path, unsigned *cursor)
{
   FILE *f = fopen(path, "rb");
   char *buf;
   long n;
   if (!f)
   {
      fprintf(stderr, "cannot read %s\n", path);
      exit(2);
   }
   fseek(f, 0, SEEK_END);
   n = ftell(f);
   fseek(f, 0, SEEK_SET);
   buf = malloc((size_t)n + 1);
   if (!buf || fread(buf, 1, (size_t)n, f) != (size_t)n)
   {
      fprintf(stderr, "cannot read %s\n", path);
      exit(2);
   }
   buf[n] = '\0';
   fclose(f);
   parse_input(buf, cursor);
   free(buf);
}

int plat_init(int argc, char **argv)
{
   unsigned cursor = 0;
   int i;
   for (i = 1; i < argc; i++)
   {
      const char *a = argv[i], *v = i + 1 < argc ? argv[i + 1] : NULL;
      if (!strcmp(a, "--root") && v)             { opt.root = v; i++; }
      else if (!strcmp(a, "--app") && v)         { opt.app = v; i++; }
      else if (!strcmp(a, "--shot-dir") && v)
      {
         opt.shot_dir = v;
         mkdir(v, 0755);            /* one level; an existing one is fine */
         i++;
      }
      else if (!strcmp(a, "--frames") && v)      { opt.frames = (unsigned)atoi(v); i++; }
      else if (!strcmp(a, "--refresh-mhz") && v) { opt.refresh_mhz = (unsigned)atoi(v); i++; }
      else if (!strcmp(a, "--work-us") && v)     { opt.work_us = (unsigned)atoi(v); i++; }
      else if (!strcmp(a, "--flip-queue") && v)  { opt.flip_queue = (unsigned)atoi(v); i++; }
      else if (!strcmp(a, "--slow") && v)
      {
         if (sscanf(v, "%u:%u:%u", &opt.slow_at, &opt.slow_n, &opt.slow_us) != 3)
         {
            fprintf(stderr, "--slow FRAME:COUNT:US\n");
            return -1;
         }
         i++;
      }
      else if (!strcmp(a, "--wav") && v)         { opt.wav = v; i++; }
      else if (!strcmp(a, "--input") && v)       { parse_input(v, &cursor); i++; }
      else if (!strcmp(a, "--input-file") && v)  { parse_input_file(v, &cursor); i++; }
      else if (!strcmp(a, "--worker"))           opt.worker = 1;
      else if (!strcmp(a, "--heap-free-mb") && v) { opt.heap_free_mb = (unsigned)atoi(v); i++; }
      else if (!strcmp(a, "--echo"))             opt.echo = 1;
      /* vback_soft.c: run the GPU programs (shaders/) as the console
       * would, per pixel -- slow; off, the twin has none */
      else if (!strcmp(a, "--gpu-progs"))        setenv("GBADHOC_SOFT_PROGS", "1", 1);
      /* ...and what a timed present says the GPU took (vb_gpu_stats) */
      else if (!strcmp(a, "--gpu-us") && v)      { setenv("GBADHOC_SOFT_GPU_US", v, 1); i++; }
      else if (!strcmp(a, "--realtime"))         opt.realtime = 1;
      else if (!strcmp(a, "--suspend") && v)
      {
         if (opt.nsuspend < 8)
            opt.suspend_at[opt.nsuspend++] = (unsigned)atoi(v);
         i++;
      }
      else if (!strcmp(a, "--shot") && v)
      {
         const char *colon = strchr(v, ':');
         if (!colon) { fprintf(stderr, "--shot FRAME:file.png\n"); return -1; }
         add_shot((unsigned)atoi(v), colon + 1, strlen(colon + 1));
         i++;
      }
      else
      {
         fprintf(stderr, "unknown argument '%s' (see plat_host.c)\n", a);
         return -1;
      }
   }
   if (opt.refresh_mhz < 1000)
      opt.refresh_mhz = 60000;
   real_t0_ns = real_ns();
   return 0;
}

void plat_exit(int code)
{
   if (wav_fp)
   {
      wav_header(wav_fp, wav_rate, (uint32_t)wav_frames);
      fclose(wav_fp);
      wav_fp = NULL;
   }
   fprintf(stderr, "host: exit %d after %u refreshes (%u presented, %u "
           "emulated frames), %llu ms %s, audio frames=%llu crc=%08x peak=%d\n",
           code, script_frame(), presents, frames_ran,
           (unsigned long long)(now_ns() / 1000000ull),
           opt.realtime ? "of the wall" : "simulated",
           (unsigned long long)wav_frames, (unsigned)wav_crc, (int)wav_peak);
   exit(code);
}

const char *plat_data_root(void) { return opt.root; }
const char *plat_app_dir(void)
{
   return opt.app ? opt.app : HOST_APP_DIR ? HOST_APP_DIR : opt.root;
}

int plat_quit_requested(void)
{
   unsigned f = script_frame();
   return (opt.frames && f >= opt.frames) || (opt.quit_at && f >= opt.quit_at);
}

void plat_console(const char *line) { fputs(line, stderr); }

#ifdef VITASIM
/* The Vita's own report (vita/jit_vita.c), over the simulated kernel. */
#include <stdarg.h>
#include "../jit_vita.h"
static void report_line(const char *fmt, ...)
{
   char buf[256];
   va_list ap;
   va_start(ap, fmt);
   vsnprintf(buf, sizeof(buf), fmt, ap);
   va_end(ap);
   fe_evt("%s", buf);
   if (opt.echo)
      fprintf(stderr, "host: %s\n", buf);
}

void plat_memory_report(void)
{
   gbadhoc_mem_report(report_line);
}
#else
/* The caches are the core's own symbols here (MMAP_JIT_CACHE; absent from an
 * interpreter-only build, hence weak). */
extern uint8_t *rom_translation_cache __attribute__((weak));
extern uint8_t *gbab_rom_translation_cache __attribute__((weak));
extern void retro_run(void);
extern void gbab_retro_run(void) __attribute__((weak));

void plat_memory_report(void)
{
   uint8_t *ja = &rom_translation_cache ? rom_translation_cache : NULL;
   uint8_t *jb = &gbab_rom_translation_cache ? gbab_rom_translation_cache : NULL;
   long da = ja ? (long)((intptr_t)ja - (intptr_t)&retro_run) : 0;
   long db = jb ? (long)((intptr_t)jb - (intptr_t)&gbab_retro_run) : 0;
   fe_evt("mem_layout host text_a=%p jit_a=%p dist_a_kib=%ld text_b=%p "
          "jit_b=%p dist_b_kib=%ld", (void *)&retro_run, (void *)ja, da / 1024,
          &gbab_retro_run ? (void *)&gbab_retro_run : NULL, (void *)jb,
          db / 1024);
   if (opt.echo)
      fprintf(stderr, "host: jit_a=%p (%ld KiB from text) jit_b=%p (%ld KiB)\n",
              (void *)ja, da / 1024, (void *)jb, db / 1024);
}
#endif

/* ---- time, display clock, power ------------------------------------------- */

uint64_t plat_time_us(void) { return now_ns() / 1000u; }

void plat_sleep_us(uint32_t us)
{
   if (opt.realtime)
      usleep(us);
   else
      sim_advance(sim_ns + (uint64_t)us * 1000u);
}

time_t plat_wallclock(void)
{
   /* A fixed date plus the run's own time: cartridge clocks must not make
    * two simulated runs differ. */
   return (time_t)(1700000000ull + now_ns() / 1000000000ull);
}

unsigned plat_vcount(void) { return (unsigned)(now_ns() / refresh_ns()); }

void plat_wait_vblank(void)
{
   uint64_t next = (now_ns() / refresh_ns() + 1) * refresh_ns();
   if (opt.realtime)
   {
      uint64_t now = now_ns();
      if (next > now)
         usleep((useconds_t)((next - now) / 1000u) + 1);
   }
   else
      sim_advance(next);
}

unsigned plat_refresh_mhz(void) { return opt.refresh_mhz; }

unsigned plat_arm_mhz(void) { return 0; }

unsigned plat_arm_mhz_measured(void) { return 0; }

void plat_power_tick(void) { }

/* An emulated frame costs --work-us of simulated time (--slow's frames
 * cost more). */
void plat_frame_ran(void)
{
   unsigned us = opt.work_us;
   if (opt.slow_n && frames_ran >= opt.slow_at &&
       frames_ran < opt.slow_at + opt.slow_n)
      us = opt.slow_us;
   frames_ran++;
   if (!opt.realtime)
      sim_advance(sim_ns + (uint64_t)us * 1000u);
}

/* --flip-queue: when each queued flip reaches the glass. */
static uint64_t flip_at[8];
static unsigned nflip;

static void flip_queue_present(void)
{
   const uint64_t r = refresh_ns();
   uint64_t now = now_ns(), t;
   unsigned k;
   while (nflip && flip_at[0] <= now)
   {
      for (k = 1; k < nflip; k++)
         flip_at[k - 1] = flip_at[k];
      nflip--;
   }
   if (nflip >= opt.flip_queue)
   {
      /* full: the present waits for the oldest to be shown */
      if (!opt.realtime)
         sim_advance(flip_at[0]);
      now = flip_at[0];
      for (k = 1; k < nflip; k++)
         flip_at[k - 1] = flip_at[k];
      nflip--;
   }
   t = (now / r + 1) * r;            /* the next refresh, or after the last */
   if (nflip && flip_at[nflip - 1] + r > t)
      t = flip_at[nflip - 1] + r;
   if (nflip < 8)
      flip_at[nflip++] = t;
}

/* vback_soft.c's vb_present_state: the newest queued flip not yet shown
 * (bit 1), and not yet "taken" -- the queue's previous one still pending
 * (bit 0). */
int plat_host_flip_state(void)
{
   const uint64_t now = now_ns();
   unsigned k, ahead = 0;
   if (!opt.flip_queue)
      return 0;
   for (k = 0; k < nflip; k++)
      ahead += flip_at[k] > now;
   return ahead >= 2 ? 3 : ahead == 1 ? 2 : 0;
}

void plat_present_hook(void)
{
   unsigned i, f = script_frame();
   presents++;
   if (opt.flip_queue)
      flip_queue_present();
   for (i = 0; i < opt.nshot; i++)
   {
      if (opt.shot[i].done || f < opt.shot[i].frame)
         continue;
      opt.shot[i].done = 1;
      {
         char path[SHOT_PATH * 2];
         if (opt.shot_dir && opt.shot[i].path[0] != '/')
            snprintf(path, sizeof(path), "%s/%s", opt.shot_dir,
                     opt.shot[i].path);
         else
            snprintf(path, sizeof(path), "%s", opt.shot[i].path);
         if (vid_dump_ge(path) == 0)
            fe_evt("host_shot frame=%u emu=%u file=%s", f, frames_ran,
                   opt.shot[i].path);
         else
            fprintf(stderr, "host: cannot write %s\n", path);
      }
   }
}

/* A suspend is two calls apart: SUSPEND on the first at or after its frame,
 * RESUME on the next -- ten simulated seconds later, the display counter and
 * the clock having both jumped, as they do on the console. */
unsigned plat_power_info(void)
{
   return 0;
}

unsigned plat_power_events(void)
{
   unsigned e = power_pending, i;
   power_pending = 0;
   if (suspend_phase == 1)
   {
      suspend_phase = 0;
      if (!opt.realtime)
      {
         sim_ns += 10000000000ull;
         slept_ns += 10000000000ull;
         grains_done = sim_ns * (wav_rate ? wav_rate : 48000u) /
                       (wav_grain ? wav_grain : 512u) / 1000000000ull;
      }
      return e | PLAT_POWER_RESUME;
   }
   for (i = 0; i < opt.nsuspend; i++)
      if (opt.suspend_at[i] && script_frame() >= opt.suspend_at[i])
      {
         opt.suspend_at[i] = 0;
         suspend_phase = 1;
         e |= PLAT_POWER_SUSPEND;
         break;
      }
   return e;
}

/* Real time, whatever the clock: the simulated one does not move. */
void plat_vm_probe(int mode, const char *report_path)
{
   (void)mode; (void)report_path;  /* a console's question */
}

void plat_thread_yield(void)
{
   usleep(100);
}

static pthread_t bg_thread;
static int bg_on;
static void (*bg_fn)(void *);
static void *bg_arg;
static volatile int bg_ended;

static void *bg_main(void *p)
{
   (void)p;
   bg_fn(bg_arg);
   bg_ended = 1;
   return NULL;
}

int plat_bg_start(void (*fn)(void *), void *arg)
{
   if (bg_on)
      return -1;
   bg_fn = fn;
   bg_arg = arg;
   bg_ended = 0;
   if (pthread_create(&bg_thread, NULL, bg_main, NULL) != 0)
      return -1;
   bg_on = 1;
   return 0;
}

int plat_bg_join(uint32_t timeout_us)
{
   uint32_t waited = 0;
   if (!bg_on)
      return 1;
   while (!bg_ended && waited < timeout_us)
   {
      usleep(10000);
      waited += 10000;
   }
   if (!bg_ended)
   {
      pthread_detach(bg_thread);
      bg_on = 0;
      return 0;
   }
   pthread_join(bg_thread, NULL);
   bg_on = 0;
   return 1;
}

void plat_thread_sleep_us(uint32_t us)
{
   usleep(us);
}

void plat_thread_watch(const char *tag, int thid)
{
   (void)tag; (void)thid;      /* a console's question */
}

void plat_thread_report(char *buf, size_t cap)
{
   if (buf && cap)
      buf[0] = '\0';
}

int plat_thread_pin(int core)
{
   (void)core;
   return 0;
}

/* A Linux heap has no ceiling worth reporting; --heap-free-mb N plays a
 * Vita whose heap has N MiB left when it is asked (rewind's sizing). */
size_t plat_heap_free(void)
{
   return opt.heap_free_mb ? (size_t)opt.heap_free_mb << 20 : (size_t)-1;
}

/* ---- worker ---------------------------------------------------------------- */

static pthread_t       wk_thread;
static pthread_mutex_t wk_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  wk_cv = PTHREAD_COND_INITIALIZER;
static void          (*wk_step)(void);
static int             wk_run, wk_kick;

static void *wk_main(void *arg)
{
   (void)arg;
   pthread_mutex_lock(&wk_mu);
   while (wk_run)
   {
      struct timespec ts;
      clock_gettime(CLOCK_REALTIME, &ts);
      ts.tv_nsec += 20000000;                 /* real time: 20 ms */
      if (ts.tv_nsec >= 1000000000) { ts.tv_sec++; ts.tv_nsec -= 1000000000; }
      if (!wk_kick)
         pthread_cond_timedwait(&wk_cv, &wk_mu, &ts);
      wk_kick = 0;
      pthread_mutex_unlock(&wk_mu);
      wk_step();
      pthread_mutex_lock(&wk_mu);
   }
   pthread_mutex_unlock(&wk_mu);
   wk_step();                                 /* the last pass */
   return NULL;
}

int plat_worker_start(void (*step)(void), uint32_t period_us)
{
   (void)period_us;
   if (!opt.worker)
      return -1;                              /* the app stays synchronous */
   wk_step = step;
   wk_run = 1;
   wk_kick = 0;
   if (pthread_create(&wk_thread, NULL, wk_main, NULL) != 0)
   {
      wk_run = 0;
      return -1;
   }
   return 0;
}

void plat_worker_wake(void)
{
   pthread_mutex_lock(&wk_mu);
   wk_kick = 1;
   pthread_cond_signal(&wk_cv);
   pthread_mutex_unlock(&wk_mu);
}

void plat_worker_stop(void)
{
   if (!wk_run)
      return;
   pthread_mutex_lock(&wk_mu);
   wk_run = 0;
   pthread_cond_signal(&wk_cv);
   pthread_mutex_unlock(&wk_mu);
   pthread_join(wk_thread, NULL);
}

/* ---- pad ------------------------------------------------------------------- */

static void pad_at(plat_pad *pad, int pad2)
{
   unsigned i, f = script_frame();
   uint32_t b = 0;
   for (i = 0; i < opt.npress; i++)
      if (opt.press[i].pad2 == pad2 && f >= opt.press[i].frame &&
          f < opt.press[i].frame + opt.press[i].len)
         b |= opt.press[i].buttons;
   pad->buttons = b & 0xFFFFu;
   pad->lx = (b & HOST_LS_LEFT) ? 0 : (b & HOST_LS_RIGHT) ? 255 : 128;
   pad->ly = (b & HOST_LS_UP) ? 0 : (b & HOST_LS_DOWN) ? 255 : 128;
   pad->rx = (b & HOST_RS_LEFT) ? 0 : (b & HOST_RS_RIGHT) ? 255 : 128;
   pad->ry = (b & HOST_RS_UP) ? 0 : (b & HOST_RS_DOWN) ? 255 : 128;
}

void plat_pad_read(plat_pad *pad)
{
   pad_at(pad, 0);
}

/* The second controller: there from the start when the script names it
 * ("p2."), or coming and going as its pad2= entries say. */
static int pad2_present(void)
{
   unsigned i, f = script_frame();
   int on;
   if (!opt.nplug)
      return opt.have_pad2;
   on = !opt.plug[0].on;
   for (i = 0; i < opt.nplug; i++)
      if (f >= opt.plug[i].frame)
         on = opt.plug[i].on;
   return on;
}

int plat_pad_ports(void)
{
   return pad2_present() ? 2 : 1;
}

int plat_pad_read2(plat_pad *pad)
{
   if (!pad2_present())
      return 0;
   pad_at(pad, 1);
   return 1;
}

/* ---- the finger and the keyboard (pspcompat.c asks, plat.h) ------------------ */

int plat_script_touch(int *nx, int *ny)
{
   unsigned i, f = script_frame();
   for (i = 0; i < opt.ntouch; i++)
      if (f >= opt.touch[i].frame && f < opt.touch[i].frame + opt.touch[i].len)
      {
         /* a drag arrives at its end point on its last frame */
         int span = opt.touch[i].len > 1 ? (int)opt.touch[i].len - 1 : 1;
         int k = (int)(f - opt.touch[i].frame);
         *nx = opt.touch[i].x0 + (opt.touch[i].x1 - opt.touch[i].x0) * k / span;
         *ny = opt.touch[i].y0 + (opt.touch[i].y1 - opt.touch[i].y0) * k / span;
         return 1;
      }
   return 0;
}

/* The latest "type=" entry at or before now answers; none = the keyboard is
 * closed without accepting. */
const char *plat_script_ime(const char *title, const char *initial, int frames)
{
   /* pspcompat.c's own cancel marker (pspcompat.h is not included here: its
    * names are the PSP SDK's). */
   extern const char PSPCOMPAT_HOST_IME_CANCEL[];
   unsigned i, f = script_frame();
   int best = -1;
   (void)initial;
   if (frames < IME_FRAMES)
      return NULL;
   for (i = 0; i < opt.ntype; i++)
      if (opt.type[i].frame <= f)
         best = (int)i;
   fe_evt("host_ime title=\"%s\" text=\"%s\"", title ? title : "",
          best >= 0 && !opt.type[best].cancel ? opt.type[best].text : "<cancel>");
   if (best < 0 || opt.type[best].cancel)
      return PSPCOMPAT_HOST_IME_CANCEL;
   return opt.type[best].text;
}
