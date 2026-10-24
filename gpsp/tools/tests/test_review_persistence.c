/* Functional review of production persistence paths. Normal I/O failures are
 * injected at stdio boundaries; the production state/save functions are used.
 * --baseline records the v4.0.0 failures; without it this is a regression test. */
#define _GNU_SOURCE
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <errno.h>

static int short_write, close_error;
static unsigned rename_calls, rename_fail_mask, rename_lie_success_mask;
static unsigned rename_lie_failure_mask;
static int remove_old_error;
static unsigned live_allocations;
static size_t review_write(const void *p, size_t s, size_t n, FILE *f)
{
   if (short_write && s * n > 16)
      return fwrite(p, 1, 16, f) / s;
   return fwrite(p, s, n, f);
}
static int review_close(FILE *f)
{
   int rc = fclose(f);
   return close_error ? EOF : rc;
}
static void *review_alloc(size_t n)
{
   void *p = malloc(n);
   if (p) live_allocations++;
   return p;
}
static void review_free(void *p)
{
   if (p) live_allocations--;
   free(p);
}
static int review_rename(const char *from, const char *to)
{
   unsigned bit;
   int rc;
   assert(rename_calls < 32);
   bit = 1u << rename_calls++;
   if (rename_fail_mask & bit)
   {
      errno = EACCES;
      return -1;
   }
   /* Emulate PSP/PPSSPP disagreeing with the actual filesystem effect. */
   if (rename_lie_success_mask & bit)
      return 0;
   rc = rename(from, to);
   if (rc == 0 && (rename_lie_failure_mask & bit))
   {
      errno = EIO;
      return -1;
   }
   return rc;
}
static int review_remove(const char *path)
{
   size_t n = strlen(path);
   if (remove_old_error && n >= 4 && !strcmp(path + n - 4, ".old"))
   {
      errno = EACCES;
      return -1;
   }
   return remove(path);
}
#define fwrite review_write
#define fclose review_close
#define malloc review_alloc
#define free review_free
#define rename review_rename
#define remove review_remove
#include "../../frontend-common/fe_host.c"
#undef fwrite
#undef fclose
#undef malloc
#undef free
#undef rename
#undef remove

void fe_evt(const char *fmt, ...) { (void)fmt; }
void fe_log(const char *fmt, ...) { (void)fmt; }
uint32_t fe_crc32(uint32_t crc, const void *buf, size_t len)
{ (void)buf; (void)len; return crc; }
size_t retro_serialize_size(void) { return FE_STATE_MAX; }
bool retro_serialize(void *data, size_t size)
{ memset(data, 0x5A, size); return true; }
bool retro_unserialize(const void *data, size_t size)
{ return size == FE_STATE_MAX && ((const uint8_t *)data)[0] == 0x5A; }
long gbcore_state_save(gbcore_t *c, void *p, size_t n)
{ (void)c; (void)p; (void)n; return -1; }
size_t gbcore_state_size(gbcore_t *c) { (void)c; return 0; }
int gbcore_state_load(gbcore_t *c, const void *p, size_t n)
{ (void)c; (void)p; (void)n; return -1; }

static void seed(const char *path)
{
   uint8_t original[64];
   FILE *f = fopen(path, "wb");
   assert(f);
   memset(original, 0x31, sizeof(original));
   assert(fwrite(original, 1, sizeof(original), f) == sizeof(original));
   assert(fclose(f) == 0);
}
static int original_intact(const char *path)
{
   uint8_t data[65];
   FILE *f = fopen(path, "rb");
   size_t n;
   if (!f) return 0;
   n = fread(data, 1, sizeof(data), f);
   fclose(f);
   if (n != 64) return 0;
   while (n) if (data[--n] != 0x31) return 0;
   return 1;
}

static void reset_faults(void)
{
   rename_calls = rename_fail_mask = rename_lie_success_mask = 0;
   rename_lie_failure_mask = 0;
   remove_old_error = short_write = close_error = 0;
}

static int saved_state_intact(const char *path)
{
   FILE *f = fopen(path, "rb");
   size_t i;
   int ok = 1;
   if (!f) return 0;
   for (i = 0; i < FE_STATE_MAX; i++)
      if (fgetc(f) != 0x5A) { ok = 0; break; }
   if (ok && fgetc(f) != EOF) ok = 0;
   if (ferror(f)) ok = 0;
   fclose(f);
   return ok;
}

