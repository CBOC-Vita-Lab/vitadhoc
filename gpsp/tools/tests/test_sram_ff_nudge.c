/* White-box test for the FF durability nudge (fe_host.c): a battery save made
 * during fast-forward is persisted by the io writer without waiting for FF to
 * end, and nothing at all happens when no save is pending.
 *
 * The writer thread is simulated: the fake io yield runs one writer pass
 * (fe_host_sram_service_io) when `writer_runs` is set, which is exactly what a
 * ~1 ms sleep on the emulation thread gives the real one.  With it clear, the
 * writer is starved -- the Unlimited-FF case the nudge exists for. */
#include <assert.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "../../frontend-common/fe_host.c"

/* The core's counter (gba_memory.c).  Defining it here satisfies the weak
 * reference in fe_host.c. */
volatile uint32_t backup_write_gen;

static uint8_t live_sram[FE_SRAM_SIZE];
static unsigned fake_wakes, fake_yields, writer_passes;
static int writer_runs = 1;
/* A game AT full speed: the writer gets the CPU in the vblank slack, so a
 * wake is served before the next frame.  Clear = only a yield serves it. */
static int writer_on_wake;
static unsigned scans;          /* writer passes that ran a scan */
static uint64_t fake_now = 1000;

/* Mid-scan injection: when armed, the Nth CRC call of the NEXT writer pass
 * mutates live SRAM block `inject_blk` and bumps the generation, as a game
 * write racing the scan would. */
static int inject_at = -1, inject_blk, crc_calls;

static uint64_t fake_time_us(void) { return fake_now; }

static void writer_pass(void)
{
   int req = sram_scan_req != 0;
   crc_calls = 0;
   writer_passes++;
   (void)fe_host_sram_service_io();
   if (req && !sram_scan_req)
      scans++;
   inject_at = -1;
}

