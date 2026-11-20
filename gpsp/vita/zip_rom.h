/* zip_rom.h -- games kept in .zip files.
 *
 *   zip_rom_peek     which game a .zip holds: its first entry named .gba,
 *                    .gb, .gbc, .sfc, .smc, .nes, .fds, .unf, .unif, .ws, .wsc, .pc2, .md, .gen, .smd, .bin, .68k, .sms, .gg, .sg or .sc.  0 = found and usable; -2 = found but in a
 *                    form this reader refuses (encrypted, another method,
 *                    too large); -1 = no game in it, or not a zip.
 *   zip_rom_extract  that entry, checked against its CRC-32, written to
 *                    out_path (through out_path.part).  0 = done; -2 = the
 *                    data does not match its CRC; -1 = could not.
 */
#ifndef ZIP_ROM_H
#define ZIP_ROM_H

#define ZIP_ROM_MAX (32ul << 20)       /* a GBA cartridge's address space */

typedef struct
{
   int           console;              /* 0 GBA, 1 GB, 2 GBC, 3 SNES, 4 NES, 5 WS, 6 MD, 7 SMS, 8 GG, 9 SG (fe_console_t) */
   int           method;               /* 0 stored, 8 deflated            */
   unsigned      flags;
   unsigned long crc, csize, usize, local;
   char          name[256];            /* the entry's name in the archive */
} zip_rom_entry;

int zip_rom_peek(const char *path, zip_rom_entry *out);
int zip_rom_extract(const char *path, const zip_rom_entry *e,
                    const char *out_path);

/* The first n bytes of the entry (after the same CRC check); the count, or -1. */
int zip_rom_head(const char *path, const zip_rom_entry *e, unsigned char *buf,
                 unsigned n);

#endif
