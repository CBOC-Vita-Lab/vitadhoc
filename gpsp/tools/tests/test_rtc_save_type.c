/* White-box tests for [rtc] the cartridge clock following the console's
 * clock, and [save-type] the per-game save type and the 128 KiB file hint.
 * Includes gba_memory.c itself, as test_rtc_protocol.c does. */
#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include "../../gba_memory.c"

RFILE *filestream_open(const char *path, unsigned mode, unsigned hints)
{ (void)path; (void)mode; (void)hints; return NULL; }
int64_t filestream_seek(RFILE *stream, int64_t offset, int seek_position)
{ (void)stream; (void)offset; (void)seek_position; return -1; }
int64_t filestream_read(RFILE *stream, void *data, int64_t len)
{ (void)stream; (void)data; (void)len; return -1; }
int filestream_close(RFILE *stream)
{ (void)stream; return 0; }

u32 cpu_ticks;
u32 frame_counter;
u32 reg[64];
u32 cph_bkr, cph_bkw;
u16 palette_ram[512];
u16 oam_ram[512];
u16 palette_ram_converted[512];
u16 io_registers[512];
u8 vram[1024 * 96];
u8 vram_clean[VRAM_DIRTY_MAP_BYTES];
u32 vram_dirty_marks;
u8 ewram[1024 * 256 * 2];
u8 iwram[1024 * 32 * 2];
u8 *memory_map_read[8 * 1024];


static time_t fake_now;                 /* the tests' hook: the Vita's clock */
static time_t fake_clock(void) { return fake_now; }

#define T0 ((s64)1791504000)            /* 2026-10-09 00:00:00 */
#define DAY ((s64)86400)

static void reset_session(void)
{
  rtc_last_shown = 0;
  gpsp_rtc_frames_only = 0;
  gpsp_rtc_wall = 0;
  frame_counter = 0;
  rtc_base_time = 0;
}

static void test_clock(void)
{
  /* Before: no wall clock published = base + frames, bit for bit. */
  reset_session();
  gpsp_wallclock = fake_clock;
  fake_now = T0;
  rtc_init_base_time();
  frame_counter = 60 * 600;                       /* ten minutes of play */
  assert(rtc_current_time() ==
         (time_t)(T0 + (s64)((double)frame_counter * GBA_FRAME_SECONDS)));

  /* Save & Quit, three days away, Continue: a new load, the old base and
   * frame counter come back from the resume state. */
  {
    s64 saved_base = rtc_base_time;
    u32 saved_frames = frame_counter;
    reset_session();
    fake_now = T0 + 3 * DAY;
    rtc_init_base_time();
    gpsp_rtc_wall = fake_now;                     /* fe_host publishes */
    rtc_base_time = saved_base;                   /* state load */
    frame_counter = saved_frames;
    assert(rtc_current_time() == fake_now);       /* the real time, not +600s */
  }

  /* An old state (base written long ago, any frame count) loads and the
   * game sees the console's time. */
  reset_session();
  fake_now = T0 + 10 * DAY;
  gpsp_rtc_wall = fake_now;
  rtc_base_time = T0 - 400 * DAY;
  frame_counter = 123456;
  assert(rtc_current_time() == fake_now);
  /* ... and a state saved now continues from now. */
  assert(rtc_base_time + (s64)((double)frame_counter * GBA_FRAME_SECONDS)
         == (s64)fake_now);

  /* The clock moves with the console, never backwards. */
  fake_now += 60; gpsp_rtc_wall = fake_now;
  assert(rtc_current_time() == fake_now);
  gpsp_rtc_wall = fake_now - 30;                  /* a late reading */
  assert(rtc_current_time() == fake_now);         /* holds still */

  /* Fast-forward (frames far ahead of the clock) cannot run it ahead. */
  frame_counter += 60 * 3600;
  gpsp_rtc_wall = fake_now + 5;
  assert(rtc_current_time() == fake_now + 5);

  /* A link session forces the frame clock on both machines. */
  reset_session();
  rtc_base_time = T0;
  frame_counter = 0;
  gpsp_rtc_wall = T0 + 9 * DAY;
  gpsp_rtc_frames_only = 1;
  assert(rtc_current_time() == (time_t)T0);
  gpsp_rtc_frames_only = 0;

  /* The 1970 base of an uptime clock is replaced by the console's. */
  reset_session();
  fake_now = T0;
  gpsp_rtc_wall = T0;
  rtc_base_time = 5000;
  assert(rtc_current_time() == (time_t)T0);
}

