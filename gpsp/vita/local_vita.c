/* local_vita.c -- THE LOCAL LINK: two players on this Vita.
 *
 * Part B of app_vita.h and section 5 of ui_host_hooks.h.  Both machines of a
 * link-cable game run in this process -- the player's (the game he booted)
 * and a partner's -- with the cable between them and no network anywhere:
 * trade with yourself, or play a friend on one console.
 *
 *   GBA      frontend-common/fe_gbadual's local pair: instance A carries on
 *            from where it stands, instance B is the partner
 *   GB/GBC   fe_host's local pair (fe_host_gb_local_*): the two TGB Dual
 *            machines of a GB link session, without the session
 *
 * THE PARTNER starts as a copy of the player's machine (state and battery),
 * or powers on from a save file of its own:
 *
 *     same game       <rom>.p2.sav  (.gb.p2.sav / .gbc.p2.sav), beside the
 *                     ROM like the player's own; written when the partner's
 *                     game first saves
 *     another game    that game's OWN save, <other rom>.sav: the two
 *                     versions of a pair, each with the save the player has
 *                     for it (local.ini remembers the choice per game)
 *
 * Whatever the partner's game writes to its battery goes to that file and
 * never to the player's.  A copy that saves becomes the second save file:
 * the file it replaces is kept once as <file>.bak.
 *
 * THE PAIR IS ONE THING TO THE PLAYER.  One pad drives one machine at a
 * time -- the VIEW: right stick sideways, a tap on the other picture, the
 * menu's "Swap" -- and that machine is the one heard and, in the large +
 * small layout, the large one.  With a second controller each player keeps
 * his own machine and the view only chooses the sound and the large
 * picture.  Fast forward runs both.  A save state taken while the pair runs
 * holds BOTH machines and the cable (<state>.p2 beside the player's own
 * state file, which stays a plain state of his game), so loading it
 * continues the pair exactly, a transfer in flight included; a state
 * without one reloads the player's game and plugs the cable in again.
 *
 * SOUND.  Only the machine that is heard synthesises any (the other's
 * sound hardware runs silently: sound.h sound_synth_off).  A change of view
 * fades the old one out over a frame, leaves a frame of silence while the
 * new one's synthesis starts, and fades it in: no click, nothing mixed.
 *
 * THE PARTNER'S SAVE FILE is written like the player's: a snapshot is taken
 * on the emulation thread half a second after the partner's game stopped
 * writing its backup chip, and the I/O worker (link_local_io, from
 * main_vita.c's io_step) writes it to <file>.tmp and renames.  Where there
 * is no worker the emulation thread writes it itself.
 *
 * SPEED.  Two machines are twice the work of one.  Should a frame's work
 * come to take longer than a refresh (measured here, per frame, up to the
 * end of the pair's drawing), every other frame of BOTH machines is
 * emulated without being rendered -- the games keep their speed and their
 * sound, the picture goes to thirty a second -- until the work has been
 * well under a refresh for ten seconds.  vita.ini local_skip = 0 never,
 * 1 (default) by itself, 2 always.
 *
 * TEST SWITCHES (vita.ini, read at each game's start):
 *   local_auto = 1 | 2      start the pair by itself once the game has run
 *                           local_auto_frame frames (default 2): 1 a copy,
 *                           2 its own save
 *   local_lookahead = N     the GBA pair's lookahead in cycles (0 = default)
 *   local_gb_batch = N      the GB pair's scanline batch (0 = 154 lines)
 *   local_hash = 1          both machines' state hashes per frame to
 *                           log/localhash.txt (GBA: gl_host --hash's
 *                           format; GB: the pair's one hash)
 *   local_peek = ADDR:N     at every stop, N words (GB: bytes) of both
 *                           machines' RAM at ADDR to the log (a test ROM's
 *                           verdict)
 *   local_hear_both = 1     both machines synthesise sound all the time (the
 *                           cost of the unheard one, for measurements)
 *   local_dump = 1          the player's machine's state, as the pair starts
 *                           from it, to log/local_start.st (for a reference
 *                           run of tools/gbamirror's gl_host)
 *   local_cpu = N           every N frames, the CPU time the emulation has
 *                           cost so far (fe_host_cpu_stat: without drawing;
 *                           zeros on a console) to the log, alone or linked
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

#include "app_vita.h"
#include "ui_host_hooks.h"
#include "fe_gbadual.h"
#include "fe_evt.h"
#include "fe_util.h"
#include "libretro.h"

#define PSPCOMPAT_IMPL
#include "pspcompat.h"
#include "osd_psp.h"
#include "config_psp.h"
#include "video_vita.h"
#include "rom_paths.h"
#include "plat.h"
#include "audio_vita.h"
#include "cheats_vita.h"

/* The GB/GBC pair and the partner's-game choice, further down. */
#define LOCAL_E_UNSUPPORTED (-10)
static int  local_gb_available(void);
static int  local_gb_start(int clone, const char *partner_rom,
                           const uint8_t *save, size_t save_n);
static void local_gb_stop(void);
static int  local_gb_error(void);
static void local_gb_hear(int who);
static const uint8_t *local_gb_partner_save(uint32_t *writes, size_t *n,
                                            int fresh);
static int  local_gb_restart(int reclone);
static size_t local_gb_state_max(void);
static long local_gb_state_save(void *buf, size_t cap);
static int  local_gb_state_load(const void *buf, size_t n);
static void partner_choice_load(void);

/* ---- state ------------------------------------------------------------------ */

#define SAVE_BYTES   0x20000u          /* a GBA battery image                 */
#define P_W_MAX      240
#define P_H_MAX      160
/* The partner's sound waiting for the player's machine's frame to end: a
 * frame is at most 65536 / 59.7 = 1098 sample pairs. */
#define FIFO_FRAMES  4096u
/* A battery write is "over" when the chip has been quiet this long. */
#define SAVE_SETTLE_FRAMES 30u
/* No sign of the I/O worker for this long: write on this thread. */
#define SAVE_WORKER_FRAMES 120u

enum { SV_IDLE = 0, SV_PENDING, SV_WRITING };
enum { AU_STEADY = 0, AU_GAP, AU_IN };

static struct
{
   int active;                 /* the pair runs                               */
   int gb;                     /* ...and is the GB/GBC one                    */
   int start_req;              /* a start asked for: done in link_local_frame */
   int clone;                  /* the partner began as a copy                 */
   int view;                   /* 0 the player's machine, 1 the partner's     */
   int have2;                  /* a second controller, this iteration         */
   int have2_told;             /* ...as last announced                        */
   unsigned hint_frames;       /* "how to change games" stays up this long    */
   uint32_t keys2;             /* the partner's buttons (libretro mask)       */
   uint32_t held_block;        /* held at the last switch: not passed on      */
   unsigned iter;              /* link_local_frame calls                      */
   const char *error;          /* why the last start did not work             */

   /* the partner's picture, as its core last finished one */
   uint16_t pix[P_W_MAX * P_H_MAX];
   unsigned pw, ph;
   int have_pix;

   /* sound */
   int heard;                  /* whose sound goes out                        */
   int au_phase, au_count;
   int16_t fifo[FIFO_FRAMES * 2];
   unsigned fifo_n;            /* sample pairs queued                         */
   int16_t last_l, last_r;     /* the last pair that went out                 */

