#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#include "menu_load.h"
#include "menu_files_private.h"
#include "gamedb.h"
#include "plat.h"
#include "../psp/state_slots.h"
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#ifdef __vita__
#include <psp2/io/stat.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/rtc.h>
#else
#include <pthread.h>
#endif

#define MF_ENCODED_MAX (8u * 1024u * 1024u)
#define MF_COVER_PIXELS (1600u * 1600u)
#define MF_CHUNK 65536u
#define MF_GET(x) __atomic_load_n(&(x), __ATOMIC_RELAXED)
#define MF_SET(x, v) __atomic_store_n(&(x), (v), __ATOMIC_RELAXED)
#define MF_ADD(x, v) __atomic_add_fetch(&(x), (v), __ATOMIC_RELAXED)
static menu_files_diag g_diag;
/* One dedicated menu worker owns the decoder and its accounting. */
static int g_busy;
static size_t g_used, g_output_bytes, g_limit = MENU_FILES_BUDGET;
static int (*g_cancel)(void);
#ifdef MENU_FILES_TEST
static int (*g_hook)(unsigned, const char *);
static size_t g_test_limit;
void menu_files_test_hook(int (*hook)(unsigned, const char *)) { g_hook = hook; }
void menu_files_test_limit(size_t bytes) { g_test_limit = bytes; }
#endif

static int mf_cancelled(void) { return g_cancel && g_cancel(); }
static uint32_t mf_elapsed(uint64_t t)
{
   uint64_t n = plat_time_us() - t;
   return n > UINT32_MAX ? UINT32_MAX : (uint32_t)n;
}
static void mf_max(uint32_t *p, uint32_t n)
{
   uint32_t old = MF_GET(*p);
   while (old < n && !__atomic_compare_exchange_n(p, &old, n, 0,
                                 __ATOMIC_RELAXED, __ATOMIC_RELAXED)) {}
}
static void mf_memory(void)
{
   MF_SET(g_diag.live_bytes, (uint32_t)g_used);
   mf_max(&g_diag.peak_bytes, (uint32_t)g_used);
}
typedef union mf_header
{
   size_t bytes;
   long double alignment;
   void *pointer;
} mf_header;

static void *mf_alloc(size_t n)
{
   mf_header *p;
   if (!n || mf_cancelled() || n > g_limit ||
       sizeof(*p) > g_limit - n || g_used > g_limit - n - sizeof(*p))
      return NULL;
   p = (mf_header *)malloc(n + sizeof(*p));
   if (!p) return NULL;
   p->bytes = n + sizeof(*p);
   g_used += p->bytes;
   mf_memory();
   return p + 1;
}
static void mf_free(void *ptr)
{
   if (ptr)
   {
      mf_header *p = (mf_header *)ptr - 1;
      g_used -= p->bytes;
      free(p);
      mf_memory();
   }
}
static void *mf_realloc(void *ptr, size_t n)
{
   void *p;
   size_t old;
   if (!ptr) return mf_alloc(n);
   if (!n) { mf_free(ptr); return NULL; }
   old = ((mf_header *)ptr - 1)->bytes - sizeof(mf_header);
   /* Account both old and new blocks during growth, including the copy. */
   p = mf_alloc(n);
   if (!p) return NULL;
   memcpy(p, ptr, old < n ? old : n);
   mf_free(ptr);
   return p;
}

/* Private decoder symbols and all its heap allocations stay in this budget.
 * No stdio path can bypass the I/O adapters or introduce a cache writer. */
#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#define STBI_ONLY_BMP
#define STBI_NO_STDIO
#define STBI_NO_HDR
#define STBI_NO_LINEAR
#define STBI_MALLOC(n) mf_alloc(n)
#define STBI_REALLOC(p, n) mf_realloc(p, n)
#define STBI_FREE(p) mf_free(p)
#include "../psp/stb_image.h"

