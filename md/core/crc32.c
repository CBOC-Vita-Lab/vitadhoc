/*
 * CRC-32 (IEEE 802.3, reflected, as zlib's crc32()) for the core's SRAM change check and the Master System ROM
 * database. Written for VitAdhoc to replace the libretro-common one; not part of Genesis Plus GX.
 * Copyright CBOC-Vita. MIT licence.
 */
#include "shared.h"

unsigned long crc32(unsigned long crc, const unsigned char *buf, unsigned int len)
{
  static unsigned long table[256];
  static int ready;
  unsigned int i, k;
  if (!ready)
  {
    for (i = 0; i < 256; i++)
    {
      unsigned long c = i;
      for (k = 0; k < 8; k++) c = (c & 1) ? (0xEDB88320UL ^ (c >> 1)) : (c >> 1);
      table[i] = c;
    }
    ready = 1;
  }
  crc = ~crc & 0xFFFFFFFFUL;
  for (i = 0; i < len; i++) crc = table[(crc ^ buf[i]) & 0xFF] ^ (crc >> 8);
  return ~crc & 0xFFFFFFFFUL;
}