   /* the partner's save file */
   char save_path[APP_PATH_CAP];
   int  save_exists;           /* for the menu: stat'ed when it can change    */
   int  save_bak_done;         /* the file a copy replaces was kept           */
   uint32_t save_writes;       /* backup-chip writes at the last look         */
   unsigned save_quiet;        /* iterations since they last changed          */
   int  save_dirty;            /* the image differs from the file             */
   volatile int save_state;    /* SV_*                                        */
   unsigned save_pending_iter; /* when it became SV_PENDING                   */
   volatile unsigned io_calls; /* link_local_io calls: the worker is alive    */
   unsigned io_calls_seen, io_seen_iter;
   size_t save_n;              /* bytes of the image (GB: the cartridge's)    */
   uint8_t save_snap[SAVE_BYTES + 64];  /* + a GB clock trailer */
   unsigned save_count;        /* files written (telemetry)                   */
   /* the writer's word on its last write, for the emulation thread to log */
   struct { volatile int done; int ok, bak; unsigned size; } wr;

   /* drawing every other frame when the work does not fit a refresh */
   int skip_on, skip_set;
   unsigned skip_n, skip_calm;
   uint32_t skip_ema;          /* a frame's work, microseconds, smoothed      */

   /* test switches */
   int sw_skip;
   int sw_auto, sw_hash, sw_hear_both;
   unsigned sw_auto_frame, sw_lookahead, sw_peek_addr, sw_peek_n;
   unsigned sw_gb_batch;
   int sw_dump;
   unsigned sw_cpu;
   FILE *gb_hash;
} L;

/* The partner's cartridge when it is not the player's ("" = the same). */
static char g_partner_rom[APP_PATH_CAP];
static char g_partner_name[64];

static const char *base_name(const char *path)
{
   const char *s = strrchr(path, '/');
   const char *c = strrchr(path, ':');
   if (c && (!s || c > s))
      s = c;
   return s ? s + 1 : path;
}

static int file_bytes(const char *path, long *size)
{
   struct stat st;
   if (stat(path, &st) != 0 || !S_ISREG(st.st_mode))
      return -1;
   if (size)
      *size = (long)st.st_size;
   return 0;
}

/* ---- the partner's save file ------------------------------------------------- */

/* Where the partner's battery lives for the start that would happen now. */
static void save_path_set(void)
{
   fe_console_t c = g_game.console;
   L.save_path[0] = '\0';
   if (g_partner_rom[0])
      /* another cartridge: its own save, named as main_vita.c names it */
      psp_rom_path_suffix(L.save_path, sizeof(L.save_path), g_partner_rom,
                          c == FE_CONSOLE_GBA ? ".sav" :
                          c == FE_CONSOLE_GBC ? ".gbc.sav" : ".gb.sav");
   else
      psp_rom_path_suffix(L.save_path, sizeof(L.save_path), g_game.rom_path,
                          c == FE_CONSOLE_GBA ? ".p2.sav" :
                          c == FE_CONSOLE_GBC ? ".gbc.p2.sav" : ".gb.p2.sav");
   L.save_exists = L.save_path[0] && file_bytes(L.save_path, NULL) == 0;
}

/* The partner's battery image, or NULL.  fresh: as its machine has it at
 * this instant (a Game Boy's is otherwise read a few times a second). */
static const uint8_t *partner_image(uint32_t *writes, size_t *n, int fresh)
{
   if (L.gb)
      return local_gb_partner_save(writes, n, fresh);
   *n = SAVE_BYTES;
   return fe_gbadual_local_partner_save(writes);
}

static int copy_file(const char *from, const char *to)
{
   FILE *a = fopen(from, "rb"), *b;
   char buf[4096];
   size_t n;
   int ok = 1;
   if (!a)
      return -1;
   b = fopen(to, "wb");
   if (!b)
   {
      fclose(a);
      return -1;
   }
   while ((n = fread(buf, 1, sizeof(buf), a)) > 0)
      if (fwrite(buf, 1, n, b) != n)
      {
         ok = 0;
         break;
      }
   fclose(a);
   if (fclose(b) != 0)
      ok = 0;
   if (!ok)
      remove(to);
   return ok ? 0 : -1;
}

/* Write the snapshot.  On whichever thread claimed it (SV_WRITING) -- the
 * I/O worker, as a rule -- so nothing here may log or touch the OSD: the
 * event log has one producer, the emulation thread.  What happened is left
 * in L.wr for save_report(). */
static void save_write(void)
{
   char tmp[APP_PATH_CAP + 8];
   FILE *f;
   int ok = 0, bak = 0;

   /* A copy of the player's save is about to replace a second save file
    * that was there before: keep that one, once. */
   if (L.clone && !L.save_bak_done)
   {
      char bakp[APP_PATH_CAP + 8];
      L.save_bak_done = 1;
      if (file_bytes(L.save_path, NULL) == 0 &&
          snprintf(bakp, sizeof(bakp), "%s.bak", L.save_path) <
             (int)sizeof(bakp))
         bak = copy_file(L.save_path, bakp) == 0 ? 1 : -1;
   }
   snprintf(tmp, sizeof(tmp), "%s.tmp", L.save_path);
   f = fopen(tmp, "wb");
   if (f)
   {
      ok = fwrite(L.save_snap, 1, L.save_n, f) == L.save_n;
      if (fclose(f) != 0)
         ok = 0;
      if (ok)
      {
         /* rename() over an existing file is refused on the Vita's card */
         remove(L.save_path);
         ok = rename(tmp, L.save_path) == 0;
      }
      if (!ok)
         remove(tmp);
   }
   if (!ok)
   {
      /* no room for a second copy, or no rename: in place */
      f = fopen(L.save_path, "wb");
      if (f)
      {
         ok = fwrite(L.save_snap, 1, L.save_n, f) == L.save_n;
         if (fclose(f) != 0)
            ok = 0;
      }
   }
   L.wr.bak = bak;
   L.wr.ok = ok;
   L.wr.size = (unsigned)L.save_n;
   __sync_lock_test_and_set(&L.wr.done, 1);
   __sync_lock_test_and_set(&L.save_state, SV_IDLE);
}

/* On the emulation thread: say what the last write did. */
static void save_report(void)
{
   if (!__sync_bool_compare_and_swap(&L.wr.done, 1, 0))
      return;
   L.save_count++;
   L.save_exists = L.save_exists || L.wr.ok;
   if (L.wr.bak)
      fe_evt("local_save_backup file=%s.bak rc=%d", L.save_path,
             L.wr.bak > 0 ? 0 : -1);
   fe_evt("local_save file=%s size=%u crc=%08x ok=%d", L.save_path, L.wr.size,
          (unsigned)fe_crc32(0, L.save_snap, L.wr.size), L.wr.ok);
   if (!L.wr.ok)
      osd_toast("The second game's save could not be written");
}

/* app_vita.h: on the I/O worker, from main_vita.c's io_step. */
void link_local_io(void)
{
   L.io_calls++;
   if (__sync_bool_compare_and_swap(&L.save_state, SV_PENDING, SV_WRITING))
      save_write();
}

/* Nothing of the save is in flight when this returns. */
static void save_drain(void)
{
   if (__sync_bool_compare_and_swap(&L.save_state, SV_PENDING, SV_WRITING))
      save_write();
   while (L.save_state != SV_IDLE)
      plat_sleep_us(1000);          /* the worker has it: it runs below us */
   save_report();
}

/* Take the snapshot and hand it to whoever writes.  force: written before
 * this returns (stop, the game going away, suspend). */
static void save_flush(int force)
{
   const uint8_t *img;
   uint32_t writes = 0;
   size_t n = 0;

   /* A forced flush looks once more: the game may have written its battery
    * in the frames since the last look. */
   if (force && L.save_path[0] && partner_image(&writes, &n, 1) &&
       writes != L.save_writes)
   {
      L.save_writes = writes;
      L.save_dirty = 1;
   }
   if (!L.save_dirty || !L.save_path[0])
   {
      if (force)
         save_drain();
      return;
   }
   if (L.save_state != SV_IDLE)
   {
      if (!force)
         return;                    /* the last one first; this one next time */
      save_drain();
   }
   save_report();                   /* before the snapshot is overwritten */
   img = partner_image(&writes, &n, force);
   if (!img || !n || n > sizeof(L.save_snap))
      return;
   memcpy(L.save_snap, img, n);
   L.save_n = n;
   L.save_dirty = 0;
   L.save_pending_iter = L.iter;
   __sync_lock_test_and_set(&L.save_state, SV_PENDING);
   if (force)
      save_drain();
   else
      plat_worker_wake();
}

