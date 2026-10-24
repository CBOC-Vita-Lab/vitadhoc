/* White-box test: an in-game save must reach the .sav at exit even when the
 * held-open file handle has died underneath the writer.
 *
 * WHY THE HANDLE DIES.  The .sav is kept open "r+b" for the whole run
 * (fe_host.c, ADR-0020) and a PSP invalidates open Memory Stick files across
 * sleep (the same fact load_gamepak_page's reopen exists for).  The first
 * write after a wake therefore fails.  The delta writer's answer to a failed
 * write is sram_abandon_deltas(): close the file, forget the dirty set, and
 * leave the whole-image rewrite to "the next scan".
 *
 * THE DEFECT (3.5.0).  At exit there is no next scan.  fe_host_shutdown() is
 *      fe_host_sram_flush(0);   scan -> 14 dirty blocks -> first write fails
 *                               -> abandon: dirty set cleared, nothing queued
 *      fe_host_sram_sync();     nothing pending -> returns
 *      sram_close();
 * and the process ends with the save only in memory.  What is left on the
 * stick is whatever was there before: the previous save, or -- when the
 * writer's periodic scan happened to land in the middle of the game's save
 * before the sleep -- a slot with some new sectors and some old ones, which a
 * generation-3 Pokemon game reports as "The save file is corrupted.  The
 * previous save will be loaded."
 *
 * The dead handle is simulated by pointing the FILE's descriptor at a
 * read-only /dev/null: seeks succeed, every write fails, exactly one error
 * path further than a seek that fails.  Both are exercised.
 *
 * Sectors here are the 4 KiB blocks of the dirty map; a "save" rewrites 14 of
 * them, as the games do. */
#include <assert.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

/* S1 probe: the size of the save file on disk at the moment a write into it
 * begins (armed only around sram_write_full).  A "wb" open shows 0 here. */
static const char *probe_path;
static long probe_min_size = -1;
static size_t probe_fwrite(const void *p, size_t s, size_t n, FILE *f)
{
   if (probe_path)
   {
      struct stat st;
      if (stat(probe_path, &st) == 0 &&
          (probe_min_size < 0 || (long)st.st_size < probe_min_size))
         probe_min_size = (long)st.st_size;
   }
   return (fwrite)(p, s, n, f);
}
#define fwrite(p, s, n, f) probe_fwrite(p, s, n, f)

#include "../../frontend-common/fe_host.c"

volatile uint32_t backup_write_gen;

static uint8_t live_sram[FE_SRAM_SIZE];
static uint64_t fake_now = 1000;
static unsigned writer_passes;

static uint64_t fake_time_us(void) { return fake_now; }
static void fake_wake(void) { }
static void fake_yield(void)
{
   /* ~1 ms asleep on the emulation thread: the writer gets one pass. */
   fake_now += 1000;
   writer_passes++;
   (void)fe_host_sram_service_io();
}
static const fe_host_io fake_io = { fake_wake, fake_yield };

void *retro_get_memory_data(unsigned id)
{
   assert(id == RETRO_MEMORY_SAVE_RAM);
   return live_sram;
}

uint32_t fe_crc32(uint32_t crc, const void *data, size_t len)
{
   const uint8_t *p = (const uint8_t *)data;
   size_t i;
   unsigned bit;
   crc = ~crc;
   for (i = 0; i < len; i++)
   {
      crc ^= p[i];
      for (bit = 0; bit < 8; bit++)
         crc = (crc >> 1) ^ (0xEDB88320u & (uint32_t)-(int32_t)(crc & 1));
   }
   return ~crc;
}

void fe_evt(const char *fmt, ...) { (void)fmt; }
void fe_log(const char *fmt, ...) { (void)fmt; }

