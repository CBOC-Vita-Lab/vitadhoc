/* Serialize actual memory state, mutate protocol fields, validate and consume. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../../gba_memory.c"
#include "../../savestate.c"
u32 cpu_ticks, frame_counter, reg[64], cph_bkr, cph_bkw;
u16 palette_ram[512], oam_ram[512], palette_ram_converted[512], io_registers[512];
u8 vram[1024 * 96], vram_clean[VRAM_DIRTY_MAP_BYTES];
u32 vram_dirty_marks;
u8 ewram[1024 * 256 * 2], iwram[1024 * 32 * 2];
u8 *memory_map_read[8 * 1024];
static u8 st[GBA_STATE_MEM_SIZE];
RFILE *filestream_open(const char *p, unsigned m, unsigned h)
{ (void)p; (void)m; (void)h; return NULL; }
int64_t filestream_seek(RFILE *f, int64_t o, int s)
{ (void)f; (void)o; (void)s; return -1; }
int64_t filestream_read(RFILE *f, void *d, int64_t n)
{ (void)f; (void)d; (void)n; return -1; }
int filestream_close(RFILE *f) { (void)f; return 0; }
static void put32(u8 *p, u32 x)
{ p[0]=x; p[1]=x>>8; p[2]=x>>16; p[3]=x>>24; }
static void capture(void)
{
  u32 n=memory_write_savestate(st + 4) + 5;
  assert(n <= sizeof(st)); st[n - 1] = 0; put32(st, n);
  assert(bson_validate_document(st, n, 0, false));
  assert(memory_check_savestate(st));
}
static void make_state(void)
{
  flash_bank_cnt=FLASH_SIZE_64KB;
  eeprom_mode=EEPROM_BASE_MODE; eeprom_counter=0; eeprom_address=0;
  eeprom_size=EEPROM_8_KBYTE;
  memset(io_registers,0,sizeof(io_registers));
  capture();
}
static void set(const char *k, u32 x)
{ put32((u8 *)bson_find_key(bson_find_key(st, "backup"), k), x); }
static void valid_eeprom_phases(void)
{
  unsigned big, bit;
  for (big=0; big<2; big++) {
    make_state(); eeprom_size=big?EEPROM_8_KBYTE:EEPROM_512_BYTE;
    write_eeprom(0,1); capture(); write_eeprom(0,0); capture();
    for(bit=0;bit<(big?14u:6u);bit++) {
      write_eeprom(0,1);capture();assert(memory_read_savestate(st));
    }
    assert(eeprom_address==(big?0x1fff8u:0x1f8u));
    for(bit=0;bit<64;bit++){write_eeprom(0,1);capture();}
    write_eeprom(0,0);capture();
    write_eeprom(0,1);capture();write_eeprom(0,1);capture();
    for(bit=0;bit<(big?14u:6u);bit++){write_eeprom(0,1);capture();}
    write_eeprom(0,0);capture();
    for(bit=0;bit<68;bit++) {
      u32 value=read_eeprom(); if(bit>=4)assert(value==1);
      capture();assert(memory_read_savestate(st));
    }
  }
  puts("EEPROM: writer/validator/restore at every real serial read/write phase PASS");
}
int main(int argc, char **argv)
{
  unsigned i;
  static const u32 aligned_modes[] = { EEPROM_READ_MODE, EEPROM_WRITE_MODE,
    EEPROM_READ_HEADER_MODE, EEPROM_ADDRESS_FOOTER_MODE, EEPROM_WRITE_FOOTER_MODE };
  make_state();
  set("eeprom-mode", EEPROM_WRITE_ADDRESS_MODE); set("eeprom-addr", 0x10000);
#ifdef EXPECT_UNSAFE
  printf("baseline EEPROM partial address accepted=%d\n", memory_check_savestate(st));
  fflush(stdout);
  if (argc > 1 && !strcmp(argv[1], "consume")) {
    assert(memory_read_savestate(st));
    for (i = 0; i < 14; i++) write_eeprom(0, 0);
    return 0;
  }
#else
  (void)argc; (void)argv;
  assert(!memory_check_savestate(st));
#endif
  for (i = 0; i < sizeof(aligned_modes)/sizeof(aligned_modes[0]); i++) {
    make_state(); set("eeprom-mode", aligned_modes[i]); set("eeprom-addr", 0x1ffff);
#ifdef EXPECT_UNSAFE
    printf("baseline EEPROM mode=%u unaligned-end-address accepted=%d\n",
           aligned_modes[i], memory_check_savestate(st));
#else
    assert(!memory_check_savestate(st));
    set("eeprom-addr", 0x1fff8); assert(memory_check_savestate(st));
#endif
  }
#ifndef EXPECT_UNSAFE
  valid_eeprom_phases();
  make_state();
  {
    u8 *io = (u8 *)bson_find_key(bson_find_key(st, "memory"), "ioregs") + 5;
    io[REG_VCOUNT * 2] = 228; io[REG_VCOUNT * 2 + 1] = 0;
    assert(!memory_check_savestate(st));
    io[REG_VCOUNT * 2] = 200; /* valid VCOUNT but impossible visible flag */
    assert(!memory_check_savestate(st));
    io[REG_DISPSTAT * 2] = 1; assert(memory_check_savestate(st));
    io[REG_VCOUNT * 2] = 227; assert(memory_check_savestate(st));
    io[REG_VCOUNT * 2] = 159; assert(!memory_check_savestate(st));
    io[REG_DISPSTAT * 2] = 0; assert(memory_check_savestate(st));
  }
  puts("Memory state: actual writer + EEPROM phase bounds + display timing fields PASS");
#endif
  return 0;
}
