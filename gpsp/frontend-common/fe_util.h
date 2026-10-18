/* fe_util.h — small shared helpers: crc32, sha1, BMP dump, ini reader. */
#ifndef FE_UTIL_H
#define FE_UTIL_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* CRC-32 (IEEE, zlib-compatible). Used for SRAM dirty detection + EVT lines. */
uint32_t fe_crc32(uint32_t crc, const void *data, size_t len);

/* SHA-1: out_hex must hold 41 bytes (40 hex chars + NUL). */
void fe_sha1_hex(const void *data, size_t len, char *out_hex);

/* The same hash, fed in pieces (a ROM streamed from a file or received in
 * chunks).  fe_sha1_final writes the 20-byte digest. */
typedef struct fe_sha1_ctx
{
   uint32_t h[5];
   uint64_t len;
   uint8_t  buf[64];
   size_t   used;
} fe_sha1_ctx;
void fe_sha1_init(fe_sha1_ctx *c);
void fe_sha1_update(fe_sha1_ctx *c, const void *data, size_t len);
void fe_sha1_final(fe_sha1_ctx *c, uint8_t out[20]);

/* Write a 240x160-ish RGB565 frame as a bottom-up 24bpp BMP.
 * pitch is in BYTES. Returns 0 on success. */
int fe_bmp_write_rgb565(const char *path, const uint16_t *pix,
                        unsigned w, unsigned h, size_t pitch);

/* Same, but the source is PSP-native GE 5650 (R in bits 0-4, G 5-10,
 * B 11-15 — the reverse of libretro RGB565). Used by the PSP frontend's
 * GE drawbuffer readback (GU color-order regression check). */
int fe_bmp_write_psp565(const char *path, const uint16_t *pix,
                        unsigned w, unsigned h, size_t pitch);

/* Minimal ini/key=value reader ('#' and ';' comments, sections ignored).
 * Copies the value for `key` into out (trimmed). Returns 1 if found. */
int fe_ini_get(const char *path, const char *key, char *out, size_t out_sz);
/* Integer convenience: returns def if missing/unparsable. */
long fe_ini_get_int(const char *path, const char *key, long def);
/* How many fe_ini_get calls have run, and how many found no file.  EVERY
 * lookup opens the file (no cache), so at boot this is a count of Memory
 * Stick opens -- the boot timeline (main_psp.c boot_mark) reports it. */
void fe_ini_stats(unsigned *lookups, unsigned *misses);
/* A path that cannot exist for the life of the process: every fe_ini_get /
 * fe_ini_get_int of it returns "not found" without touching the Memory
 * Stick (counted in neither stat).  One path; NULL clears it.  The release
 * build registers its ADR-0067 impossible harness path, so the ~130 harness
 * keys main() asks for at boot stop costing a negative lookup each. */
void fe_ini_never(const char *path);

/* Lookup audit.  Register a path, and every fe_ini_get/fe_ini_get_int of it
 * is recorded; fe_ini_audit_report() then calls emit() once per key=value
 * line in the file with `asked` = whether anything looked that key up (2 =
 * a DUPLICATE line, ignored because the first one wins; 0 is a
 * typo or a key this build does not have), the integer parse and whether the
 * whole value parsed as one.  Returns lines reported, -1 if not registered. */
void fe_ini_audit(const char *path);
int  fe_ini_audit_report(const char *path,
                         void (*emit)(void *user, const char *key,
                                      const char *raw, int asked, long ival,
                                      int is_int),
                         void *user);

/* Replace (or append) `key = value` in a small ini file, preserving all
 * other lines. Creates the file if missing. Returns 0 on success. */
int fe_ini_set(const char *path, const char *key, const char *value);
int fe_ini_set_int(const char *path, const char *key, long value);

/* ---- an INI file held in memory: read once, written once ------------------
 *
 * fe_ini_get opens, scans and closes the file for every key, and fe_ini_set
 * reads and rewrites the whole file for every key: CONFIG.INI was opened 74
 * times at boot and rewritten 43 times by one save.  A document is the same
 * reader and writer over a buffer -- byte for byte the same answers and the
 * same output (fe_util.c) -- loaded with one read and saved with one
 * atomic write.  NO HEAP: the caller owns the struct (on its stack).
 *
 *   load   0 = loaded, or the file is absent (an empty document, which a
 *          save creates); -1 = larger than FE_INI_DOC_CAP - 1 bytes or a read
 *          error -- the document is unusable and must not be saved (nothing
 *          is ever truncated to fit).  A missing file first gets
 *          fe_ini_recover's look at a save a power cut interrupted.
 *   set    -1 when the result would not fit; the document is unchanged.
 *   save   atomic (path.tmp, then renames; see fe_ini_doc_save); writes
 *          nothing when no set changed a byte.  -1 on a write failure or
 *          after a refused set (the rest is still written).
 *   bind   while a document is bound to `path`, fe_ini_get / fe_ini_get_int
 *          / fe_ini_set / fe_ini_set_int of exactly that path use it, so
 *          existing readers (ctl_map, the display profiles) need no change.
 *          NULL unbinds.  One thread, one binding. */
#define FE_INI_DOC_CAP  8192
#define FE_INI_PATH_CAP 256
typedef struct fe_ini_doc
{
   char   buf[FE_INI_DOC_CAP];
   size_t len;
   int    ok;              /* loaded (or absent): usable                  */
   int    existed;         /* the file was there at load                  */
   int    changed;         /* a set changed a byte since load / save      */
   int    overflow;        /* a set was refused                           */
   int    save_fallback;   /* the last save could not rename: in place    */
} fe_ini_doc;
int  fe_ini_doc_load(fe_ini_doc *d, const char *path);
int  fe_ini_doc_get(const fe_ini_doc *d, const char *key, char *out,
                    size_t out_sz);
long fe_ini_doc_get_int(const fe_ini_doc *d, const char *key, long def);
int  fe_ini_doc_set(fe_ini_doc *d, const char *key, const char *value);
int  fe_ini_doc_set_int(fe_ini_doc *d, const char *key, long value);
int  fe_ini_doc_save(fe_ini_doc *d, const char *path);
void fe_ini_doc_bind(fe_ini_doc *d, const char *path);
/* Lookups a bound document answered (no file touched). */
unsigned fe_ini_doc_hits(void);
/* `path` is missing: finish an interrupted save from path.tmp / path.bak.
 * 1 = the new file restored, 2 = the old one, 0 = nothing to do, -1 error. */
int  fe_ini_recover(const char *path);

#ifdef __cplusplus
}
#endif

#endif /* FE_UTIL_H */