/* Once per iteration while the pair runs. */
static void save_service(void)
{
   uint32_t writes = 0;
   size_t n = 0;

   if (L.io_calls != L.io_calls_seen)
   {
      L.io_calls_seen = L.io_calls;
      L.io_seen_iter = L.iter;
   }
   if (L.save_state == SV_IDLE)
      save_report();
   if (partner_image(&writes, &n, 0))
   {
      if (writes != L.save_writes)
      {
         L.save_writes = writes;
         L.save_quiet = 0;
         L.save_dirty = 1;
      }
      else if (L.save_dirty && ++L.save_quiet >= SAVE_SETTLE_FRAMES)
         save_flush(0);
   }
   /* Handed over and nobody took it: there is no worker (the twin's default,
    * vita.ini io_thread = 0, or a thread that could not be started). */
   if (L.save_state == SV_PENDING &&
       L.iter - L.io_seen_iter > SAVE_WORKER_FRAMES &&
       L.iter - L.save_pending_iter >= 2 &&
       __sync_bool_compare_and_swap(&L.save_state, SV_PENDING, SV_WRITING))
   {
      save_write();
      save_report();
   }
}

/* ---- the partner's picture, sound and buttons --------------------------------- */

static void local_partner_video(const void *data, unsigned w, unsigned h,
                         size_t pitch)
{
   unsigned y;
   if (!data || !w || !h || w > P_W_MAX || h > P_H_MAX ||
       pitch < w * sizeof(uint16_t))
      return;                       /* a skipped frame: keep the last one */
   for (y = 0; y < h; y++)
      memcpy(L.pix + (size_t)y * w, (const uint8_t *)data + (size_t)y * pitch,
             w * sizeof(uint16_t));
   L.pw = w;
   L.ph = h;
   L.have_pix = 1;
}

static void local_partner_audio(const int16_t *lr, size_t frames)
{
   /* Kept only while it is, or is about to be, the sound that goes out. */
   if (L.heard != 1 || !lr || !frames)
      return;
   if (frames > FIFO_FRAMES)
      frames = FIFO_FRAMES;
   if (L.fifo_n + frames > FIFO_FRAMES)
      L.fifo_n = 0;                 /* nobody is taking it (fast forward) */
   memcpy(L.fifo + L.fifo_n * 2, lr, frames * 2 * sizeof(int16_t));
   L.fifo_n += (unsigned)frames;
}

static uint16_t partner_keys(void *user)
{
   (void)user;
   return fe_gbadual_keys_from_retro(L.keys2);
}

/* Which machines synthesise sound from their next frames on. */
static void hear_apply(int both)
{
   int who = (both || L.sw_hear_both) ? FE_GBADUAL_HEAR_BOTH
           : L.heard ? FE_GBADUAL_HEAR_PARTNER : FE_GBADUAL_HEAR_PLAYER;
   if (L.gb)
      local_gb_hear(who);
   else
      fe_gbadual_local_hear(who);
}

static void ramp(int16_t *lr, size_t frames, int up)
{
   size_t i;
   for (i = 0; i < frames; i++)
   {
      int g = (int)(up ? i : frames - 1 - i) * 256 / (int)frames;
      lr[i * 2]     = (int16_t)(lr[i * 2] * g / 256);
      lr[i * 2 + 1] = (int16_t)(lr[i * 2 + 1] * g / 256);
   }
}

/* Queue what is heard, remembering where the wave was left. */
static void heard_push(const int16_t *lr, size_t n)
{
   if (!n)
      return;
   audio_push(lr, n);
   L.last_l = lr[(n - 1) * 2];
   L.last_r = lr[(n - 1) * 2 + 1];
}

/* app_vita.h: the player's machine's sound for the frame that just ended.
 * 1 = taken (this file queued what is to be heard instead). */
int link_local_audio(const int16_t *lr, size_t frames)
{
   static int16_t buf[FIFO_FRAMES * 2];
   size_t n = frames > FIFO_FRAMES ? FIFO_FRAMES : frames, i;

   if (!L.active || !lr || !frames)
      return 0;
   switch (L.au_phase)
   {
   case AU_STEADY:
      if (L.heard == L.view)
      {
         if (L.heard == 0)
         {
            /* the player's own sound, as always (main_vita.c queues it) */
            L.last_l = lr[(frames - 1) * 2];
            L.last_r = lr[(frames - 1) * 2 + 1];
            return 0;
         }
         heard_push(L.fifo, L.fifo_n);
         L.fifo_n = 0;
         return 1;
      }
      /* The view changed: this frame of the old sound fades to nothing,
       * and the new machine's synthesis is switched on. */
      if (L.heard == 0)
         memcpy(buf, lr, n * 2 * sizeof(int16_t));
      else
      {
         n = L.fifo_n;
         memcpy(buf, L.fifo, n * 2 * sizeof(int16_t));
      }
      if (!n)
      {
         /* the partner finished no frame during this one: the fade is made
          * from where its wave was left */
         n = 128;
         for (i = 0; i < n; i++)
         {
            buf[i * 2] = L.last_l;
            buf[i * 2 + 1] = L.last_r;
         }
      }
      ramp(buf, n, 0);
      audio_push(buf, n);
      L.last_l = L.last_r = 0;
      L.fifo_n = 0;
      L.heard = L.view;
      hear_apply(1);
      L.au_phase = AU_GAP;
      L.au_count = 0;
      return 1;
   case AU_GAP:
      /* Two frames of silence (the port keeps its cushion) while the
       * machine about to be heard runs its first frames with the synthesis
       * on; what it produced before that is thrown away. */
      memset(buf, 0, n * 2 * sizeof(int16_t));
      audio_push(buf, n);
      L.fifo_n = 0;
      if (L.heard != L.view)
         L.heard = L.view;          /* switched again meanwhile: both are on */
      else if (++L.au_count >= 2)
         L.au_phase = AU_IN;
      return 1;
   default: /* AU_IN */
      if (L.heard != L.view)
      {
         /* switched again before anything was heard: back to the silence */
         L.heard = L.view;
         L.fifo_n = 0;
         L.au_phase = AU_GAP;
         L.au_count = 0;
         memset(buf, 0, n * 2 * sizeof(int16_t));
         audio_push(buf, n);
         return 1;
      }
      if (L.heard == 0)
         memcpy(buf, lr, n * 2 * sizeof(int16_t));
      else if (L.fifo_n)
      {
         n = L.fifo_n;
         memcpy(buf, L.fifo, n * 2 * sizeof(int16_t));
         L.fifo_n = 0;
      }
      else
      {
         /* nothing of the partner's yet: silence for one more frame */
         memset(buf, 0, n * 2 * sizeof(int16_t));
         audio_push(buf, n);
         return 1;
      }
      ramp(buf, n < 256 ? n : 256, 1);
      heard_push(buf, n);
      L.au_phase = AU_STEADY;
      hear_apply(0);                /* the other one may stop synthesising */
      return 1;
   }
}

/* ---- views -------------------------------------------------------------------- */

static void view_set(int v, const char *via)
{
   v = v ? 1 : 0;
   if (!L.active || v == L.view)
      return;
   L.view = v;
   /* What is held now was pressed for the other game. */
   L.held_block = ~0u;
   L.hint_frames = 180;
   fe_evt("local_view view=%d via=%s pad2=%d", L.view, via, L.have2);
}