static void fresh_cart(u32 type, u32 banks)
{
  backup_type_reset = type;
  flash_bank_cnt = banks;
  flash_device_id = banks == FLASH_SIZE_128KB ? FLASH_DEVICE_SANYO_128KB
                                              : FLASH_DEVICE_MACRONIX_64KB;
  backup_type_eeprom_guessed = 0;
  detected_rom_sigs = 0;
  is_known_game = false;
  gpsp_force_save_type = GPSP_SAVE_AUTO;
  gpsp_save_size_hint = 0;
}

static void test_save_type(void)
{
  /* Auto, no file: detection is left alone. */
  fresh_cart(BACKUP_FLASH, FLASH_SIZE_64KB);
  apply_save_type_choice();
  assert(backup_type_reset == BACKUP_FLASH && flash_bank_cnt == FLASH_SIZE_64KB);

  /* Auto, 131072-byte save, hack says 64 KB flash: 128 KB (gpsp#246). */
  gpsp_save_size_hint = 131072;
  apply_save_type_choice();
  assert(backup_type_reset == BACKUP_FLASH && flash_bank_cnt == FLASH_SIZE_128KB);
  assert(flash_device_id == FLASH_DEVICE_SANYO_128KB);
  assert(!strcmp(gpsp_backup_name(), "flash128"));

  /* ... with the 16-byte trailer too, and from "nothing found". */
  fresh_cart(BACKUP_UNKN, FLASH_SIZE_64KB);
  gpsp_save_size_hint = 131072 + 16;
  apply_save_type_choice();
  assert(backup_type_reset == BACKUP_FLASH && flash_bank_cnt == FLASH_SIZE_128KB);

  /* A ROM naming EEPROM and FLASH1M with a 128 KiB file is flash. */
  fresh_cart(BACKUP_EEPROM, FLASH_SIZE_64KB);
  detected_rom_sigs = ROM_SIG_EEPROM | ROM_SIG_FLASH1M;
  gpsp_save_size_hint = 131072;
  apply_save_type_choice();
  assert(backup_type_reset == BACKUP_FLASH && flash_bank_cnt == FLASH_SIZE_128KB);

  /* Not for a plain EEPROM game, an explicit SRAM game, or a known game. */
  fresh_cart(BACKUP_EEPROM, FLASH_SIZE_64KB);
  detected_rom_sigs = ROM_SIG_EEPROM;
  gpsp_save_size_hint = 131072;
  apply_save_type_choice();
  assert(backup_type_reset == BACKUP_EEPROM);
  fresh_cart(BACKUP_SRAM, FLASH_SIZE_64KB);
  gpsp_save_size_hint = 131072;
  apply_save_type_choice();
  assert(backup_type_reset == BACKUP_SRAM);
  fresh_cart(BACKUP_FLASH, FLASH_SIZE_64KB);
  is_known_game = true;
  gpsp_save_size_hint = 131072;
  apply_save_type_choice();
  assert(flash_bank_cnt == FLASH_SIZE_64KB);

  /* A 64 KiB file or a missing file changes nothing. */
  fresh_cart(BACKUP_FLASH, FLASH_SIZE_64KB);
  gpsp_save_size_hint = 65536;
  apply_save_type_choice();
  assert(flash_bank_cnt == FLASH_SIZE_64KB);

  /* The player's choice wins over detection and over the hint. */
  fresh_cart(BACKUP_FLASH, FLASH_SIZE_128KB);
  gpsp_force_save_type = GPSP_SAVE_SRAM;
  apply_save_type_choice();
  assert(backup_type_reset == BACKUP_SRAM);
  gpsp_force_save_type = GPSP_SAVE_FLASH64;
  gpsp_save_size_hint = 131072;
  apply_save_type_choice();
  assert(backup_type_reset == BACKUP_FLASH && flash_bank_cnt == FLASH_SIZE_64KB);
  assert(flash_device_id == FLASH_DEVICE_MACRONIX_64KB);
  gpsp_force_save_type = GPSP_SAVE_FLASH128;
  apply_save_type_choice();
  assert(flash_bank_cnt == FLASH_SIZE_128KB);
  gpsp_force_save_type = GPSP_SAVE_EEPROM8K;
  backup_type_eeprom_guessed = 1;
  apply_save_type_choice();
  assert(backup_type_reset == BACKUP_EEPROM && !backup_type_eeprom_guessed);
  assert(eeprom_size_forced == EEPROM_8_KBYTE);
  assert(!strcmp(gpsp_backup_name(), "eeprom8k"));
  gpsp_force_save_type = GPSP_SAVE_EEPROM512;
  apply_save_type_choice();
  assert(eeprom_size_forced == EEPROM_512_BYTE);
  /* Back to Auto drops the forced EEPROM size. */
  gpsp_force_save_type = GPSP_SAVE_AUTO;
  apply_save_type_choice();
  assert(eeprom_size_forced == 0);
}

int main(void)
{
  test_clock();
  test_save_type();
  puts("ok");
  return 0;
}
