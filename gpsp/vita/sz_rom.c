/* sz_rom.c -- a game kept in a .7z (see sz_rom.h). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sz_rom.h"

#include "7z.h"
#include "7zAlloc.h"
#include "7zCrc.h"

#define SZ_IN_BUF   (1u << 16)
#define SZ_HDR_MAX  (16ul << 20)       /* one allocation while reading the header */
#define SZ_ALLOC_MAX (SZ_ROM_BLOCK_MAX + (1ul << 20))

/* the SDK's allocator with a ceiling: a size a damaged archive makes up is refused, not tried */
typedef struct { ISzAlloc vt; size_t cap; } sz_alloc;

static void *sz_a(ISzAllocPtr p, size_t size)
{
   const sz_alloc *a = (const sz_alloc *)p;
   return size > a->cap ? NULL : malloc(size ? size : 1);
}
static void sz_f(ISzAllocPtr p, void *ptr)
{
   (void)p;
   free(ptr);
}

/* the archive file as the SDK reads it: stdio, as the rest of the app reads files (the SDK's own 7zFile.c is not used) */
typedef struct
{
   ISeekInStream vt;
   FILE         *f;
} sz_file;

static SRes sz_file_read(ISeekInStreamPtr pp, void *buf, size_t *size)
{
   Z7_CONTAINER_FROM_VTBL_TO_DECL_VAR_pp_vt_p(sz_file)
   *size = fread(buf, 1, *size, p->f);
   return ferror(p->f) ? SZ_ERROR_READ : SZ_OK;
}

static SRes sz_file_seek(ISeekInStreamPtr pp, Int64 *pos, ESzSeek origin)
{
   Z7_CONTAINER_FROM_VTBL_TO_DECL_VAR_pp_vt_p(sz_file)
   const int how = origin == SZ_SEEK_SET ? SEEK_SET : origin == SZ_SEEK_CUR ? SEEK_CUR : SEEK_END;
   long at;
   if (*pos > 0x7FFFFFFELL || *pos < -0x7FFFFFFELL || fseek(p->f, (long)*pos, how) != 0 || (at = ftell(p->f)) < 0)
      return SZ_ERROR_READ;
   *pos = at;
   return SZ_OK;
}

typedef struct
{
   sz_alloc      main, temp;
   CSzArEx       db;
   sz_file       fs;
   CLookToRead2  look;
   int           have_db;
} sz_arc;

static void sz_close(sz_arc *a)
{
   if (a->have_db)
      SzArEx_Free(&a->db, &a->main.vt);
   if (a->look.buf)
      free(a->look.buf);
   if (a->fs.f)
      fclose(a->fs.f);
}

static int sz_open(sz_arc *a, const char *path)
{
   SRes r;
   memset(a, 0, sizeof(*a));
   a->main.vt.Alloc = sz_a;  a->main.vt.Free = sz_f;  a->main.cap = SZ_ALLOC_MAX;
   a->temp.vt.Alloc = sz_a;  a->temp.vt.Free = sz_f;  a->temp.cap = SZ_HDR_MAX;
   a->fs.f = fopen(path, "rb");
   if (!a->fs.f)
      return -1;
   a->fs.vt.Read = sz_file_read;
   a->fs.vt.Seek = sz_file_seek;
   LookToRead2_CreateVTable(&a->look, False);
   a->look.buf = (Byte *)malloc(SZ_IN_BUF);
   if (!a->look.buf)
      return -1;
   a->look.bufSize = SZ_IN_BUF;
   LookToRead2_INIT(&a->look)
   a->look.realStream = &a->fs.vt;
   CrcGenerateTable();
   SzArEx_Init(&a->db);
   a->have_db = 1;
   r = SzArEx_Open(&a->db, &a->look.vt, &a->main.vt, &a->temp.vt);
   return r == SZ_OK ? 0 : -1;
}