void menu_files_get_diag(menu_files_diag *out)
{
   if (!out) return;
#define COPY(n) out->n = MF_GET(g_diag.n)
   COPY(calls); COPY(stat_calls); COPY(fstat_calls); COPY(open_calls);
   COPY(read_calls); COPY(seek_calls); COPY(close_calls); COPY(bytes);
   COPY(stat_us); COPY(read_us); COPY(meta_us); COPY(decode_us);
   COPY(resample_us); COPY(max_io_us); COPY(peak_bytes); COPY(live_bytes);
   COPY(thread_tag);
#undef COPY
}
static int mf_before(unsigned op, const char *path)
{
#ifdef MENU_FILES_TEST
   if (g_hook)
   {
      int e = g_hook(op, path);
      if (e) { errno = e; return -1; }
   }
#else
   (void)op; (void)path;
#endif
   return 0;
}
static void mf_io_done(uint64_t t, int stat_op)
{
   uint32_t n = mf_elapsed(t);
   if (stat_op) MF_ADD(g_diag.stat_us, n);
   else MF_ADD(g_diag.read_us, n);
   mf_max(&g_diag.max_io_us, n);
}
static int mf_open(const char *path)
{
   uint64_t t = plat_time_us();
   int fd;
   MF_ADD(g_diag.open_calls, 1);
   fd = mf_before(MF_OPEN, path) < 0 ? -1 : open(path, O_RDONLY | O_NONBLOCK);
   mf_io_done(t, 0);
   return fd;
}
static void mf_close(int fd, const char *path)
{
   uint64_t t = plat_time_us();
   MF_ADD(g_diag.close_calls, 1);
   /* Even a cancellation or injected close error must release the fd. */
   mf_before(MF_CLOSE, path);
   close(fd);
   mf_io_done(t, 0);
}
static int mf_fstat(int fd, const char *path, struct stat *st)
{
   uint64_t t = plat_time_us();
   int rc;
   MF_ADD(g_diag.fstat_calls, 1);
   rc = mf_before(MF_FSTAT, path) < 0 ? -1 : fstat(fd, st);
   mf_io_done(t, 1);
   return rc;
}
static int mf_seek(int fd, const char *path, off_t pos)
{
   uint64_t t = plat_time_us();
   int rc;
   MF_ADD(g_diag.seek_calls, 1);
   rc = mf_before(MF_SEEK, path) < 0 ? -1 :
                  (lseek(fd, pos, SEEK_SET) == pos ? 0 : -1);
   mf_io_done(t, 0);
   return rc;
}
static int mf_read(int fd, const char *path, void *buffer, size_t len)
{
   unsigned char *p = (unsigned char *)buffer;
   while (len)
   {
      uint64_t t;
      ssize_t n;
      size_t chunk = len > MF_CHUNK ? MF_CHUNK : len;
      if (mf_cancelled()) return ML_CANCELLED;
      t = plat_time_us();
      MF_ADD(g_diag.read_calls, 1);
      n = mf_before(MF_READ, path) < 0 ? -1 : read(fd, p, chunk);
      mf_io_done(t, 0);
      if (n > 0) MF_ADD(g_diag.bytes, (uint32_t)n);
      if (mf_cancelled()) return ML_CANCELLED;
      if (n < 0 && errno == EINTR) continue;
      if (n <= 0) return ML_ERROR;
      p += n; len -= (size_t)n;
   }
   return ML_OK;
}
static int mf_missing(void) { return errno == ENOENT ? ML_ABSENT : ML_ERROR; }

static void mf_stamp(char out[24], int year, int mon, int day, int hr, int min)
{
   static const char *months[] = { "Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                  "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };
   out[0] = '\0';
   if (year < 1 || year > 9999 || mon < 1 || mon > 12 || day < 1 ||
       day > 31 || hr < 0 || hr > 23 || min < 0 || min > 59) return;
   snprintf(out, 24, "%d %s %d, %02d:%02d", day, months[mon - 1], year, hr, min);
}
static int mf_stat(const char *path, char *stamp)
{
   int rc, answer;
   uint64_t t;
   if (mf_cancelled()) return ML_CANCELLED;
   t = plat_time_us();
   MF_ADD(g_diag.stat_calls, 1);
   if (mf_before(MF_STAT, path) < 0) answer = mf_missing();
   else
   {
#ifdef __vita__
      SceIoStat st;
      rc = sceIoGetstat(path, &st);
      if (rc < 0)
         answer = (uint32_t)rc == 0x80010002u ? ML_ABSENT : ML_ERROR;
      else if (!SCE_S_ISREG(st.st_mode)) answer = ML_ERROR;
      else
      {
         answer = ML_OK;
         if (stamp)
         {
            /* sceIo stamps are UTC; save times are shown in the console's
             * own clock, exactly as pspcompat's stat_out (stamp_local) does
             * for every other page.  If the RTC cannot convert, the stamp is
             * kept as it is, as there. */
            SceDateTime d = st.st_mtime;
            SceRtcTick utc, local;
            if (sceRtcGetTick(&st.st_mtime, &utc) >= 0 &&
                sceRtcConvertUtcToLocalTime(&utc, &local) >= 0)
               sceRtcSetTick(&d, &local);
            mf_stamp(stamp, d.year, d.month, d.day, d.hour, d.minute);
         }
      }
#else
      struct stat st;
      rc = stat(path, &st);
      if (rc < 0) answer = mf_missing();
      else if (!S_ISREG(st.st_mode)) answer = ML_ERROR;
      else
      {
         answer = ML_OK;
         if (stamp)
         {
            struct tm tm;
            if (localtime_r(&st.st_mtime, &tm))
               mf_stamp(stamp, tm.tm_year + 1900, tm.tm_mon + 1,
                                tm.tm_mday, tm.tm_hour, tm.tm_min);
         }
      }
#endif
   }
   mf_io_done(t, 1);
   return mf_cancelled() ? ML_CANCELLED : answer;
}

