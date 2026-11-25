/* sz_rom.h -- games kept in .7z files (the reader behind zip_rom.h).
 *
 * zip_rom_peek / zip_rom_extract / zip_rom_head call these for a file that
 * begins with the 7z signature (zip_rom.c looks).  A .7z is read with Igor Pavlov's LZMA SDK
 * (third_party/lzma, public domain): LZMA, LZMA2, BCJ and Delta filters;
 * no encryption, no PPMd, no BCJ2 (the SDK's own set).
 *
 * Memory: a solid block is unpacked whole (the SDK decodes into one buffer),
 * so an archive whose block holding the game is larger than SZ_ROM_BLOCK_MAX
 * is refused, as is a game larger than ZIP_ROM_MAX.  Every allocation the
 * SDK makes is capped (a damaged header cannot ask for gigabytes).
 */
#ifndef SZ_ROM_H
#define SZ_ROM_H

#include "zip_rom.h"

#define SZ_ROM_BLOCK_MAX (64ul << 20)  /* the largest solid block unpacked */

/* Same contract as zip_rom_peek.  out->method = 7, out->local = the file's
 * index in the archive, out->csize = the solid block's size. */
int sz_rom_peek(const char *path, zip_rom_entry *out, int (*name_console)(const char *, size_t));

/* The entry's data written to out_path (through out_path.part), or its first
 * n bytes into buf (the count, or -1).  0 / -2 / -1 as zip_rom_extract. */
int sz_rom_extract(const char *path, const zip_rom_entry *e, const char *out_path);
int sz_rom_head(const char *path, const zip_rom_entry *e, unsigned char *buf, unsigned n);

#endif