/* UTF-16 to UTF-8 into out (cap bytes); 0 = it did not fit or holds a NUL name */
static int sz_name(const CSzArEx *db, size_t i, char *out, size_t cap)
{
   size_t n = SzArEx_GetFileNameUtf16(db, i, NULL), k, o = 0;
   UInt16 *w;
   if (n < 2 || n > 1024)
      return 0;
   w = (UInt16 *)malloc(n * sizeof(UInt16));
   if (!w)
      return 0;
   SzArEx_GetFileNameUtf16(db, i, w);
   for (k = 0; k + 1 < n; k++)
   {
      unsigned c = w[k];
      if (c >= 0xD800 && c < 0xDC00 && k + 2 < n && w[k + 1] >= 0xDC00 && w[k + 1] < 0xE000)
         c = 0x10000 + ((c - 0xD800) << 10) + (w[++k] - 0xDC00);
      if (c < 0x80)
      {
         if (o + 1 >= cap) { o = 0; break; }
         out[o++] = (char)c;
      }
      else if (c < 0x800)
      {
         if (o + 2 >= cap) { o = 0; break; }
         out[o++] = (char)(0xC0 | (c >> 6));
         out[o++] = (char)(0x80 | (c & 63));
      }
      else if (c < 0x10000)
      {
         if (o + 3 >= cap) { o = 0; break; }
         out[o++] = (char)(0xE0 | (c >> 12));
         out[o++] = (char)(0x80 | ((c >> 6) & 63));
         out[o++] = (char)(0x80 | (c & 63));
      }
      else
      {
         if (o + 4 >= cap) { o = 0; break; }
         out[o++] = (char)(0xF0 | (c >> 18));
         out[o++] = (char)(0x80 | ((c >> 12) & 63));
         out[o++] = (char)(0x80 | ((c >> 6) & 63));
         out[o++] = (char)(0x80 | (c & 63));
      }
   }
   free(w);
   out[o] = '\0';
   return o > 0;
}

/* a folder that holds an AES coder is encrypted */
static int sz_folder_encrypted(const CSzArEx *db, UInt32 fo)
{
   CSzData sd;
   CSzFolder f;
   UInt32 k;
   if (fo >= db->db.NumFolders)
      return 0;
   sd.Data = db->db.CodersData + db->db.FoCodersOffsets[fo];
   sd.Size = db->db.FoCodersOffsets[fo + 1] - db->db.FoCodersOffsets[fo];
   if (SzGetNextFolderItem(&f, &sd) != SZ_OK)
      return 1;
   for (k = 0; k < f.NumCoders; k++)
      if (f.Coders[k].MethodID == 0x06F10701u)
         return 1;
   return 0;
}

int sz_rom_peek(const char *path, zip_rom_entry *out, int (*name_console)(const char *, size_t))
{
   sz_arc a;
   int rc = -1;
   UInt32 i;

   if (!path || !out || !name_console)
      return -1;
   memset(out, 0, sizeof(*out));
   out->console = -1;
   if (sz_open(&a, path) != 0)
      goto done;
   for (i = 0; i < a.db.NumFiles; i++)
   {
      char nm[sizeof(out->name)];
      const char *leaf;
      size_t nl, j;
      UInt64 sz;
      int c;
      if (SzArEx_IsDir(&a.db, i) || !sz_name(&a.db, i, nm, sizeof(nm)))
         continue;
      nl = strlen(nm);
      c = name_console(nm, nl);
      for (leaf = nm, j = 0; j < nl; j++)
         if (nm[j] == '/' || nm[j] == '\\')
            leaf = nm + j + 1;
      sz = SzArEx_GetFileSize(&a.db, i);
      if (c == 6 && sz < 0x4000)       /* as in a zip: no Genesis ROM is under 16 KiB */
         c = -1;
      if (c < 0 || !leaf[0] || leaf[0] == '.')
         continue;
      out->console = c;
      out->method  = 7;
      out->flags   = 0;
      out->crc     = SzBitWithVals_Check(&a.db.CRCs, i) ? a.db.CRCs.Vals[i] : 0;
      out->usize   = sz > 0xFFFFFFFFu ? 0xFFFFFFFFul : (unsigned long)sz;
      out->local   = i;
      memcpy(out->name, nm, nl + 1);
      {
         UInt32 fo = a.db.FileToFolder[i];
         UInt64 blk = fo == (UInt32)-1 ? 0 : SzAr_GetFolderUnpackSize(&a.db.db, fo);
         out->csize = blk > 0xFFFFFFFFu ? 0xFFFFFFFFul : (unsigned long)blk;
         rc = sz == 0 || sz > ZIP_ROM_MAX || fo == (UInt32)-1 ||
              blk > SZ_ROM_BLOCK_MAX || sz_folder_encrypted(&a.db, fo) ? -2 : 0;
      }
      break;
   }
done:
   sz_close(&a);
   return rc;
}