static int mf_whole(const char *path, unsigned char **out, size_t *len)
{
   int fd, rc;
   struct stat st;
   unsigned char *p = NULL;
   *out = NULL; *len = 0;
   if (mf_cancelled()) return ML_CANCELLED;
   fd = mf_open(path);
   if (fd < 0) return mf_cancelled() ? ML_CANCELLED : mf_missing();
   if (mf_cancelled()) rc = ML_CANCELLED;
   else if (mf_fstat(fd, path, &st) < 0)
      rc = ML_ERROR;
   else if (!S_ISREG(st.st_mode) || st.st_size <= 0 ||
            (uint64_t)st.st_size > MF_ENCODED_MAX)
      rc = ML_INVALID;       /* never decodable as it stands */
   else if (mf_cancelled()) rc = ML_CANCELLED;
   else if (!(p = (unsigned char *)mf_alloc((size_t)st.st_size)))
      rc = mf_cancelled() ? ML_CANCELLED : ML_ERROR;
   else
   {
      *len = (size_t)st.st_size;
      rc = mf_read(fd, path, p, *len);
   }
   mf_close(fd, path);
   if (rc == ML_OK) *out = p;
   else { mf_free(p); *len = 0; }
   return rc;
}

static unsigned mf_u16(const unsigned char *p) { return p[0] | (unsigned)p[1] << 8; }
static uint32_t mf_u32(const unsigned char *p)
{
   return p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
static uint32_t mf_rgba565(unsigned v)
{
   unsigned r = v & 31, g = (v >> 5) & 63, b = (v >> 11) & 31;
   return 0xff000000u | (((b << 3) | (b >> 2)) << 16) |
            (((g << 2) | (g >> 4)) << 8) | ((r << 3) | (r >> 2));
}
static int mf_raw(const unsigned char *file, size_t len, int shot,
                    uint32_t **pixels, unsigned *w, unsigned *h)
{
   unsigned stride, x, y;
   uint32_t *p;
   if (len < 16) return ML_INVALID;
   *w = mf_u16(file + 4); *h = mf_u16(file + 6); stride = mf_u16(file + 8);
   if (shot)
   {
      if (psp_state_shot_check(file, w, h)) return ML_INVALID;
   }
   else if (mf_u32(file) != 0x52353635u || !*w || !*h ||
            *w > ML_IMAGE_MAX_W || *h > ML_IMAGE_MAX_H || stride < *w)
      return ML_INVALID;
   if (len < 16u + (size_t)stride * *h * 2u) return ML_INVALID;
   p = (uint32_t *)mf_alloc((size_t)*w * *h * 4u);
   if (!p) return mf_cancelled() ? ML_CANCELLED : ML_ERROR;
   for (y = 0; y < *h; y++)
   {
      if (mf_cancelled()) { mf_free(p); return ML_CANCELLED; }
      for (x = 0; x < *w; x++)
         p[y * *w + x] = mf_rgba565(mf_u16(file + 16 + ((size_t)y * stride + x) * 2));
   }
   *pixels = p;
   return ML_OK;
}
static int mf_decode(const unsigned char *file, size_t len, int title,
                       uint32_t **pixels, unsigned *ow, unsigned *oh)
{
   int w = 0, h = 0, channels, x, y;
   unsigned char *src;
   uint32_t *p;
   uint64_t t = plat_time_us();
   if (!stbi_info_from_memory(file, (int)len, &w, &h, &channels) || w <= 0 ||
       h <= 0 || (title ? (w > 2048 || h > 2048) :
                           ((unsigned)w > MF_COVER_PIXELS / (unsigned)h)))
      return ML_INVALID;
   /* Preserve the supported legacy BMP subset (24/32-bit, uncompressed). */
   if (len >= 2 && file[0] == 'B' && file[1] == 'M' &&
       (len < 54 || (mf_u16(file + 28) != 24 && mf_u16(file + 28) != 32) ||
        mf_u32(file + 30) != 0)) return ML_INVALID;
   if (mf_cancelled()) return ML_CANCELLED;
   src = stbi_load_from_memory(file, (int)len, &w, &h, &channels, 4);
   MF_ADD(g_diag.decode_us, mf_elapsed(t));
   if (!src)
   {
      /* This private decoder's failure reason is thread-local and only this
       * worker uses it: "outofmem" is the budget (mf_alloc), which may pass;
       * anything else is the file's content. */
      const char *why = stbi_failure_reason();
      if (mf_cancelled()) return ML_CANCELLED;
      return why && !strcmp(why, "outofmem") ? ML_ERROR : ML_INVALID;
   }
   if (w <= 0 || h <= 0 || (title ? (w > 2048 || h > 2048) :
                  ((unsigned)w > MF_COVER_PIXELS / (unsigned)h)))
   { stbi_image_free(src); return ML_INVALID; }
   p = (uint32_t *)mf_alloc((size_t)w * h * 4u);
   if (!p) { stbi_image_free(src); return mf_cancelled() ? ML_CANCELLED : ML_ERROR; }
   for (y = 0; y < h; y++)
   {
      if (mf_cancelled()) { mf_free(p); stbi_image_free(src); return ML_CANCELLED; }
      for (x = 0; x < w; x++)
      {
         const unsigned char *s = src + ((size_t)y * w + x) * 4;
         p[(size_t)y * w + x] = 0xff000000u | (uint32_t)s[2] << 16 |
                                 (uint32_t)s[1] << 8 | s[0];
      }
   }
   stbi_image_free(src);
   *pixels = p; *ow = (unsigned)w; *oh = (unsigned)h;
   return ML_OK;
}

/* These are Home's hm_square/hm_round pixel rules, without UI state. */
static int mf_square(const uint32_t *src, int sw, int sh, uint32_t *dst)
{
   int edge = ML_TILE_EDGE, s = sw < sh ? sw : sh;
   int x0 = (sw - s) / 2, y0 = (sh - s) / 2, x, y;
   for (y = 0; y < edge; y++)
   {
      if (mf_cancelled()) return ML_CANCELLED;
      for (x = 0; x < edge; x++)
         if (s >= edge * 2)
         {
            int ax = x0 + x * s / edge, bx = x0 + (x + 1) * s / edge;
            int ay = y0 + y * s / edge, by = y0 + (y + 1) * s / edge, u, v;
            unsigned r = 0, g = 0, b = 0, n = 0;
            for (v = ay; v < by; v++) for (u = ax; u < bx; u++)
            {
               uint32_t c = src[v * sw + u];
               r += c & 255; g += (c >> 8) & 255; b += (c >> 16) & 255; n++;
            }
            if (!n) n = 1;
            dst[y * edge + x] = 0xff000000u | (b / n) << 16 | (g / n) << 8 | r / n;
         }
         else dst[y * edge + x] = src[(y0 + y * s / edge) * sw + x0 + x * s / edge];
   }
   return ML_OK;
}
static void mf_round(uint32_t *px)
{
   int edge = ML_TILE_EDGE, r = 20, x, y, c;
   for (c = 0; c < 4; c++) for (y = 0; y < r; y++) for (x = 0; x < r; x++)
   {
      int sx, sy, hits = 0;
      for (sy = 0; sy < 4; sy++) for (sx = 0; sx < 4; sx++)
      {
         float dx = (float)r - ((float)x + ((float)sx + 0.5f) / 4.0f);
         float dy = (float)r - ((float)y + ((float)sy + 0.5f) / 4.0f);
         if (dx * dx + dy * dy <= (float)(r * r)) hits++;
      }
      {
         int xx = (c & 1) ? edge - 1 - x : x, yy = (c & 2) ? edge - 1 - y : y;
         uint32_t *p = px + yy * edge + xx;
         *p = (*p & 0xffffffu) | (((*p >> 24) * (unsigned)hits / 16u) << 24);
      }
   }
}
static int mf_output(const uint32_t *src, unsigned w, unsigned h,
                        int tile, int title, menu_load_result *out)
{
   unsigned ow = w, oh = h, x, y;
   uint32_t *p;
   uint64_t t = plat_time_us();
   size_t bytes;
   if (tile) ow = oh = ML_TILE_EDGE;
   else
   {
      unsigned bw = title ? 240 : ML_IMAGE_MAX_W, bh = title ? 160 : ML_IMAGE_MAX_H;
      if (ow > bw || oh > bh)
      {
         if ((uint64_t)w * bh > (uint64_t)h * bw)
         { ow = bw; oh = (unsigned)((uint64_t)h * bw / w); }
         else { oh = bh; ow = (unsigned)((uint64_t)w * bh / h); }
         if (!ow) ow = 1;
         if (!oh) oh = 1;
      }
   }
   bytes = (size_t)ow * oh * 4;
   if (mf_cancelled()) return ML_CANCELLED;
   if (bytes > g_limit || g_used > g_limit - bytes || !(p = (uint32_t *)malloc(bytes)))
      return ML_ERROR;
   g_used += bytes; g_output_bytes = bytes; mf_memory();
   if (tile)
   {
      if (mf_square(src, (int)w, (int)h, p) == ML_CANCELLED)
      { free(p); g_used -= bytes; g_output_bytes = 0; mf_memory(); return ML_CANCELLED; }
      mf_round(p);
   }
   else for (y = 0; y < oh; y++)
   {
      if (mf_cancelled())
      { free(p); g_used -= bytes; g_output_bytes = 0; mf_memory(); return ML_CANCELLED; }
      for (x = 0; x < ow; x++)
         p[(size_t)y * ow + x] = src[(size_t)(y * h / oh) * w + x * w / ow];
   }
   MF_ADD(g_diag.resample_us, mf_elapsed(t));
   out->pixels = p; out->w = ow; out->h = oh;
   return ML_OK;
}
static int mf_image(const char *path, int raw, int shot, int tile,
                       int title, menu_load_result *out)
{
   unsigned char *file;
   size_t len;
   uint32_t *pixels = NULL;
   unsigned w = 0, h = 0;
   int rc = mf_whole(path, &file, &len);
   if (rc != ML_OK) return rc;
   rc = raw || shot ? mf_raw(file, len, shot, &pixels, &w, &h) :
                     mf_decode(file, len, title, &pixels, &w, &h);
   mf_free(file);
   if (rc == ML_OK) rc = mf_output(pixels, w, h, tile, title, out);
   mf_free(pixels);
   return rc;
}
static int mf_art(const menu_load_job *job, unsigned kind, menu_load_result *out)
{
   static const char *ext[] = { "v565", "png", "jpg", "jpeg", "bmp", "565", "vh565" };
   char path[ML_PATH];
   /* failure: a candidate may load later (I/O, memory); invalid: one can
    * never load.  A missing picture is reported with the worse of them. */
   int tile = kind == ML_TILE, failure = 0, invalid = 0, rc, n;
   unsigned i;
   if (kind >= ML_STATE0)
   {
      unsigned slot = 1;
      for (i = ML_STATE0; i != kind && slot <= 5; i <<= 1) slot++;
      if (slot > 5 || psp_state_path_for_slot(path, sizeof(path), job->state1, slot) ||
          strlen(path) + 5 >= sizeof(path)) return ML_ERROR;
      strcat(path, ".shot");
      return mf_image(path, 0, 1, 0, 0, out);
   }
   if (tile)
   {
      n = snprintf(path, sizeof(path), "%s/boxart/%s.t2.565", job->root, job->stem);
      if (n < 0 || (size_t)n >= sizeof(path)) return ML_ERROR;
      rc = mf_image(path, 1, 0, 1, 0, out);
      if (rc == ML_OK || rc == ML_CANCELLED) return rc;
      if (rc == ML_ERROR) failure = 1;
      if (rc == ML_INVALID) invalid = 1;
   }
   for (i = 0; i < (tile ? 7u : 6u); i++)
   {
      n = snprintf(path, sizeof(path), "%s/%s/%s.%s", job->root,
                                  tile ? "boxart" : "hero", job->stem, ext[i]);
      if (n < 0 || (size_t)n >= sizeof(path)) return ML_ERROR;
      rc = mf_image(path, i == 0 || i >= 5, 0, tile, 0, out);
      if (rc == ML_OK && !tile) out->source = ML_SOURCE_HERO;
      if (rc == ML_OK || rc == ML_CANCELLED) return rc;
      if (rc == ML_ERROR) failure = 1;
      if (rc == ML_INVALID) invalid = 1;
   }
   n = snprintf(path, sizeof(path), "%s/scenes/%s.shot", job->root, job->stem);
   if (n < 0 || (size_t)n >= sizeof(path)) return ML_ERROR;
   rc = mf_image(path, 0, 1, tile, 0, out);
   if (rc == ML_OK || rc == ML_CANCELLED) return rc;
   if (rc == ML_ERROR) failure = 1;
   if (rc == ML_INVALID) invalid = 1;
   n = snprintf(path, sizeof(path), "%s/titles/%s.png", job->root, job->stem);
   if (n < 0 || (size_t)n >= sizeof(path)) return ML_ERROR;
   rc = mf_image(path, 0, 0, tile, 1, out);
   if (rc == ML_OK || rc == ML_CANCELLED || rc == ML_ERROR) return rc;
   /* Transient failures win, so the game is retried soon. */
   if (failure) return ML_ERROR;
   return invalid ? ML_INVALID : rc;
}
static int mf_header_blurb(const menu_load_job *job, char out[400])
{
   int fd, rc = ML_OK, i;
   struct stat st;
   char code[4], key[8], size[24], name[256];
   if (job->console != FE_CONSOLE_GBA) return ML_OK;
   if (mf_cancelled()) return ML_CANCELLED;
   fd = mf_open(job->rom);
   if (fd < 0) return mf_cancelled() ? ML_CANCELLED :
                          (errno == ENOENT ? ML_OK : ML_ERROR);
   if (mf_cancelled()) rc = ML_CANCELLED;
   else if (mf_fstat(fd, job->rom, &st) < 0 || !S_ISREG(st.st_mode)) rc = ML_ERROR;
   else if (mf_cancelled()) rc = ML_CANCELLED;
   else if (st.st_size >= 0xb0)
   {
      if (mf_seek(fd, job->rom, 0xac) < 0) rc = ML_ERROR;
      else rc = mf_read(fd, job->rom, code, sizeof(code));
   }
   else { mf_close(fd, job->rom); return ML_OK; }
   mf_close(fd, job->rom);
   if (rc != ML_OK) return rc;
   for (i = 0; i < 4; i++)
      if (!((code[i] >= 'A' && code[i] <= 'Z') || (code[i] >= '0' && code[i] <= '9')))
         return ML_OK;
   if (mf_cancelled()) return ML_CANCELLED;
   snprintf(key, sizeof(key), "Z%.4s", code);
   if (gamedb_get(key, size, sizeof(size)) &&
       (uint64_t)st.st_size * 2 >= strtoul(size, NULL, 10) &&
       gamedb_name_by_code(code, name, sizeof(name)))
      gamedb_blurb(name, out, 400);
   return mf_cancelled() ? ML_CANCELLED : ML_OK;
}
static int mf_metadata(const menu_load_job *job, menu_load_meta *out)
{
   char path[ML_PATH + 4];
   int i, rc, answer = ML_OK, n;
   uint64_t t = plat_time_us();
   out->battery = ML_UNKNOWN;
   for (i = 0; i < 5; i++) out->state[i] = ML_UNKNOWN;
   for (i = 0; i < 5; i++)
   {
      if (psp_state_path_for_slot(path, sizeof(path), job->state1, (unsigned)i + 1))
      { answer = ML_ERROR; continue; }
      rc = mf_stat(path, out->when[i]);
      if (rc == ML_CANCELLED) return rc;
      if (rc == ML_OK) out->state[i] = ML_PRESENT;
      else if (rc == ML_ABSENT)
      {
         strcat(path, ".old");
         rc = mf_stat(path, out->when[i]);
         if (rc == ML_CANCELLED) return rc;
         if (rc == ML_OK) out->state[i] = ML_OLD_ONLY;
         else if (rc == ML_ABSENT) out->state[i] = ML_MISSING;
         else answer = ML_ERROR;
      }
      else answer = ML_ERROR;
   }
   if (job->console == FE_CONSOLE_GBA)
      n = psp_rom_path_suffix(path, sizeof(path), job->rom, ".sav");
   else
      n = snprintf(path, sizeof(path), "%s.sav", job->rom) >= (int)sizeof(path) ? -1 : 0;
   if (n < 0) answer = ML_ERROR;
   else
   {
      rc = mf_stat(path, NULL);
      if (rc == ML_CANCELLED) return rc;
      if (rc == ML_OK) out->battery = ML_PRESENT;
      else if (rc == ML_ABSENT) out->battery = ML_MISSING;
      else answer = ML_ERROR;
   }
   /* Where the game was left (state_slots.h), and a patch beside the ROM
    * (rom_patch.h: the first of .ips, .bps, .ups that is there). */
   out->resume = ML_UNKNOWN;
   if (psp_state_resume_path(path, sizeof(path), job->state1) == 0)
   {
      rc = mf_stat(path, out->resume_when);
      if (rc == ML_CANCELLED) return rc;
      out->resume = rc == ML_OK ? ML_PRESENT : rc == ML_ABSENT ? ML_MISSING
                                                              : ML_UNKNOWN;
   }
   out->patch = 0;
   for (i = 0; i < 3 && !out->patch; i++)
   {
      static const char *const ext[3] = { ".ips", ".bps", ".ups" };
      if (psp_rom_path_suffix(path, sizeof(path), job->rom, ext[i]) != 0)
         continue;
      rc = mf_stat(path, NULL);
      if (rc == ML_CANCELLED) return rc;
      if (rc == ML_OK)
         out->patch = i + 1;
   }
   if (mf_cancelled()) return ML_CANCELLED;
   if (!gamedb_blurb(job->stem, out->blurb, sizeof(out->blurb)))
   {
      rc = mf_header_blurb(job, out->blurb);
      if (rc == ML_CANCELLED) return rc;
      if (rc == ML_ERROR) answer = rc;
   }
   MF_ADD(g_diag.meta_us, mf_elapsed(t));
   return answer;
}

int menu_files_load(const menu_load_job *job, unsigned kind,
                     menu_load_result *out, int (*cancelled)(void))
{
   int zero = 0, rc;
   if (!out) return ML_ERROR;
   out->pixels = NULL; out->w = out->h = 0; out->source = ML_SOURCE_NORMAL;
   if (!job || !memchr(job->root, 0, sizeof(job->root)) ||
       !memchr(job->rom, 0, sizeof(job->rom)) ||
       !memchr(job->stem, 0, sizeof(job->stem)) ||
       !memchr(job->state1, 0, sizeof(job->state1)) ||
       !kind || (kind & (kind - 1)) || kind > (ML_STATE0 << 4) ||
       !__atomic_compare_exchange_n(&g_busy, &zero, 1, 0, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED))
      return out->status = ML_ERROR;
   out->job = *job; out->kind = kind;
   memset(&out->meta, 0, sizeof(out->meta));
   g_cancel = cancelled; g_used = g_output_bytes = 0;
   g_limit = MENU_FILES_BUDGET;
#ifdef MENU_FILES_TEST
   if (g_test_limit && g_test_limit < g_limit) g_limit = g_test_limit;
#endif
   MF_ADD(g_diag.calls, 1);
#ifdef __vita__
   MF_SET(g_diag.thread_tag, (uint32_t)sceKernelGetThreadId());
#else
   MF_SET(g_diag.thread_tag, (uint32_t)(uintptr_t)pthread_self());
#endif
   rc = mf_cancelled() ? ML_CANCELLED : kind == ML_META ?
                mf_metadata(job, &out->meta) : mf_art(job, kind, out);
   if (mf_cancelled()) rc = ML_CANCELLED;
   if (rc != ML_OK && out->pixels)
   { free(out->pixels); out->pixels = NULL; out->w = out->h = 0; }
   /* Output is now worker-owned malloc storage, never a decoder allocation. */
   g_used -= g_output_bytes; g_output_bytes = 0; mf_memory();
   out->status = rc;
   g_cancel = NULL;
   __atomic_store_n(&g_busy, 0, __ATOMIC_RELEASE);
   return rc;
}