size_t gbcore_save_ram_size(gbcore_t *core) { (void)core; return 0; }
size_t gbcore_cart_ram_size(gbcore_t *core) { (void)core; return 0; }
int gbcore_save_ram_read(gbcore_t *core, void *dst, size_t cap)
{ (void)core; (void)dst; (void)cap; return -1; }
gbdual_t *fe_gblink_dual(const fe_gblink *s) { (void)s; return NULL; }
int fe_gblink_local_slot(const fe_gblink *s) { (void)s; return 0; }
const gbcore_api_t *gbdual_api(int slot) { (void)slot; return NULL; }
gbcore_t *gbdual_core(const gbdual_t *d, int slot) { (void)d; (void)slot; return NULL; }

static const char *path = "/tmp/gpsp-sram-exit-retry-test.sav";

/* The game writes sectors [first, first+n) of a save whose index is `tag`. */
static void game_save(unsigned first, unsigned n, uint8_t tag)
{
   unsigned i;
   for (i = first; i < first + n; i++)
   {
      memset(live_sram + (size_t)i * SRAM_BLOCK, tag, SRAM_BLOCK);
      live_sram[(size_t)i * SRAM_BLOCK] = (uint8_t)i;
      backup_write_gen++;
   }
}

static size_t read_disk(uint8_t *disk)
{
   FILE *f = fopen(path, "rb");
   size_t n;
   if (!f)
      return 0;
   n = fread(disk, 1, FE_SRAM_SIZE, f);
   fclose(f);
   return n;
}

static int disk_matches_live(void)
{
   static uint8_t disk[FE_SRAM_SIZE];
   return read_disk(disk) == FE_SRAM_SIZE &&
          memcmp(disk, live_sram, FE_SRAM_SIZE) == 0;
}

/* How many of sectors [first, first+n) on disk carry `tag`. */
static unsigned disk_sectors_with(unsigned first, unsigned n, uint8_t tag)
{
   static uint8_t disk[FE_SRAM_SIZE];
   unsigned i, hit = 0;
   if (read_disk(disk) != FE_SRAM_SIZE)
      return 0;
   for (i = first; i < first + n; i++)
      if (disk[(size_t)i * SRAM_BLOCK + 1] == tag)
         hit++;
   return hit;
}

/* Sleep and wake: the open .sav handle no longer accepts writes.
 * seek_fails = 1 makes the seek fail as well (a closed descriptor). */
static void kill_handle(int seek_fails)
{
   int fd, null_fd;
   assert(sram_fp);
   fd = fileno(sram_fp);
   if (seek_fails)
   {
      /* A pipe's read end: lseek -> ESPIPE, write -> EBADF. */
      int p[2];
      assert(pipe(p) == 0);
      assert(dup2(p[0], fd) == fd);
      close(p[0]);
      close(p[1]);
      return;
   }
   null_fd = open("/dev/null", O_RDONLY);
   assert(null_fd >= 0);
   assert(dup2(null_fd, fd) == fd);
   close(null_fd);
}

/* What the PSP's exit does to the save, in order (psp/main_psp.c teardown):
 * io_thread_stop() -- the writer's last pass, then synchronous ownership and
 * a sync -- followed by fe_host_shutdown()'s flush / sync / close. */
static void exit_like_the_psp(void)
{
   (void)fe_host_sram_service_io();   /* io_thread's "last pass" */
   fe_host_set_io(NULL);
   fe_host_sram_sync();
   sram_shutdown();                   /* fe_host_shutdown()'s save half */
}

static void start_session(void)
{
   FILE *f = fopen(path, "wb");
   assert(f);
   assert(fwrite(live_sram, 1, sizeof(live_sram), f) == sizeof(live_sram));
   assert(fclose(f) == 0);
   host.save_path = path;
   host.time_us = fake_time_us;
   sram_load();
   assert(sram_have_blk);
   fe_host_set_io(&fake_io);
}

/* The writer's own cadence comes round: one pass with a scan due. */
static void periodic_scan(void)
{
   fake_now += SRAM_SCAN_INTERVAL_US + 1;
   (void)fe_host_sram_service_io();
}

