/* gba_save_state writes every byte of its 416 KiB image, so what the buffer
 * held before does not show in the result (retro_serialize no longer clears
 * the buffer first).  The real writers are bson_write_* sequences, which
 * store every byte they pass; here they are stand-ins of the same kind and
 * of realistic sizes, called into buffers that start out zeroed, 0xAA and
 * random.  The images must be identical, valid, with an all-zero padding. */
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include "../savestate.c"

static u8 blob[0x40000];
static u32 seed = 1;
static u32 rnd(void) { seed = seed * 1103515245u + 12345u; return seed >> 8; }

unsigned cpu_write_savestate(u8 *dst)
{
  u8 *start = dst, *hdr;
  u32 regs[7] = { 1, 2, 3, 0xdeadbeef, 5, 6, 7 };
  bson_start_document(dst, "cpu", hdr);
  bson_write_int32array(dst, "regs", regs, 7);
  bson_write_int32(dst, "bus-value", 0x12345678);
  bson_finish_document(dst, hdr);
  return (unsigned)(dst - start);
}

unsigned input_write_savestate(u8 *dst)
{
  u8 *start = dst, *hdr;
  bson_start_document(dst, "input", hdr);
  bson_write_int32(dst, "prevkey", 0x3ff);
  bson_finish_document(dst, hdr);
  return (unsigned)(dst - start);
}

unsigned main_write_savestate(u8 *dst)
{
  u8 *start = dst, *hdr;
  bson_start_document(dst, "emu", hdr);
  bson_write_int32(dst, "frame-count", 1234);
  bson_finish_document(dst, hdr);
  return (unsigned)(dst - start);
}

/* the big arrays: IWRAM, EWRAM, VRAM, OAM, palette, I/O */
unsigned memory_write_savestate(u8 *dst)
{
  u8 *start = dst, *hdr;
  bson_start_document(dst, "memory", hdr);
  bson_write_bytes(dst, "iwram", blob, 0x8000);
  bson_write_bytes(dst, "ewram", blob, 0x40000);
  bson_write_bytes(dst, "vram", blob + 7, 0x18000);
  bson_write_bytes(dst, "oamram", blob + 1, 0x400);
  bson_write_bytes(dst, "palram", blob + 2, 0x400);
  bson_write_bytes(dst, "ioregs", blob + 3, 0x400);
  bson_finish_document(dst, hdr);
  return (unsigned)(dst - start);
}

unsigned sound_write_savestate(u8 *dst)
{
  u8 *start = dst, *hdr;
  bson_start_document(dst, "sound", hdr);
  bson_write_bytes(dst, "wav-samples", blob + 5, 64);
  bson_finish_document(dst, hdr);
  return (unsigned)(dst - start);
}

int main(void)
{
  static u8 img[3][GBA_STATE_MEM_SIZE];
  const u8 *pad;
  u32 i, padlen;

  for (i = 0; i < sizeof(blob); i++)
    blob[i] = (u8)rnd();
  memset(img[0], 0x00, GBA_STATE_MEM_SIZE);
  memset(img[1], 0xAA, GBA_STATE_MEM_SIZE);
  for (i = 0; i < GBA_STATE_MEM_SIZE; i++)
    img[2][i] = (u8)rnd();
  for (i = 0; i < 3; i++)
    gba_save_state(img[i]);

  assert(memcmp(img[0], img[1], GBA_STATE_MEM_SIZE) == 0);
  assert(memcmp(img[0], img[2], GBA_STATE_MEM_SIZE) == 0);
  assert(bson_read_u32(img[0]) == GBA_STATE_MEM_SIZE);
  assert(img[0][GBA_STATE_MEM_SIZE - 1] == 0);
  assert(bson_validate_document(img[0], GBA_STATE_MEM_SIZE, 0, false));

  pad = bson_find_key(img[0], "zpadding");
  assert(pad);
  padlen = bson_read_u32(pad);
  assert(padlen > 0);
  assert((u32)(pad + 5 - img[0]) + padlen + 1 == GBA_STATE_MEM_SIZE);
  for (i = 0; i < padlen; i++)
    assert(pad[5 + i] == 0);
  return 0;
}