static void test_state_replacement(const char *path)
{
   char old[512], tmp[512];
   snprintf(old, sizeof(old), "%s.old", path);
   snprintf(tmp, sizeof(tmp), "%s.tmp", path);

   /* Refusing to move the original never loses it or leaves a temp file. */
   seed(path);
   reset_faults();
   rename_fail_mask = 1u;
   assert(fe_host_state_save(path) < 0);
   assert(rename_calls == 1 && original_intact(path));
   assert(access(old, F_OK) != 0 && access(tmp, F_OK) != 0);
   puts("backup rename failure: original kept, temp removed");

   /* The original is restored if promotion fails after the backup moved. */
   reset_faults();
   rename_fail_mask = 2u;
   assert(fe_host_state_save(path) < 0);
   assert(rename_calls == 3 && original_intact(path));
   assert(access(old, F_OK) != 0 && access(tmp, F_OK) != 0);
   puts("promotion failure: rollback restored original");

   /* If both promotion and rollback fail, the old slot remains recoverable.
    * Start with a real serialized slot so load verifies recovery end to end. */
   reset_faults();
   assert(fe_host_state_save(path) == 0 && saved_state_intact(path));
   reset_faults();
   rename_fail_mask = 2u | 4u;
   assert(fe_host_state_save(path) < 0);
   assert(rename_calls == 3 && access(path, F_OK) != 0);
   assert(saved_state_intact(old) && access(tmp, F_OK) != 0);

   reset_faults();
   rename_fail_mask = 1u;
   assert(fe_host_state_load(path) < 0);
   assert(saved_state_intact(old) && access(path, F_OK) != 0);
   /* A new save must not destroy the sole backup when recovery is blocked. */
   reset_faults();
   rename_fail_mask = 1u;
   assert(fe_host_state_save(path) < 0);
   assert(saved_state_intact(old) && access(path, F_OK) != 0);
   assert(access(tmp, F_OK) != 0);
   reset_faults();
   assert(fe_host_state_load(path) == 0);
   assert(rename_calls == 1 && saved_state_intact(path));
   assert(access(old, F_OK) != 0);
   puts("failed rollback: backup survives failed recovery and later load restores it");

   /* Existing current slot wins over a stale .old; a successful next save
    * removes both the stale backup and the new transaction's scratch. */
   seed(old);
   reset_faults();
   assert(fe_host_state_load(path) == 0);
   assert(rename_calls == 0 && original_intact(old));
   assert(fe_host_state_save(path) == 0);
   assert(saved_state_intact(path));
   assert(access(old, F_OK) != 0 && access(tmp, F_OK) != 0);
   puts("current plus stale backup: current kept, next save cleans scratch");

   /* A denied removal of the stale backup must stop before moving current. */
   seed(path);
   seed(old);
   reset_faults();
   remove_old_error = 1;
   assert(fe_host_state_save(path) < 0);
   assert(rename_calls == 0 && original_intact(path) && original_intact(old));
   assert(access(tmp, F_OK) != 0);
   reset_faults();
   assert(remove(old) == 0);
   puts("backup cleanup denied: current and backup both kept");

   /* A false success is detected by checking both names; false failure
    * after a successful rename does not roll back a correct new state. */
   reset_faults();
   rename_lie_success_mask = 2u;
   assert(fe_host_state_save(path) < 0);
   assert(rename_calls == 3 && original_intact(path));
   assert(access(old, F_OK) != 0 && access(tmp, F_OK) != 0);
   reset_faults();
   rename_lie_failure_mask = 1u | 2u;
   assert(fe_host_state_save(path) == 0);
   assert(rename_calls == 2 && saved_state_intact(path));
   assert(access(old, F_OK) != 0 && access(tmp, F_OK) != 0);
   puts("misleading rename return values: filesystem effects determine result");

   /* Failure while creating a previously empty slot leaves it empty. */
   assert(remove(path) == 0);
   reset_faults();
   rename_fail_mask = 1u;
   assert(fe_host_state_save(path) < 0);
   assert(access(path, F_OK) != 0 && access(old, F_OK) != 0 && access(tmp, F_OK) != 0);
   reset_faults();
   seed(tmp); /* a stale partial temp is replaced, never loaded */
   assert(fe_host_state_save(path) == 0 && saved_state_intact(path));
   assert(access(tmp, F_OK) != 0 && access(old, F_OK) != 0);
   assert(live_allocations == 0);
   puts("empty slot and stale temp: failed create stays empty, next save succeeds");
}
int main(int argc, char **argv)
{
   char dir[] = "/tmp/gbadhoc-review-persist-XXXXXX", path[256], missing[256];
   int rc, intact, baseline = argc > 1 && !strcmp(argv[1], "--baseline");
   uint8_t image[32768];
   FILE *f;
   assert(mkdtemp(dir));
   snprintf(path, sizeof(path), "%s/game.st0", dir);
   snprintf(missing, sizeof(missing), "%s/missing/game.sav", dir);

   seed(path);
   short_write = 1;
   rc = fe_host_state_save(path);
   short_write = 0;
   intact = original_intact(path);
   printf("short state write: result=%d previous_slot_intact=%d\n", rc, intact);
   assert(rc < 0);
   assert(intact == !baseline);

   seed(path);
   close_error = 1;
   rc = fe_host_state_save(path);
   close_error = 0;
   printf("late state close error: result=%d previous_slot_intact=%d\n", rc,
          original_intact(path));
   assert(baseline ? rc == 0 : rc < 0);
   if (!baseline) assert(original_intact(path));

   host.save_path = missing;
   memset(image, 0x52, sizeof(image));
   rc = gbl_commit(NULL, image, sizeof(image));
   printf("GB link save open failure: result=%d outstanding_allocations=%u\n",
          rc, live_allocations);
   assert(rc < 0);
   assert(live_allocations == (unsigned)baseline);

   assert(fe_host_state_save(path) == 0);
   f = fopen(path, "ab"); assert(f);
   assert(fputc(0xAB, f) != EOF); assert(fclose(f) == 0);
   rc = fe_host_state_load(path);
   printf("state with trailing byte: result=%d\n", rc);
   assert(baseline ? rc == 0 : rc < 0);
   if (!baseline)
      test_state_replacement(path);
   unlink(path);
   rmdir(dir);
   puts(baseline ? "CONFIRMED v4.0.0 persistence failures" : "PASS persistence regressions");
   return 0;
}
