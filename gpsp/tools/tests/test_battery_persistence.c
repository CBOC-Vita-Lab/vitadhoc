/* Production battery persistence with deterministic I/O failure/interleaving.
 * --baseline records the pre-fix failures without accepting them as passes. */
#define _GNU_SOURCE
#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static uint8_t live_save[131072];
static int short_write, close_error, mutate_once, read_error, open_read_error;
static const char *save_path;
static unsigned rename_calls, rename_fail_mask;
volatile uint32_t backup_write_gen;

static size_t battery_write(const void *p, size_t s, size_t n, FILE *f)
{
   size_t wrote;
   if (short_write && s * n > 16)
      return fwrite(p, 1, 16, f) / s;
   wrote = fwrite(p, s, n, f);
   if (mutate_once)
   {
      mutate_once = 0;
      live_save[0] ^= 0x5A;
      backup_write_gen++;
   }
   return wrote;
}
static int battery_close(FILE *f)
{
   int rc = fclose(f);
   return close_error ? EOF : rc;
}
static size_t battery_read(void *p, size_t s, size_t n, FILE *f)
{ return fread(p, s, read_error && n > 16 ? 16 : n, f); }
static int battery_ferror(FILE *f)
{ return read_error || ferror(f); }
static FILE *battery_open(const char *path, const char *mode)
{
   if (open_read_error && !strcmp(mode, "rb") && !strcmp(path, save_path))
   {
      errno = EACCES;
      return NULL;
   }
   return fopen(path, mode);
}
static int battery_rename(const char *a, const char *b)
{
   assert(rename_calls < 32);
   if (rename_fail_mask & (1u << rename_calls++))
   {
      errno = EACCES;
      return -1;
   }
   return rename(a, b);
}
#define fwrite battery_write
#define fclose battery_close
#define rename battery_rename
#define fread battery_read
#define ferror battery_ferror
#define fopen battery_open
#include "../../frontend-common/fe_host.c"
#undef fwrite
#undef fclose
#undef rename
#undef fread
#undef ferror
#undef fopen

void fe_evt(const char *fmt, ...) { (void)fmt; }
void fe_log(const char *fmt, ...) { (void)fmt; }
void *retro_get_memory_data(unsigned id)
{ assert(id == RETRO_MEMORY_SAVE_RAM); return live_save; }
uint32_t fe_crc32(uint32_t crc, const void *buf, size_t len)
{
   const uint8_t *p = buf;
   unsigned bit;
   crc = ~crc;
   while (len--)
   {
      crc ^= *p++;
      for (bit = 0; bit < 8; bit++)
         crc = (crc >> 1) ^ (0xEDB88320u & (uint32_t)-(int32_t)(crc & 1));
   }
   return ~crc;
}
size_t gbcore_save_ram_size(gbcore_t *c) { (void)c; return 32768; }
size_t gbcore_cart_ram_size(gbcore_t *c) { (void)c; return 32768; }
int gbcore_save_ram_read(gbcore_t *c, void *dst, size_t cap)
{ (void)c; assert(cap >= 32768); memcpy(dst, live_save, 32768); return 0; }
int gbcore_save_ram_write(gbcore_t *c, const void *src, size_t n)
{ (void)c; memcpy(live_save, src, n < 32768 ? n : 32768); return 0; }
gbdual_t *fe_gblink_dual(const fe_gblink *s) { (void)s; return NULL; }
int fe_gblink_local_slot(const fe_gblink *s) { (void)s; return 0; }
const gbcore_api_t *gbdual_api(int slot) { (void)slot; return NULL; }
gbcore_t *gbdual_core(const gbdual_t *d, int slot) { (void)d; (void)slot; return NULL; }