/* The entry unpacked: *buf is the solid block (the caller frees it), the entry's bytes at *off.  0, -2 (damaged,
 * encrypted or another method) or -1. */
static int sz_load(sz_arc *a, const char *path, const zip_rom_entry *e, Byte **buf, size_t *off, size_t *len)
{
   UInt32 block = 0xFFFFFFFFu;
   size_t bsize = 0;
   SRes r;
   *buf = NULL;
   if (!e || e->method != 7 || e->console < 0 || e->usize == 0 || e->usize > ZIP_ROM_MAX)
      return -1;
   if (sz_open(a, path) != 0 || e->local >= a->db.NumFiles ||
       SzArEx_GetFileSize(&a->db, e->local) != e->usize)
      return -1;
   {
      UInt32 fo = a->db.FileToFolder[e->local];
      if (fo == (UInt32)-1 || SzAr_GetFolderUnpackSize(&a->db.db, fo) > SZ_ROM_BLOCK_MAX ||
          sz_folder_encrypted(&a->db, fo))
         return -2;
   }
   r = SzArEx_Extract(&a->db, &a->look.vt, (UInt32)e->local, &block, buf, &bsize, off, len,
                      &a->main.vt, &a->temp.vt);
   if (r == SZ_OK && *len == e->usize)
      return 0;
   if (*buf)
   {
      ISzAlloc_Free(&a->main.vt, *buf);
      *buf = NULL;
   }
   return r == SZ_ERROR_CRC || r == SZ_ERROR_UNSUPPORTED || r == SZ_ERROR_DATA || r == SZ_ERROR_ARCHIVE ? -2 : -1;
}

int sz_rom_head(const char *path, const zip_rom_entry *e, unsigned char *buf, unsigned n)
{
   sz_arc a;
   Byte *blk;
   size_t off = 0, len = 0;
   unsigned got = 0;
   int rc = -1;
   memset(&a, 0, sizeof(a));
   if (buf && sz_load(&a, path, e, &blk, &off, &len) == 0)
   {
      got = len < n ? (unsigned)len : n;
      memcpy(buf, blk + off, got);
      ISzAlloc_Free(&a.main.vt, blk);
      rc = (int)got;
   }
   sz_close(&a);
   return rc;
}

int sz_rom_extract(const char *path, const zip_rom_entry *e, const char *out_path)
{
   sz_arc a;
   Byte *blk;
   size_t off = 0, len = 0;
   char tmp[600];
   FILE *o;
   int rc, ok;
   memset(&a, 0, sizeof(a));
   if (!out_path || snprintf(tmp, sizeof(tmp), "%s.part", out_path) >= (int)sizeof(tmp))
      return -1;
   rc = sz_load(&a, path, e, &blk, &off, &len);
   if (rc == 0)
   {
      rc = -1;
      o = fopen(tmp, "wb");
      if (o)
      {
         ok = fwrite(blk + off, 1, len, o) == len;
         if (fclose(o) != 0)
            ok = 0;
         remove(out_path);
         if (ok && rename(tmp, out_path) == 0)
            rc = 0;
         else
            remove(tmp);
      }
      ISzAlloc_Free(&a.main.vt, blk);
   }
   sz_close(&a);
   return rc;
}