void link_local_next_view(void)
{
   view_set(!L.view, "next");
}

int link_local_tap(int x, int y)
{
   vid_rect_t r[2];
   unsigned w = L.gb ? 160 : 240, h = L.gb ? 144 : 160;
   int inset = g_pcfg.local_layout == VID_PAIR_INSET, i;

   if (!L.active)
      return 0;
   vid_pair_rects(w, h, inset ? VID_PAIR_INSET : VID_PAIR_SIDE, &r[0], &r[1]);
   for (i = 0; i < 2; i++)
      if (x >= r[i].x && x < r[i].x + r[i].w && y >= r[i].y &&
          y < r[i].y + r[i].h)
      {
         /* side by side: left is the player's.  Large + small: the large
          * one is the view, so the small one is always "the other". */
         int want = inset ? (i ? !L.view : L.view) : i;
         if (want == L.view)
            return 2;            /* the one already in hand: the menu's tap */
         view_set(want, "tap");
         return 1;
      }
   return 0;
}

void link_local_pads(uint32_t keys1, uint32_t keys2, int have2)
{
   if (!L.active)
      return;
   /* A second controller came or went: whatever is held was pressed under
    * the other arrangement. */
   if ((have2 ? 1 : 0) != L.have2)
      L.held_block = ~0u;
   L.have2 = have2 ? 1 : 0;
   if (L.have2)
   {
      /* Each player his own machine: nothing to decide. */
      L.held_block &= keys1;
      g_app_keys = keys1 & ~L.held_block;
      L.keys2 = keys2;
      return;
   }
   L.held_block &= keys1;
   keys1 &= ~L.held_block;
   if (L.view)
   {
      L.keys2 = keys1;
      g_app_keys = 0;
   }
   else
   {
      L.keys2 = 0;
      g_app_keys = keys1;
   }
}

/* ---- every other frame, when two machines do not fit a refresh ------------------ */

/* At the end of the pair's drawing: what this frame's work has come to. */
static void skip_measure(void)
{
   uint32_t period = 1000000000u / (plat_refresh_mhz() ? plat_refresh_mhz()
                                                       : 60000u);
   uint32_t work = (uint32_t)(plat_time_us() - g_app_frame_t0);
   if (work > 4 * period)
      return;                       /* a hitch (a save, a suspend): no sample */
   L.skip_ema = L.skip_n ? (L.skip_ema * 15 + work) / 16 : work;
   if (L.skip_n < 100000)
      L.skip_n++;
   if (L.sw_skip != 1)
      return;
   if (!L.skip_on)
   {
      if (L.skip_n >= 90 && L.skip_ema > period * 95 / 100)
      {
         L.skip_on = 1;
         L.skip_calm = 0;
         fe_evt("local_skip on=1 work_us=%u period_us=%u",
                (unsigned)L.skip_ema, (unsigned)period);
      }
   }
   else if (L.skip_ema < period * 62 / 100)
   {
      if (++L.skip_calm >= 600)
      {
         L.skip_on = 0;
         L.skip_n = 0;
         fe_evt("local_skip on=0 work_us=%u", (unsigned)L.skip_ema);
      }
   }
   else
      L.skip_calm = 0;
}

/* Once per iteration, before the frame runs: is this one drawn?  (Fast
 * forward's own gate, when it is on, is set after this and wins.) */
static void skip_apply(void)
{
   int want = L.sw_skip == 2 || (L.sw_skip == 1 && L.skip_on);
   if (want)
   {
      fe_host_skip_override((int)(L.iter & 1));
      L.skip_set = 1;
   }
   else if (L.skip_set)
   {
      fe_host_skip_override(-1);
      L.skip_set = 0;
   }
}

/* ---- the picture ---------------------------------------------------------------- */

#define COL_P1    0x65BF            /* the OSD's soft blue  */
#define COL_P2    0xFEA0            /* ...and its warm yellow */
#define COL_WHITE 0xFFFF
#define COL_DIM   0x8410
#define COL_BLACK 0x0000

/* A player's tag: "P1" on his colour, and what the picture is. */
static void tag_draw(int x, int y, int player, int lit, const char *text)
{
   uint16_t col = player ? COL_P2 : COL_P1;
   const char *p = player ? "P2" : "P1";
   int tw = vid_text_w(p) + 8;
   if (lit)
   {
      vid_rect(x, y, tw, 16, col, 255);
      vid_text(x + 4, y, p, COL_BLACK);
   }
   else
   {
      vid_rect(x, y, tw, 16, col, 70);
      vid_text(x + 4, y, p, col);
   }
   vid_text(x + tw + 6, y, text, lit ? COL_WHITE : COL_DIM);
}

void link_local_present(const uint16_t *pix, unsigned w, unsigned h,
                        size_t pitch_bytes)
{
   static uint16_t mine[P_W_MAX * P_H_MAX];
   const uint16_t *a = pix, *b = L.have_pix ? L.pix : NULL;
   int inset = g_pcfg.local_layout == VID_PAIR_INSET;
   vid_rect_t r[2];
   int who[2], i;
   unsigned y;

   if (!pix || !w || !h || w > P_W_MAX || h > P_H_MAX)
      return;
   /* One pitch for both textures: the player's frame at the partner's
    * (tight) pitch, when the two differ. */
   if (pitch_bytes != w * sizeof(uint16_t))
   {
      for (y = 0; y < h; y++)
         memcpy(mine + (size_t)y * w,
                (const uint8_t *)pix + (size_t)y * pitch_bytes,
                w * sizeof(uint16_t));
      a = mine;
   }
   if (b && (L.pw != w || L.ph != h))
      b = NULL;
   /* Side by side: the player's game is always the left one.  Large +
    * small: the game in view is the large one. */
   who[0] = inset ? L.view : 0;
   who[1] = !who[0];
   vid_draw_frame_pair(who[0] ? b : a, who[1] ? b : a, w, h,
                       w * sizeof(uint16_t),
                       inset ? VID_PAIR_INSET : VID_PAIR_SIDE);
   vid_pair_rects(w, h, inset ? VID_PAIR_INSET : VID_PAIR_SIDE, &r[0], &r[1]);

   /* Whose picture is which, and which one the buttons and the speaker
    * belong to.  Logical (480x272) coordinates from here on; nothing is
    * drawn over a pixel of either game: a bar of the player's colour above
    * and below each picture (bright on the one in view) and his tag under
    * it -- or, where the picture reaches the bottom of the screen (the
    * large one of a GBA pair), beside it, in the small picture's column. */
   vid_overlay_begin(0);
   for (i = 0; i < 2; i++)
   {
      int x = r[i].x / 2, yy = r[i].y / 2, ww = r[i].w / 2, hh = r[i].h / 2;
      int ox = r[!i].x / 2, ow = r[!i].w / 2;
      int p = who[i], lit = p == L.view;
      uint16_t col = p ? COL_P2 : COL_P1;
      int below = yy + hh + 26 <= VID_SCR_H;
      int narrow = below ? ww < 200 : ow < 200;
      int tx = below ? x + 4 : ox + 4, ty = below ? yy + hh + 6 : yy + hh - 22;
      const char *text;

      if (!below)
         text = lit ? (L.have2 ? (ox > x ? "< sound" : "sound >")
                               : (ox > x ? "< playing" : "playing >"))
                    : "";
      else if (L.have2)
         text = p ? (narrow ? "pad 2" : lit ? "controller 2  (sound)"
                                            : "controller 2")
                  : (narrow ? "pad 1" : lit ? "this console  (sound)"
                                            : "this console");
      else
         text = p ? (narrow ? "partner" : lit ? "partner's game  (playing)"
                                              : "partner's game")
                  : (narrow ? "you" : lit ? "your game  (playing)"
                                          : "your game");
      if (yy >= 2)
         vid_rect(x, yy - 2, ww, 2, col, lit ? 255 : 70);
      if (yy + hh + 2 <= VID_SCR_H)
         vid_rect(x, yy + hh, ww, 2, col, lit ? 255 : 70);
      tag_draw(tx, ty, p, lit, text);
   }
   /* How to change games: for ten seconds, and again after a change. */
   if (!L.have2 && L.hint_frames)
   {
      int ly = r[0].y / 2 + r[0].h / 2, ry = r[1].y / 2 + r[1].h / 2;
      L.hint_frames--;
      if (inset)
      {
         /* under the small picture and its tag */
         vid_text(r[1].x / 2 + 4, ry + 34, "Right stick or", COL_DIM);
         vid_text(r[1].x / 2 + 4, ry + 50, "a tap: switch", COL_DIM);
      }
      else if (ly + 54 <= VID_SCR_H)
         vid_text_center(ly + 34, "Right stick or a tap: the other game",
                         COL_DIM);
   }
   vid_overlay_end();
   skip_measure();
}