static void seed(const char *path, size_t n)
{
   uint8_t block[4096];
   FILE *f = fopen(path, "wb");
   assert(f);
   memset(block, 0x31, sizeof(block));
   while (n)
   {
      size_t take = n < sizeof(block) ? n : sizeof(block);
      assert(fwrite(block, 1, take, f) == take);
      n -= take;
   }
   assert(fclose(f) == 0);
}
static int matches(const char *path, const uint8_t *p, size_t n, int value)
{
   FILE *f = fopen(path, "rb");
   size_t i;
   int ok = 1;
   if (!f) return 0;
   for (i = 0; i < n; i++)
      if (fgetc(f) != (p ? p[i] : value)) { ok = 0; break; }
   if (ok && (fgetc(f) != EOF || ferror(f))) ok = 0;
   assert(fclose(f) == 0);
   return ok;
}
static void reset_faults(void)
{
   short_write = close_error = mutate_once = read_error = open_read_error = 0;
   rename_calls = rename_fail_mask = 0;
}
static int write_kind(int kind)
{
   if (kind == 0) return sram_write_full(live_save);
   if (kind == 1) return gb_save_flush(1);
   return gbl_commit(NULL, live_save, 32768);
}
static void test_failed_replacement(const char *path, int baseline)
{
   int kind, fault, shorter;
   for (kind = 0; kind < 3; kind++)
      for (fault = 0; fault < 2; fault++)
         for (shorter = 0; shorter < 2; shorter++)
         {
            size_t size = kind ? 32768 : FE_SRAM_SIZE;
            size_t old_size = shorter ? size / 2 : size;
            int rc, intact;
            seed(path, old_size);
            gb_save_file_len = (long)old_size;
            memset(live_save, 0x52, sizeof(live_save));
            reset_faults();
            short_write = fault == 0;
            close_error = fault == 1;
            rc = write_kind(kind);
            reset_faults();
            intact = matches(path, NULL, old_size, 0x31);
            printf("writer=%d fault=%s old_size=%zu result=%d original_intact=%d\n",
                   kind, fault ? "close" : "short_write", old_size, rc, intact);
            assert(rc < 0);
            if (!baseline) assert(intact);
         }
}
static void test_full_write_interleaving(const char *path, int baseline)
{
   uint8_t disk[FE_SRAM_SIZE];
   FILE *f;
   int baseline_exact, caught;
   seed(path, FE_SRAM_SIZE);
   memset(live_save, 0x64, sizeof(live_save));
   sram_have_blk = 0;
   sram_full_req = 1;
   memset((void *)sram_blk_state, SBLK_CLEAN, sizeof(sram_blk_state));
   mutate_once = 1; /* game writes after block zero has reached the file */
   assert(sram_drain(0) == 0);
   f = fopen(path, "rb"); assert(f);
   assert(fread(disk, 1, sizeof(disk), f) == sizeof(disk));
   assert(fclose(f) == 0);
   assert(disk[0] != live_save[0]);
   baseline_exact = sram_blk_crc[0] == fe_crc32(0, disk, SRAM_BLOCK);
   caught = sram_scan(0);
   printf("full write interleaving: CRC_matches_disk=%d next_scan_detected_change=%d\n",
          baseline_exact, caught);
   if (!baseline)
   {
      assert(baseline_exact && caught);
      assert(sram_drain(0) == 0);
      assert(matches(path, live_save, sizeof(live_save), 0));
   }
   sram_close();
}
static void test_full_write_generation(const char *path)
{
   seed(path, FE_SRAM_SIZE);
   memset(live_save, 0x43, sizeof(live_save));
   gb_family = 0;
   sram_have_blk = 0;
   sram_full_req = 0;
   sram_scan_req = 1;
   sram_scan_gen_valid = 0;
   backup_write_gen = 7;
   mutate_once = 1;
   fe_host_sram_service_io();
   assert(backup_write_gen == 8 && sram_disk_gen == 7);
   sram_scan_req = 1;
   fe_host_sram_service_io();
   assert(sram_disk_gen == 8);
   assert(matches(path, live_save, FE_SRAM_SIZE, 0));
   sram_close();
   puts("writer generations: concurrent change not credited until next scan/drain");
}
static void test_recovery(const char *path)
{
   char old[512], bak[512], tmp[512];
   int kind;
   snprintf(old, sizeof(old), "%s.old", path);
   snprintf(bak, sizeof(bak), "%s.bak", path);
   snprintf(tmp, sizeof(tmp), "%s.tmp", path);
   assert(remove(path) == 0);
   seed(old, FE_SRAM_SIZE);
   seed(bak, 32); /* obsolete pre-share backup must not beat the newer .old */
   reset_faults();
   sram_load();
   assert(matches(path, NULL, FE_SRAM_SIZE, 0x31));
   assert(live_save[0] == 0x31 && access(old, F_OK) != 0);
   assert(matches(bak, NULL, 32, 0x31));
   assert(remove(path) == 0);
   assert(remove(bak) == 0);
   seed(bak, FE_SRAM_SIZE);
   sram_load();
   assert(matches(path, NULL, FE_SRAM_SIZE, 0x31));
   assert(access(bak, F_OK) != 0);
   assert(remove(path) == 0);
   seed(old, 32768);
   gb_save_load();
   assert(matches(path, NULL, 32768, 0x31));
   assert(access(old, F_OK) != 0);

   /* If rollback failed, another full write cannot discard the sole copy. */
   for (kind = 0; kind < 3; kind++)
   {
      assert(remove(path) == 0);
      seed(old, 32768);
      reset_faults();
      rename_fail_mask = 1u;
      assert(write_kind(kind) < 0);
      assert(matches(old, NULL, 32768, 0x31));
      assert(access(path, F_OK) != 0 && access(tmp, F_OK) != 0);
      reset_faults();
      assert(write_kind(kind) >= 0);
      assert(matches(path, live_save, kind ? 32768 : FE_SRAM_SIZE, 0));
   }
   puts("battery recovery: .old wins, .bak fallback works, failed recovery retains sole copy");
}
static void test_load_failures(const char *path, int baseline)
{
   int kind, fault;
   for (kind = 0; kind < 2; kind++)
      for (fault = 0; fault < 3; fault++)
      {
         size_t size = kind ? 32768 : FE_SRAM_SIZE;
         int intact;
         seed(path, size);
         memset(live_save, 0xEE, sizeof(live_save));
         gb_family = kind;
         read_error = fault == 0;
         close_error = fault == 1;
         open_read_error = fault == 2;
         if (kind) gb_save_load(); else sram_load();
         reset_faults();
         if (!baseline) assert(fe_host_sram_flush(1) < 0);
         /* Shutdown must not write RAM loaded from a failed file read. */
         sram_shutdown();
         intact = matches(path, NULL, size, 0x31);
         printf("load kind=%d fault=%s shutdown_preserved_save=%d\n",
                kind, fault == 2 ? "open" : fault ? "close" : "read", intact);
         if (!baseline) assert(intact);
      }
   gb_family = 0;
   /* Successful next-game load must lift the write inhibit, and real
    * short legacy images remain importable (not mistaken for I/O errors). */
   seed(path, 32768);
   memset(live_save, 0xFF, sizeof(live_save));
   sram_load();
   assert(live_save[0] == 0x31 && live_save[32767] == 0x31);
   assert(live_save[32768] == 0xFF);
   assert(fe_host_sram_flush(1) >= 0);
   assert(matches(path, live_save, FE_SRAM_SIZE, 0));
   sram_close();
   seed(path, 16384);
   gb_save_load();
   assert(gb_save_flush(1) >= 0);
   assert(matches(path, live_save, 32768, 0));
   puts("legacy short images: accepted and converted on successful load");
}
static void test_load_recovery_failures(const char *path)
{
   char old[512];
   int kind, suffix;
   for (kind = 0; kind < 2; kind++)
      for (suffix = 0; suffix < 2; suffix++)
      {
         size_t size = kind ? 32768 : FE_SRAM_SIZE;
         snprintf(old, sizeof(old), "%s.%s", path, suffix ? "bak" : "old");
         assert(remove(path) == 0);
         seed(old, size);
         gb_family = kind;
         memset(live_save, 0xEE, sizeof(live_save));
         reset_faults();
         rename_fail_mask = 1u;
         if (kind) assert(gb_save_load() < 0); else assert(sram_load() < 0);
         reset_faults();
         sram_shutdown();
         assert(access(path, F_OK) != 0);
         assert(matches(old, NULL, size, 0x31));
         if (kind) assert(gb_save_load() == 0); else assert(sram_load() == 0);
         assert(matches(path, NULL, size, 0x31));
         assert(access(old, F_OK) != 0);
      }
   gb_family = 0;
   puts("failed recovery at load: boot refuses, shutdown keeps backup, retry loads it");
}
int main(int argc, char **argv)
{
   char dir[] = "/tmp/gbadhoc-battery-review-XXXXXX", path[256];
   int baseline = argc > 1 && !strcmp(argv[1], "--baseline");
   assert(mkdtemp(dir));
   snprintf(path, sizeof(path), "%s/game.sav", dir);
   host.save_path = path;
   save_path = path;
   gb_core = (gbcore_t *)live_save; /* stubs do not dereference it */
   test_failed_replacement(path, baseline);
   test_full_write_interleaving(path, baseline);
   if (!baseline) test_full_write_generation(path);
   if (!baseline) test_recovery(path);
   test_load_failures(path, baseline);
   if (!baseline) test_load_recovery_failures(path);
   free(gb_save_buffer);
   gb_save_buffer = NULL;
   unlink(path);
   assert(rmdir(dir) == 0);
   puts(baseline ? "CONFIRMED battery persistence failures" : "PASS battery persistence regressions");
   return 0;
}