static void fake_wake(void)
{
   fake_wakes++;
   if (writer_on_wake)
      writer_pass();
}
static void fake_yield(void)
{
   fake_yields++;
   fake_now += 1000;
   if (writer_runs)
      writer_pass();
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
   if (inject_at >= 0 && crc_calls++ == inject_at)
   {
      live_sram[(size_t)inject_blk * SRAM_BLOCK + 17] ^= 0x5A;
      backup_write_gen++;
   }
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
/* A GB link session is never up in this test (gb_family is 0). */
gbdual_t *fe_gblink_dual(const fe_gblink *s) { (void)s; return NULL; }
int fe_gblink_local_slot(const fe_gblink *s) { (void)s; return 0; }
const gbcore_api_t *gbdual_api(int slot) { (void)slot; return NULL; }
gbcore_t *gbdual_core(const gbdual_t *d, int slot) { (void)d; (void)slot; return NULL; }

static const char *path = "/tmp/gpsp-sram-ff-nudge-test.sav";

/* One emulated frame followed by the main loop's nudge call. */
static int frame(int ff)
{
   frame_count++;
   fake_now += 16667;
   return fe_host_sram_ff_nudge(ff);
}

static unsigned frames(unsigned n, int ff)
{
   unsigned y = 0;
   while (n--)
      y += (unsigned)frame(ff);
   return y;
}

/* The game saves: mutate a block and bump the generation, as write_backup
 * does. */
static void game_write(unsigned blk, uint8_t v)
{
   live_sram[(size_t)blk * SRAM_BLOCK + 3] = v;
   backup_write_gen++;
}

/* The clock at each generation's write, so the age of the OLDEST write that is
 * not on the stick can be read off sram_disk_gen at any moment. */
#define GEN_LOG 8192
static uint64_t gen_us[GEN_LOG];
static uint32_t gen_base;

static void game_write_logged(unsigned blk, uint8_t v)
{
   game_write(blk, v);
   assert(backup_write_gen - gen_base < GEN_LOG);
   gen_us[backup_write_gen - gen_base] = fake_now;
}

static uint64_t oldest_unpersisted_age(void)
{
   uint32_t d = sram_disk_gen;
   if (d == backup_write_gen)
      return 0;
   return fake_now - gen_us[d + 1 - gen_base];
}

static int disk_matches_live(void)
{
   uint8_t disk[FE_SRAM_SIZE];
   FILE *f = fopen(path, "rb");
   size_t n;
   if (!f)
      return 0;
   n = fread(disk, 1, sizeof(disk), f);
   fclose(f);
   return n == sizeof(disk) && memcmp(disk, live_sram, sizeof(disk)) == 0;
}

int main(void)
{
   FILE *f;
   unsigned y, i;

   memset(live_sram, 0x31, sizeof(live_sram));
   f = fopen(path, "wb");
   assert(f);
   assert(fwrite(live_sram, 1, sizeof(live_sram), f) == sizeof(live_sram));
   assert(fclose(f) == 0);

   backup_write_gen = 7;   /* whatever the game wrote before the load */
   host.save_path = path;
   host.time_us = fake_time_us;
   sram_load();
   assert(sram_have_blk && sram_disk_gen == 7 && !ffn_pending);
   fe_host_set_io(&fake_io);

   /* 1. Nothing pending: zero yields, zero wakes, in FF or out of it. */
   assert(frames(2000, 1) == 0 && frames(100, 0) == 0);
   assert(fake_yields == 0 && fake_wakes == 0);

   /* 2. A save at NORMAL SPEED: nothing while the game is still writing;
    *    once it has been quiet for SRAM_FF_SETTLE_FRAMES a scan is asked for
    *    -- a flag and a wake, never a yield -- and asked again each settle
    *    period until the writer has put it on disk. */
   game_write(4, 0xA1);
   assert(frames(SRAM_FF_SETTLE_FRAMES, 0) == 0);
   assert(fake_wakes == 0 && sram_scan_req == 0 && ffn_pending);
   game_write(5, 0xA2);                  /* the save is still writing */
   assert(frames(SRAM_FF_SETTLE_FRAMES, 0) == 0);
   assert(fake_wakes == 0 && sram_scan_req == 0);
   assert(frames(1, 0) == 0);
   assert(fake_wakes == 1 && sram_scan_req == 1 && fake_yields == 0);
   assert(!disk_matches_live());         /* the writer has not run yet */
   assert(frames(SRAM_FF_SETTLE_FRAMES - 1, 0) == 0 && fake_wakes == 1);
   assert(frames(1, 0) == 0 && fake_wakes == 2);   /* asked again */
   (void)fe_host_sram_service_io();      /* the writer, in the vblank slack */
   assert(sram_disk_gen == backup_write_gen && disk_matches_live());
   assert(frames(500, 0) == 0 && !ffn_pending && fake_wakes == 2);
   assert(fake_yields == 0);

   /* 3. FF starts with a save still unpersisted and long settled (the
    *    writer never got to it at 1x): the first FF frame nudges, the
    *    writer persists it, and it clears. */
   game_write(4, 0xA3);
   assert(frames(60, 0) == 0 && ffn_pending && fake_yields == 0);
   frames(SRAM_FF_SETTLE_FRAMES, 1);
   assert(writer_passes == 1 && fake_yields == 1);
   assert(sram_disk_gen == backup_write_gen && disk_matches_live());
   assert(frames(500, 1) == 0 && !ffn_pending);

   /* 3b. NORMAL SPEED, the writer starved (a game below full speed: main
    *     never blocks at the vblank, so asking alone gets nothing written).
    *     Up to SRAM_STARVED_US after the last write: asked, never a yield.
    *     Past it the save gets what fast-forward gives it -- main sleeps
    *     for the writer -- and it is on disk at the first such nudge. */
   game_write(4, 0xA4);
   y = fake_yields;
   i = fake_wakes;
   assert(frames(89, 0) == 0 && fake_yields == y && fake_wakes > i);
   assert(sram_disk_gen != backup_write_gen && !ffn_starved_said);
   assert(frames(2, 0) == 1 && fake_yields == y + 1 && ffn_starved_said);
   assert(sram_disk_gen == backup_write_gen && disk_matches_live());
   assert(frames(500, 0) == 0 && !ffn_pending && fake_yields == y + 1);
   /* ...bounded like fast-forward's when the writer cannot persist at all,
    * and a new save re-arms it. */
   writer_runs = 0;
   game_write(4, 0xA5);
   y = fake_yields;
   frames(90 + SRAM_FF_NUDGE_CAP * SRAM_FF_NUDGE_EVERY * 2, 0);
   assert(fake_yields == y + SRAM_FF_NUDGE_CAP && ffn_gave_up);
   writer_runs = 1;
   game_write(4, 0xA6);
   frames(120, 0);
   assert(sram_disk_gen == backup_write_gen && disk_matches_live());
   assert(!ffn_pending && !ffn_gave_up);

   /* 4. A save made DURING FF waits SRAM_FF_SETTLE_FRAMES of quiet first,
    *    then is persisted by a single nudge. */
   game_write(9, 0xB2);
   y = frames(SRAM_FF_SETTLE_FRAMES - 1, 1);
   assert(y == 0);
   game_write(10, 0xB3);                 /* the save is still writing */
   y = frames(SRAM_FF_SETTLE_FRAMES, 1);
   assert(y == 0);                       /* settle restarts at the last write */
   y = frames(1, 1);
   assert(y == 1 && disk_matches_live());
   assert(frames(100, 1) == 0 && !ffn_pending);

   /* 5. A write racing the writer's scan must not be lost: the pass that
    *    started before it publishes only the generation it read, so the
    *    nudge keeps going until a later pass has seen the new bytes.
    *    Block 2 is CRC'd before the injection at CRC call 8 (block 4). */
   game_write(20, 0xC4);
   inject_at = 8;
   inject_blk = 2;
   y = frames(SRAM_FF_SETTLE_FRAMES + 1, 1);
   assert(y == 1);
   assert(sram_disk_gen != backup_write_gen);  /* raced write not credited */
   assert(!disk_matches_live());               /* ...and indeed not on disk */
   y = frames(SRAM_FF_SETTLE_FRAMES + 5, 1);
   assert(y == 1 && sram_disk_gen == backup_write_gen && disk_matches_live());
   assert(frames(100, 1) == 0 && !ffn_pending);

   /* 5b. (review S4) A block that changes AGAIN between the scan and its
    *     write, then goes back to what the scan saw: the stick holds the
    *     intermediate bytes.  The next scan must see that from the per-block
    *     record of what was written -- the whole image is the one the last
    *     scan saw, and comparing with that called it persisted.
    *     Block 20 is CRC'd at calls 40-41; the injection at call 60 lands
    *     after that and before the drain copies the block. */
   game_write(20, 0xC5);
   inject_at = 60;
   inject_blk = 20;
   crc_calls = 0;
   fe_host_sram_request_scan();
   (void)fe_host_sram_service_io();
   inject_at = -1;
   assert(!disk_matches_live() || sram_disk_gen != backup_write_gen);
   live_sram[(size_t)20 * SRAM_BLOCK + 17] ^= 0x5A;   /* ...and back */
   backup_write_gen++;
   assert(!disk_matches_live());
   fe_host_sram_request_scan();
   (void)fe_host_sram_service_io();
   assert(disk_matches_live() && sram_disk_gen == backup_write_gen);
   frames(2, 1);
   assert(!ffn_pending);

   /* 6. Starved writer: one yield per SRAM_FF_NUDGE_EVERY frames, bounded by
    *    SRAM_FF_NUDGE_CAP, then silence until the next save re-arms it. */
   writer_runs = 0;
   game_write(5, 0xD5);
   frames(SRAM_FF_SETTLE_FRAMES - 1, 1);
   y = frames(100, 1);
   assert(y == (100 + SRAM_FF_NUDGE_EVERY - 1) / SRAM_FF_NUDGE_EVERY);
   y += frames(SRAM_FF_NUDGE_CAP * SRAM_FF_NUDGE_EVERY * 2, 1);
   assert(y == SRAM_FF_NUDGE_CAP && ffn_gave_up);
   /* A new save re-arms it.  The OLDER save has now been pending for longer
    * than SRAM_FF_MAX_DEFER_FRAMES, so there is no settle wait: nudging
    * resumes on the very next frame, one yield per SRAM_FF_NUDGE_EVERY. */
   game_write(6, 0xD6);
   y = frames(SRAM_FF_SETTLE_FRAMES + 1, 1);
   assert(y == (SRAM_FF_SETTLE_FRAMES + SRAM_FF_NUDGE_EVERY) /
               SRAM_FF_NUDGE_EVERY && !ffn_gave_up);
   writer_runs = 1;
   frames(SRAM_FF_NUDGE_EVERY, 1);
   assert(sram_disk_gen == backup_write_gen && disk_matches_live());
   assert(frames(50, 1) == 0 && !ffn_pending);

   /* 7. A game that never stops writing is still persisted, after
    *    SRAM_FF_MAX_DEFER_FRAMES, while it keeps writing. */
   y = 0;
   for (i = 0; i < SRAM_FF_MAX_DEFER_FRAMES + 10; i++)
   {
      game_write(12, (uint8_t)i);
      y += (unsigned)frame(1);
   }
   assert(y > 0);

   /* 8. An I/O failure is never reported as persisted. */
   frames(SRAM_FF_SETTLE_FRAMES * 2, 1);
   assert(!ffn_pending);
   host.save_path = "/tmp/gpsp-sram-ff-missing-dir/save.sav";
   sram_close();                         /* force a reopen on the bad path */
   game_write(15, 0xE7);
   y = frames(SRAM_FF_SETTLE_FRAMES + 20, 1);
   assert(y > 0 && sram_disk_gen != backup_write_gen && ffn_pending);
   host.save_path = path;
   frames(SRAM_FF_NUDGE_EVERY * 2, 1);
   assert(sram_disk_gen == backup_write_gen && disk_matches_live());

   /* 8b. (review S3) A game that NEVER STOPS WRITING, at full speed with a
    *     healthy writer: one request per SRAM_FF_MAX_DEFER_FRAMES -- 3.5.0's
    *     5 s scan -- not one every settle period for ever, and never a
    *     yield.  Each request is served: nothing unpersisted is older than
    *     the deferral plus a frame. */
   frames(SRAM_FF_SETTLE_FRAMES * 2, 1);
   assert(!ffn_pending);
   writer_on_wake = 1;
   scans = 0;
   y = fake_yields;
   gen_base = backup_write_gen;
   {
      uint64_t worst = 0;
      for (i = 0; i < 10 * SRAM_FF_MAX_DEFER_FRAMES + 150; i++)
      {
         game_write_logged(12, (uint8_t)i);
         (void)frame(0);
         if (oldest_unpersisted_age() > worst)
            worst = oldest_unpersisted_age();
      }
      assert(fake_yields == y && !ffn_starved_said);
      assert(scans == 10);
      assert(worst <= (SRAM_FF_MAX_DEFER_FRAMES + 2) * 16667u);
   }
   /* ...and it stops writing: on disk a settle period later. */
   frames(SRAM_FF_SETTLE_FRAMES + 2, 0);
   assert(!ffn_pending && disk_matches_live() && scans == 11);
   writer_on_wake = 0;

   /* 8c. (S3) The same game in FAST-FORWARD: one request per deferral, one
    *     yield each (the fake writer finishes in one), not a yield and a
    *     scan every other frame. */
   scans = 0;
   y = fake_yields;
   for (i = 0; i < 10 * SRAM_FF_MAX_DEFER_FRAMES + 150; i++)
   {
      game_write(12, (uint8_t)i);
      (void)frame(1);
   }
   assert(fake_yields - y == 10 && scans == 10);
   frames(SRAM_FF_SETTLE_FRAMES + 2, 1);
   assert(!ffn_pending && disk_matches_live());

   /* 8d. (review S2) NORMAL SPEED, the writer starved, and the game writes
    *     again every second: the newest write is never SRAM_STARVED_US old,
    *     so the rule keyed on it never fired and nothing reached the stick
    *     until the game stopped.  The oldest unpersisted write is the key:
    *     past SRAM_STARVED_OLDEST_US main sleeps for the writer. */
   scans = 0;
   y = fake_yields;
   gen_base = backup_write_gen;
   {
      uint64_t worst = 0;
      for (i = 0; i < 3600; i++)
      {
         if (i % 60 == 0)
            game_write_logged(13, (uint8_t)(i / 60));
         (void)frame(0);
         if (oldest_unpersisted_age() > worst)
            worst = oldest_unpersisted_age();
      }
      assert(fake_yields > y && ffn_yields);
      /* the bound: the constant, plus the write period the request waits
       * for the gate's frame, plus the nudge itself */
      assert(worst <= SRAM_STARVED_OLDEST_US + 2 * 16667u + 1000u);
      /* 60 s of it: a handful of sleeps, not one every other frame */
      assert(fake_yields - y <= 3600 / 300 + 2 && scans <= 3600 / 300 + 2);
   }
   frames(120, 0);
   assert(!ffn_pending && disk_matches_live());

   /* 8e. (S2 + S3) The same, writing EVERY frame: the bound holds, and the
    *     sleeps are one per request. */
   scans = 0;
   y = fake_yields;
   gen_base = backup_write_gen;
   {
      uint64_t worst = 0;
      for (i = 0; i < 3600; i++)
      {
         game_write_logged(13, (uint8_t)i);
         (void)frame(0);
         if (oldest_unpersisted_age() > worst)
            worst = oldest_unpersisted_age();
      }
      assert(fake_yields > y);
      assert(worst <= SRAM_STARVED_OLDEST_US + 2 * 16667u + 1000u);
      assert(fake_yields - y <= 3600 / 300 + 2 && scans <= 3600 / 300 + 2);
   }
   frames(120, 0);
   assert(!ffn_pending && disk_matches_live());

   /* 8f. A writer that CANNOT persist and a game that never stops writing:
    *     the yield cap is spent once, not once per write -- at normal speed
    *     and in fast-forward. */
   writer_runs = 0;
   y = fake_yields;
   for (i = 0; i < 4000; i++)
   {
      game_write(13, (uint8_t)i);
      (void)frame(0);
   }
   assert(fake_yields - y == SRAM_FF_NUDGE_CAP && ffn_gave_up);
   for (i = 0; i < 4000; i++)
   {
      game_write(13, (uint8_t)i);
      (void)frame(1);
   }
   assert(fake_yields - y == SRAM_FF_NUDGE_CAP && ffn_gave_up);
   /* the writer comes back: the next request is served and it clears */
   writer_runs = 1;
   writer_on_wake = 1;
   for (i = 0; i < SRAM_FF_MAX_DEFER_FRAMES; i++)
   {
      game_write(13, (uint8_t)(i + 1));
      (void)frame(0);
   }
   frames(SRAM_FF_SETTLE_FRAMES + 2, 0);
   assert(!ffn_pending && !ffn_gave_up && disk_matches_live());
   writer_on_wake = 0;

   /* 9. Disabled (the harness A/B arm): never yields or asks, still
    *    tracks -- in fast-forward and at normal speed. */
   fe_host_sram_ff_nudge_enable(0);
   y = fake_yields;
   i = fake_wakes;
   game_write(3, 0xF8);
   assert(frames(1000, 1) == 0 && fake_yields == y && ffn_pending);
   assert(frames(1000, 0) == 0 && fake_wakes == i && sram_scan_req == 0);
   fe_host_sram_ff_nudge_enable(1);

   /* 9b. The suspend request: a flag and a wake, whatever the speed, and
    *     the writer's next pass puts the save on disk. */
   frames(SRAM_FF_SETTLE_FRAMES * 2, 1);
   assert(!ffn_pending);
   game_write(7, 0x99);
   i = fake_wakes;
   fe_host_sram_request_scan();
   assert(sram_scan_req == 1 && fake_wakes == i + 1 && !disk_matches_live());
   (void)fe_host_sram_service_io();
   assert(disk_matches_live());
   frames(2, 0);
   assert(!ffn_pending);
   y = fake_yields;

   /* 10. No writer thread installed (synchronous path): a no-op. */
   fe_host_set_io(NULL);
   assert(frames(1000, 1) == 0 && fake_yields == y);

   /* 11. The in-process return to the game list: requests left over from
    *     the game that was shut down are not the next game's. */
   sram_full_req = 1;
   sram_retry_full = 1;
   sram_scan_req = 2;
   fe_host_session_reset();
   assert(!sram_full_req && !sram_retry_full && !sram_scan_req);

   sram_close();
   unlink(path);
   puts("PASS sram FF nudge: idle-free, settles, persists, survives a racing "
        "write, bounded when starved, never credits a failed write");
   return 0;
}