/* ---- start and stop ---------------------------------------------------------------- */

static void switches_read(void)
{
   char ini[APP_PATH_CAP], v[64];
   if (snprintf(ini, sizeof(ini), "%s/vita.ini", g_app_root) >= (int)sizeof(ini))
      return;
   L.sw_auto = (int)fe_ini_get_int(ini, "local_auto", 0);
   L.sw_auto_frame = (unsigned)fe_ini_get_int(ini, "local_auto_frame", 2);
   L.sw_lookahead = (unsigned)fe_ini_get_int(ini, "local_lookahead", 0);
   L.sw_hash = (int)fe_ini_get_int(ini, "local_hash", 0);
   L.sw_hear_both = (int)fe_ini_get_int(ini, "local_hear_both", 0);
   L.sw_gb_batch = (unsigned)fe_ini_get_int(ini, "local_gb_batch", 0);
   L.sw_dump = (int)fe_ini_get_int(ini, "local_dump", 0);
   L.sw_cpu = (unsigned)fe_ini_get_int(ini, "local_cpu", 0);
   L.sw_skip = (int)fe_ini_get_int(ini, "local_skip", 1);
   L.sw_peek_addr = L.sw_peek_n = 0;
   if (fe_ini_get(ini, "local_peek", v, sizeof(v)) == 1)
   {
      char *colon = strchr(v, ':');
      L.sw_peek_addr = (unsigned)strtoul(v, NULL, 0);
      L.sw_peek_n = colon ? (unsigned)strtoul(colon + 1, NULL, 0) : 1;
      if (L.sw_peek_n > 64)
         L.sw_peek_n = 64;
   }
}

/* A test ROM's verdict: words of both machines' RAM, in gl_host's format,
 * sixteen to a line (a log line holds 512 characters). */
static void peek_evt(void)
{
   char line[16 * 9 + 8];
   unsigned s, i, k;
   if (!L.sw_peek_n)
      return;
   if (L.gb)
   {
      /* a Game Boy: bytes, sixteen to a line */
      for (s = 0; s < 2; s++)
         for (i = 0; i < L.sw_peek_n; i += 16)
         {
            size_t at = 0;
            for (k = i; k < i + 16 && k < L.sw_peek_n; k++)
            {
               uint8_t b = 0;
               int ok = fe_host_gb_local_peek((int)s,
                           (uint16_t)(L.sw_peek_addr + k), &b, 1);
               at += (size_t)snprintf(line + at, sizeof(line) - at,
                                      ok == 0 ? " %02x" : " ??", b);
            }
            fe_evt("local_peek s=%u %04x:%s", s, L.sw_peek_addr + i, line);
         }
      return;
   }
   for (s = 0; s < 2; s++)
      for (i = 0; i < L.sw_peek_n; i += 16)
      {
         size_t at = 0;
         for (k = i; k < i + 16 && k < L.sw_peek_n; k++)
         {
            uint32_t w = 0;
            int ok = fe_gbadual_local_peek((int)s, L.sw_peek_addr + k * 4,
                                           &w, 4);
            at += (size_t)snprintf(line + at, sizeof(line) - at,
                                   ok == 0 ? " %08x" : " ????????",
                                   (unsigned)w);
         }
         fe_evt("local_peek s=%u %08x:%s", s, L.sw_peek_addr + i * 4, line);
      }
}

static void stats_evt(const char *why)
{
   fe_gbadual_local_info st;
   if (L.gb || fe_gbadual_local_stats(&st) != 0)
      return;
   fe_evt("local_stats why=%s frames=%u/%u switches=%u runs=%u/%u "
          "lookahead=%u mp=%u/%u n=%u/%u late=%u/%u late_max=%u/%u "
          "bus_ovf=%u bus_depth=%u err=%d saves=%u", why, st.frames[0],
          st.frames[1], st.switches, st.runs[0], st.runs[1], st.lookahead,
          st.mp_xfers[0], st.mp_xfers[1], st.n_xfers[0], st.n_xfers[1],
          st.ev_late[0], st.ev_late[1], st.late_max[0], st.late_max[1],
          st.bus_overflow, st.bus_depth_max, st.error, L.save_count);
}

/* The start itself.  0 = the pair runs. */
static int start_now(int clone)
{
   uint8_t *save = NULL;
   size_t save_n = 0;
   int rc;

   L.error = NULL;
   if (L.active)
      return 0;
   if (link_net_busy())
   {
      /* main_vita.c decides this for the menu; whatever else asks (the
       * test switch) gets the same answer */
      L.error = "End the network game first.";
      fe_evt("local_start REFUSED reason=network");
      return -1;
   }
   if (g_partner_rom[0])
      clone = 0;                    /* another game has its own save */
   save_path_set();
   if (!clone && L.save_exists)
   {
      long n = 0;
      FILE *f;
      if (file_bytes(L.save_path, &n) == 0 && n > 0 &&
          (save = (uint8_t *)malloc(SAVE_BYTES + 64)) != NULL &&
          (f = fopen(L.save_path, "rb")) != NULL)
      {
         save_n = fread(save, 1, SAVE_BYTES + 64, f);
         fclose(f);
      }
   }
   L.gb = g_game.console != FE_CONSOLE_GBA;
   if (L.sw_dump)
   {
      /* the machine the pair starts from, for a reference run elsewhere */
      char path[APP_PATH_CAP];
      if (snprintf(path, sizeof(path), "%s/log/local_start.st", g_app_root) <
          (int)sizeof(path))
         (void)fe_host_state_save(path);
   }
   L.have_pix = 0;
   L.fifo_n = 0;
   L.heard = 0;
   L.view = 0;
   L.au_phase = AU_STEADY;
   L.keys2 = 0;
   L.held_block = ~0u;              /* the button that chose "Start" */
   L.clone = clone;
   if (clone)
   {
      /* A copy shows what the player's game shows until its own first
       * frame arrives: no black picture for a frame. */
      unsigned w = 0, h = 0;
      size_t pitch = 0;
      const uint16_t *pix = app_last_frame(&w, &h, &pitch);
      if (pix)
         local_partner_video(pix, w, h, pitch);
   }
   if (L.gb)
      rc = local_gb_start(clone, g_partner_rom[0] ? g_partner_rom : NULL,
                          save, save_n);
   else
   {
      fe_gbadual_local lc;
      memset(&lc, 0, sizeof(lc));
      lc.clone = clone;
      lc.save = save;
      lc.save_n = save_n;
      lc.rom_path = g_partner_rom[0] ? g_partner_rom : NULL;
      lc.lookahead = L.sw_lookahead;
      lc.video = local_partner_video;
      lc.audio = local_partner_audio;
      lc.keys = partner_keys;
      rc = fe_gbadual_local_start(&lc);
   }
   free(save);
   if (rc != 0)
   {
      const char *a, *b;
      ui_link_fail_text(UI_FAIL_LOCAL, &a, &b);
      L.error = rc == FE_GBADUAL_LOCAL_E_CART
              ? "The partner's game could not be loaded."
              : rc == LOCAL_E_UNSUPPORTED
              ? "This game cannot be linked on one console."
              : a;
      L.gb = 0;
      fe_evt("local_start FAILED rc=%d clone=%d partner=%s", rc, clone,
             g_partner_rom[0] ? g_partner_rom : "-");
      return -1;
   }
   L.active = 1;
   L.have2 = 0;
   L.have2_told = 0;
   L.hint_frames = 600;
   L.save_bak_done = 0;
   L.save_dirty = 0;
   L.save_quiet = 0;
   L.save_count = 0;
   L.io_seen_iter = L.iter;
   {
      uint32_t writes = 0;
      size_t n = 0;
      if (partner_image(&writes, &n, 1))
         L.save_writes = writes;
   }
   hear_apply(0);
   if (L.sw_hash)
   {
      char path[APP_PATH_CAP];
      if (snprintf(path, sizeof(path), "%s/log/localhash.txt", g_app_root) <
          (int)sizeof(path))
      {
         if (L.gb)
            L.gb_hash = fopen(path, "w");
         else
            fe_gbadual_local_hash_file(path);
      }
   }
   /* The player's machine is the one heard, as before: its sound carries
    * on without a seam.  The pacer must not count the start (a second
    * cartridge read from the card) as a late frame. */
   app_pace_reset();
   fe_evt("local_start ok console=%d clone=%d partner=%s save=%s size=%u "
          "layout=%d pads=%d", (int)g_game.console, clone,
          g_partner_rom[0] ? g_partner_rom : "-", L.save_path,
          (unsigned)save_n, g_pcfg.local_layout, plat_pad_ports());
   return 0;
}