int main(void)
{
   int seek_fails;

   for (seek_fails = 0; seek_fails < 2; seek_fails++)
   {
      /* ---- A. save, sleep, wake, save again, exit at once -------------
       * Slot 1 = sectors 0-13, slot 2 = sectors 14-27. */
      memset(live_sram, 0xFF, sizeof(live_sram));
      game_save(0, 14, 0x10);            /* the save the session starts from */
      start_session();

      game_save(14, 14, 0x11);           /* first save of the session */
      periodic_scan();                   /* lands; the handle is now open */
      assert(disk_matches_live() && sram_fp);

      kill_handle(seek_fails);           /* the console sleeps and wakes */

      game_save(0, 14, 0x12);            /* the player saves again ...    */
      exit_like_the_psp();               /* ... and quits within 5 s      */
      assert(disk_sectors_with(0, 14, 0x12) == 14);   /* the save is there */
      assert(disk_matches_live());
      unlink(path);

      /* ---- B. the writer's scan lands mid-save, then sleep, wake, exit --
       * This is the sequence that leaves a slot half old, half new. */
      memset(live_sram, 0xFF, sizeof(live_sram));
      game_save(0, 14, 0x20);
      game_save(14, 14, 0x1F);
      start_session();

      game_save(14, 6, 0x21);            /* the game is 6 sectors into a save */
      periodic_scan();                   /* the 5 s scan lands right here */
      assert(disk_sectors_with(14, 14, 0x21) == 6 && sram_fp);
      game_save(20, 8, 0x21);            /* the game finishes: "saved."   */

      kill_handle(seek_fails);           /* sleep within 5 s of saving; wake */

      exit_like_the_psp();               /* Exit from the wake screen     */
      assert(disk_sectors_with(14, 14, 0x21) == 14);  /* not 6 of 14 */
      assert(disk_matches_live());
      unlink(path);

      /* ---- C. the same, but the player keeps playing after the wake ----
       * The writer's own scan hits the dead handle first; the rewrite it
       * leaves for later must still happen at exit. */
      memset(live_sram, 0xFF, sizeof(live_sram));
      game_save(0, 14, 0x30);
      start_session();
      game_save(14, 14, 0x31);
      periodic_scan();
      assert(disk_matches_live() && sram_fp);
      kill_handle(seek_fails);
      game_save(0, 14, 0x32);
      periodic_scan();                   /* fails on the dead handle */
      assert(!disk_matches_live());
      exit_like_the_psp();               /* before the next 5 s scan */
      assert(disk_matches_live());
      unlink(path);
   }

   /* ---- D. nothing to retry: a clean exit writes nothing extra ---------- */
   memset(live_sram, 0xFF, sizeof(live_sram));
   game_save(0, 14, 0x40);
   start_session();
   game_save(14, 14, 0x41);
   periodic_scan();
   assert(disk_matches_live());
   {
      unsigned before = sram_run_blocks;
      exit_like_the_psp();
      assert(sram_run_blocks == before && !sram_full_req && !sram_retry_full);
   }
   assert(disk_matches_live());

   /* S1: the whole-image rewrite (the routine path after every wake) never
    * leaves the file empty or short: an existing save of the right size is
    * overwritten in place.  A missing or wrong-sized file is created. */
   sram_close();
   live_sram[7 * 4096 + 1] ^= 0x5A;
   probe_path = path; probe_min_size = -1;
   assert(sram_write_full(live_sram) == 0);
   assert(probe_min_size == (long)FE_SRAM_SIZE);     /* never truncated */
   assert(disk_matches_live());
   assert(truncate(path, 100) == 0);                 /* wrong size: rewritten */
   assert(sram_write_full(live_sram) == 0 && disk_matches_live());
   unlink(path);                                     /* missing: created */
   assert(sram_write_full(live_sram) == 0 && disk_matches_live());
   probe_path = NULL;
   unlink(path);

   puts("PASS sram exit retry: a save survives a dead .sav handle at exit "
        "(write error and seek error), mid-save scan included");
   return 0;
}