void link_local_start(void)
{
   /* Done at the top of the next iteration: the menu has drawn "Starting
    * the second game" by then, and another cartridge takes a moment to
    * read. */
   L.error = NULL;
   if (!L.active)
      L.start_req = g_pcfg.local_partner ? 2 : 1;
}

static void stop_now(const char *why)
{
   if (!L.active)
      return;
   save_flush(1);
   stats_evt(why);
   peek_evt();
   /* What is queued of the partner's sound is not followed by any more of
    * it (the port fades an emptied queue out); the player's own, when that
    * is what was heard, simply continues. */
   if (L.heard != 0 || L.au_phase != AU_STEADY)
      app_audio_flush();
   if (L.gb_hash)
      fclose(L.gb_hash);
   L.gb_hash = NULL;
   if (L.gb)
      local_gb_stop();
   else
      fe_gbadual_local_stop();
   L.active = 0;
   L.gb = 0;
   L.have_pix = 0;
   L.fifo_n = 0;
   L.keys2 = 0;
   L.view = 0;
   if (L.skip_set)
      fe_host_skip_override(-1);
   L.skip_set = L.skip_on = 0;
   L.skip_n = 0;
   L.heard = 0;
   L.au_phase = AU_STEADY;
   save_path_set();
   app_pace_reset();
   fe_evt("local_stop why=%s", why);
}

void link_local_stop(void)
{
   L.start_req = 0;
   L.error = NULL;
   stop_now("menu");
}

int link_local_active(void)
{
   return L.active;
}

/* ---- the session's life (app_vita.h part B) ------------------------------------ */

void link_local_game_begin(void)
{
   memset(&L, 0, sizeof(L));
   switches_read();
   partner_choice_load();
   save_path_set();
}

void link_local_game_end(void)
{
   L.start_req = 0;
   stop_now("game_end");
   save_drain();
}

void link_local_frame(void)
{
   L.iter++;
   if (L.sw_cpu && fe_host_frame_count() % L.sw_cpu == 0)
   {
      uint64_t ns = 0, n = 0;
      fe_host_cpu_stat(&ns, &n);
      fe_evt("local_cpu frames=%u emu_us=%u", (unsigned)n,
             (unsigned)(ns / 1000));     /* (wraps after 71 minutes) */
   }
   if (L.sw_auto && !L.active && !L.start_req &&
       fe_host_frame_count() >= L.sw_auto_frame)
   {
      L.start_req = L.sw_auto;
      L.sw_auto = 0;
   }
   if (L.start_req)
   {
      int clone = L.start_req == 1;
      L.start_req = 0;
      start_now(clone);
   }
   if (!L.active)
      return;
   /* The pair stopped by itself: a fault of the stepper, or a machine that
    * derailed.  The player's game carries on alone. */
   if (L.gb ? local_gb_error() : fe_gbadual_local_error())
   {
      fe_evt("local_pair_error err=%d",
             L.gb ? local_gb_error() : fe_gbadual_local_error());
      stop_now("error");
      osd_toast("The link between the two games stopped");
      return;
   }
   save_service();
   skip_apply();
   if (L.gb_hash)
      fprintf(L.gb_hash, "f=%u h=%016llx\n", fe_host_frame_count(),
              (unsigned long long)fe_host_gb_local_hash());
   /* A second controller that comes or goes is said once. */
   {
      int p2 = plat_pad_ports() >= 2;
      if (p2 != L.have2_told)
      {
         L.have2_told = p2;
         osd_toast(p2 ? "Controller 2 drives the partner's game"
                      : "One controller: right stick or tap swaps games");
         fe_evt("local_pad2 on=%d", p2);
      }
   }
}

void link_local_power(int resumed)
{
   if (!L.active)
      return;
   if (!resumed)
      save_flush(1);                /* the console may be left for days */
   else
   {
      L.fifo_n = 0;
      L.au_phase = AU_STEADY;
      L.heard = L.view;
      hear_apply(0);
      L.held_block = ~0u;
   }
}

void link_local_ui_view(ui_link_view *v)
{
   if (L.error)
      v->error = L.error;
}

/* ---- ui_host_hooks.h section 5 ---------------------------------------------------- */

void ui_host_local_state(ui_local_state *out)
{
   if (!out)
      return;
   memset(out, 0, sizeof(*out));
   out->active = L.active;
   out->controlled = L.view;
   out->have_pad2 = plat_pad_ports() >= 2;
   out->partner_save = L.save_exists;
   out->partner_game = g_partner_rom[0] ? g_partner_name : NULL;
   out->saves_written = (int)L.save_count;
   if (g_game.console == FE_CONSOLE_GBA)
   {
      if (!fe_gbadual_loaded())
         out->why_not = "The second game could not be set up for this game.";
   }
   else if (!local_gb_available())
      out->why_not = "This game cannot be linked on one console.";
   if (!out->why_not && !L.active && link_net_busy())
      out->why_not = "End the network game first.";
   out->available = out->why_not == NULL;
}

/* ---- pair save states: <state>.p2 ---------------------------------------------------
 *
 * The player's own state file is main_vita.c's, written before this is
 * called, and stays a plain state of his game.  Beside it goes the pair:
 * a header that ties it to that exact file (its CRC-32: a state saved over
 * the slot later, with or without a partner, orphans the old companion
 * instead of being paired with it) and to the partner's cartridge, then
 * fe_gbadual's image of both machines and the cable. */
#define PAIR_MAGIC 0x32504247u      /* "GBP2" */

typedef struct pair_hdr
{
   uint32_t magic, version;
   uint32_t state_crc, state_size;  /* of the player's state file */
   uint32_t body_size, body_crc;
   uint32_t clone;                  /* how the partner had started */
   char     partner[96];            /* its cartridge's file name, "" = same */
} pair_hdr;

static int pair_path(char *out, size_t cap, const char *state_file)
{
   return snprintf(out, cap, "%s.p2", state_file) < (int)cap ? 0 : -1;
}

static int file_crc(const char *path, uint32_t *crc, uint32_t *size)
{
   FILE *f = fopen(path, "rb");
   uint8_t buf[4096];
   size_t n;
   uint32_t c = 0, total = 0;
   if (!f)
      return -1;
   while ((n = fread(buf, 1, sizeof(buf), f)) > 0)
   {
      c = fe_crc32(c, buf, n);
      total += (uint32_t)n;
   }
   fclose(f);
   *crc = c;
   *size = total;
   return 0;
}

void link_local_state_saved(const char *state_file)
{
   char path[APP_PATH_CAP + 8];
   pair_hdr h;
   uint8_t *body = NULL;
   size_t cap;
   long n = -1;
   FILE *f;
   int ok = 0;

   if (!state_file || pair_path(path, sizeof(path), state_file) != 0)
      return;
   /* Whatever was there described another state of this slot. */
   remove(path);
   if (!L.active)
      return;
   memset(&h, 0, sizeof(h));
   h.magic = PAIR_MAGIC;
   h.version = 1;
   h.clone = (uint32_t)L.clone;
   snprintf(h.partner, sizeof(h.partner), "%s",
            g_partner_rom[0] ? base_name(g_partner_rom) : "");
   if (L.gb)
   {
      cap = local_gb_state_max();
      if (cap && (body = (uint8_t *)malloc(cap)) != NULL)
         n = local_gb_state_save(body, cap);
   }
   else
   {
      cap = fe_gbadual_local_state_max();
      if (cap && (body = (uint8_t *)malloc(cap)) != NULL)
         n = fe_gbadual_local_state_save(body, cap);
   }
   if (n > 0 && file_crc(state_file, &h.state_crc, &h.state_size) == 0 &&
       (f = fopen(path, "wb")) != NULL)
   {
      h.body_size = (uint32_t)n;
      h.body_crc = fe_crc32(0, body, (size_t)n);
      ok = fwrite(&h, 1, sizeof(h), f) == sizeof(h) &&
           fwrite(body, 1, (size_t)n, f) == (size_t)n;
      if (fclose(f) != 0)
         ok = 0;
      if (!ok)
         remove(path);
   }
   free(body);
   fe_evt("local_state_save file=%s size=%ld ok=%d", path, n, ok);
   if (!ok)
      osd_toast("Saved your game only: the second game was not saved");
}

void link_local_state_loaded(const char *state_file)
{
   char path[APP_PATH_CAP + 8];
   pair_hdr h;
   uint8_t *body = NULL;
   uint32_t crc = 0, size = 0;
   const char *why = "none";
   FILE *f = NULL;
   int rc = -1;

   if (!L.active)
      return;
   L.fifo_n = 0;
   L.au_phase = AU_STEADY;
   L.heard = L.view;
   hear_apply(0);
   if (state_file && pair_path(path, sizeof(path), state_file) == 0 &&
       (f = fopen(path, "rb")) != NULL)
   {
      why = "format";
      if (fread(&h, 1, sizeof(h), f) == sizeof(h) && h.magic == PAIR_MAGIC &&
          h.version == 1 && h.body_size && h.body_size < (8u << 20))
      {
         h.partner[sizeof(h.partner) - 1] = '\0';
         if (file_crc(state_file, &crc, &size) != 0 || crc != h.state_crc ||
             size != h.state_size)
            why = "other_state";    /* the slot was saved over since */
         else if (strcmp(h.partner, g_partner_rom[0]
                                    ? base_name(g_partner_rom) : "") != 0)
            why = "other_partner";
         else if ((body = (uint8_t *)malloc(h.body_size)) == NULL ||
                  fread(body, 1, h.body_size, f) != h.body_size ||
                  fe_crc32(0, body, h.body_size) != h.body_crc)
            why = "damaged";
         else
         {
            rc = L.gb ? local_gb_state_load(body, h.body_size)
                      : fe_gbadual_local_state_load(body, h.body_size);
            why = rc == 0 ? "pair" : "refused";
            if (rc == 0)
               /* the player's machine was loaded once more, from the pair's
                * own copy of it: the codes again (cheats_vita.h) */
               cheats_apply();
         }
      }
      fclose(f);
   }
   free(body);
   if (rc != 0)
   {
      /* A state of the player's game alone.  A partner that is a copy
       * becomes a copy of what was loaded; one with a save of its own
       * carries on, the cable plugged in again. */
      int reclone = L.clone && !g_partner_rom[0];
      int r2 = L.gb ? local_gb_restart(reclone)
                    : fe_gbadual_local_restart(reclone);
      if (!strcmp(why, "other_partner"))
         osd_toast("That state's second game is another cartridge");
      else
         osd_toast(reclone ? "State loaded: the second game is a copy again"
                           : "State loaded: your game only");
      fe_evt("local_state_load file=%s pair=0 why=%s reclone=%d rc=%d",
             state_file ? state_file : "-", why, reclone, r2);
      return;
   }
   fe_evt("local_state_load file=%s pair=1", state_file);
}

/* ---- the partner's game -----------------------------------------------------------
 *
 * Another cartridge of the same console in the partner's machine: the other
 * version of a pair of games.  The choice is the player's, per game, kept in
 * <root>/local.ini as  p_<crc32 of this game's name under roms/> = <the
 * partner's name under roms/>  (a key made of a ROM's name would not survive
 * an ini file).  The menu's picker lists the candidates through the three
 * ui_host_local_rom* hooks. */
#define PICK_MAX  2048           /* the game list's size (ui_psp.c BROWSER_MAX) */
#define PICK_POOL (160 * 1024)   /* their names, packed: ~60 bytes each */

/* A candidate is named as the game list names a game: relative to roms/, or
 * for a game of an extra folder (ui_host_extra_roots) by its whole path. */
static char **g_pick;
static char *g_pick_pool;
static unsigned g_pick_used;
static int g_pick_n;

static int same_console(const char *name)
{
   size_t n = strlen(name);
   fe_console_t c = g_game.console;
   if (c == FE_CONSOLE_GBA)
      return n > 4 && !strcasecmp(name + n - 4, ".gba");
   if (c == FE_CONSOLE_GBC)
      return n > 4 && !strcasecmp(name + n - 4, ".gbc");
   return n > 3 && !strcasecmp(name + n - 3, ".gb");
}

static int ini_path(char *out, size_t cap)
{
   return snprintf(out, cap, "%s/local.ini", g_app_root) < (int)cap ? 0 : -1;
}

static void ini_key(char *out, size_t cap)
{
   snprintf(out, cap, "p_%08x",
            (unsigned)fe_crc32(0, g_game.key, strlen(g_game.key)));
}

/* A name for the menus: the file's, without folder and extension. */
static void display_name(char *out, size_t cap, const char *rel)
{
   char *dot;
   snprintf(out, cap, "%s", base_name(rel));
   dot = strrchr(out, '.');
   if (dot && dot != out)
      *dot = '\0';
}

/* The game being played is not a candidate for its own partner. */
static int is_this_game(const char *rel)
{
   char path[APP_PATH_CAP];
   return psp_rom_path_join(path, sizeof(path), g_app_rom_dir, rel) == 0 &&
          !strcasecmp(path, g_game.rom_path);
}

static int partner_set(const char *rel)
{
   char path[APP_PATH_CAP];
   long size = 0;
   g_partner_rom[0] = '\0';
   g_partner_name[0] = '\0';
   if (!rel || !rel[0] || is_this_game(rel) || !same_console(rel) ||
       psp_rom_path_join(path, sizeof(path), g_app_rom_dir, rel) != 0 ||
       file_bytes(path, &size) != 0 ||
       (g_game.console == FE_CONSOLE_GBA && size < 0xC0))
      return -1;
   snprintf(g_partner_rom, sizeof(g_partner_rom), "%s", path);
   display_name(g_partner_name, sizeof(g_partner_name), rel);
   return 0;
}

static void partner_choice_load(void)
{
   char ini[APP_PATH_CAP], key[24], rel[PSP_ROM_REL_PATH_CAP];
   g_partner_rom[0] = '\0';
   g_partner_name[0] = '\0';
   ini_key(key, sizeof(key));
   if (ini_path(ini, sizeof(ini)) == 0 &&
       fe_ini_get(ini, key, rel, sizeof(rel)) == 1 && partner_set(rel) != 0)
      fe_evt("local_partner_gone game=%s partner=%s", g_game.key, rel);
}

static int pick_cmp(const void *a, const void *b)
{
   return strcasecmp(base_name(*(char *const *)a), base_name(*(char *const *)b));
}

static void pick_add(const char *name)
{
   size_t n = strlen(name) + 1;
   if (g_pick_n >= PICK_MAX || g_pick_used + n > PICK_POOL)
      return;
   memcpy(g_pick_pool + g_pick_used, name, n);
   g_pick[g_pick_n++] = g_pick_pool + g_pick_used;
   g_pick_used += (unsigned)n;
}

/* `root` is the folder, `sub` a path under it ("" at the top).  An extra
 * folder's games are added by their whole path (`whole`), roms/'s by their
 * name under it. */
static void pick_scan(const char *root, int whole, const char *sub, int depth)
{
   char dir[APP_PATH_CAP];
   pc_uid d;
   pc_dirent e;

   if (sub[0] ? snprintf(dir, sizeof(dir), "%s/%s", root, sub) >=
                   (int)sizeof(dir)
              : snprintf(dir, sizeof(dir), "%s", root) >= (int)sizeof(dir))
      return;
   d = pc_io_dopen(dir);
   if (d < 0)
      return;
   memset(&e, 0, sizeof(e));
   while (g_pick_n < PICK_MAX && pc_io_dread(d, &e) > 0)
   {
      char rel[PSP_ROM_REL_PATH_CAP], name[PSP_ROM_REL_PATH_CAP];
      if (e.d_name[0] == '.')
         continue;
      if (snprintf(rel, sizeof(rel), "%s%s%s", sub, sub[0] ? "/" : "",
                   e.d_name) >= (int)sizeof(rel))
         continue;
      if ((e.d_stat.st_mode & 0x1000) || (e.d_stat.st_attr & 0x10))
      {
         if (depth < 2)
            pick_scan(root, whole, rel, depth + 1);
      }
      else if (same_console(e.d_name))
      {
         if (whole ? snprintf(name, sizeof(name), "%s/%s", root, rel) >=
                        (int)sizeof(name)
                   : snprintf(name, sizeof(name), "%s", rel) >=
                        (int)sizeof(name))
            continue;
         if (!is_this_game(name))
            pick_add(name);
      }
   }
   pc_io_dclose(d);
}

/* ui_host_hooks.h section 5: the picker opens -- list the cartridges of
 * this console under roms/ (two folders deep), this game left out. */
int ui_host_local_roms(void)
{
   int i;
   g_pick_n = 0;
   g_pick_used = 0;
   if (!g_pick)
      g_pick = malloc(PICK_MAX * sizeof(*g_pick));
   if (!g_pick_pool)
      g_pick_pool = malloc(PICK_POOL);
   if (!g_pick || !g_pick_pool)
      return 0;
   pick_scan(g_app_rom_dir, 0, "", 0);
   for (i = 0; i < ui_host_extra_roots(); i++)
      pick_scan(ui_host_extra_root(i), 1, "", 0);
   qsort(g_pick, (size_t)g_pick_n, sizeof(*g_pick), pick_cmp);
   fe_evt("local_roms n=%d console=%d", g_pick_n, (int)g_game.console);
   return g_pick_n;
}

const char *ui_host_local_rom_name(int i)
{
   static char name[64];
   if (i < 0 || i >= g_pick_n)
      return "";
   display_name(name, sizeof(name), g_pick[i]);
   return name;
}

/* i < 0: the partner plays this game again.  0 = chosen and remembered. */
int ui_host_local_rom_pick(int i)
{
   char ini[APP_PATH_CAP], key[24];
   const char *rel = i >= 0 && i < g_pick_n ? g_pick[i] : "";
   int rc = 0;

   if (L.active)
      return -1;                    /* stop the pair first */
   if (rel[0])
      rc = partner_set(rel);
   else
      partner_set(NULL);
   ini_key(key, sizeof(key));
   if (rc == 0 && ini_path(ini, sizeof(ini)) == 0)
      fe_ini_set(ini, key, rel);
   save_path_set();
   fe_evt("local_partner game=%s partner=%s rc=%d", g_game.key,
          rel[0] ? rel : "-", rc);
   return rc;
}

/* ---- GB / GBC -----------------------------------------------------------------------
 * fe_host's local pair (fe_host.h): the running Game Boy becomes slot 0 of
 * a gbcore_dual pair and the partner's runs beside it.  Both synthesise
 * their sound (TGB Dual has no silent-but-drawn mode, and a Game Boy's four
 * channels cost next to nothing); which is queued is decided above. */

static uint32_t gb_partner_pad(void)
{
   return L.keys2;
}

static int local_gb_available(void)
{
   return g_game.console == FE_CONSOLE_GB || g_game.console == FE_CONSOLE_GBC;
}

static int local_gb_start(int clone, const char *partner_rom,
                          const uint8_t *save, size_t save_n)
{
   fe_host_gb_local lc;
   int rc;
   memset(&lc, 0, sizeof(lc));
   lc.clone = clone;
   lc.rom_path = partner_rom;
   lc.save = save;
   lc.save_n = save_n;
   lc.batch_lines = L.sw_gb_batch;
   lc.video = local_partner_video;
   lc.audio = local_partner_audio;
   lc.pad = gb_partner_pad;
   rc = fe_host_gb_local_start(&lc);
   /* The machine was shut down and made again around the start, whatever
    * came of it: the frame main_vita.c remembers is gone. */
   app_frame_forget();
   return rc == 0 ? 0 : rc == -2 ? FE_GBADUAL_LOCAL_E_CART
        : rc == -3 ? FE_GBADUAL_LOCAL_E_CLONE : FE_GBADUAL_LOCAL_E_BUSY;
}

static void local_gb_stop(void)
{
   fe_host_gb_local_stop();
   app_frame_forget();
}

static int local_gb_error(void)
{
   return fe_host_gb_local_active() ? 0 : -1;
}

static void local_gb_hear(int who)
{
   (void)who;
}

static const uint8_t *local_gb_partner_save(uint32_t *writes, size_t *n,
                                            int fresh)
{
   return fe_host_gb_local_partner_save(writes, n, fresh);
}

static int local_gb_restart(int reclone)
{
   return fe_host_gb_local_restart(reclone);
}

static size_t local_gb_state_max(void)
{
   return fe_host_gb_local_state_max();
}

static long local_gb_state_save(void *buf, size_t cap)
{
   return fe_host_gb_local_state_save(buf, cap);
}

static int local_gb_state_load(const void *buf, size_t n)
{
   return fe_host_gb_local_state_load(buf, n);
}
