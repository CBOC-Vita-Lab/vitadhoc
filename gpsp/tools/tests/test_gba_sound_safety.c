/* Actual register write + mixer: changing noise width without a trigger.
 * Also tests real sound writer/validator round trips and unsafe state fields. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../../gba_memory.c"
#include "../../sound.c"
#include "../../savestate.c"
u32 cpu_ticks, frame_counter, execute_cycles, reg[64], cph_dman, cph_bkr, cph_bkw;
u32 cph_amix, gbc_update_count;
u32 core_phase_enter(u32 lvl) { (void)lvl; return 0; }
void core_phase_leave(u32 lvl, u32 *acc, u32 t0)
{ (void)lvl; (void)acc; (void)t0; }
timer_type timer[4];
u16 palette_ram[512], oam_ram[512], palette_ram_converted[512], io_registers[512];
u8 vram[1024 * 96], vram_clean[VRAM_DIRTY_MAP_BYTES];
u32 vram_dirty_marks;
u8 ewram[1024 * 256 * 2], iwram[1024 * 32 * 2];
u8 *memory_map_read[8 * 1024];
s32 affine_reference_x[2], affine_reference_y[2];
RFILE *filestream_open(const char *p, unsigned m, unsigned h)
{ (void)p; (void)m; (void)h; return NULL; }
int64_t filestream_seek(RFILE *f, int64_t o, int s)
{ (void)f; (void)o; (void)s; return -1; }
int64_t filestream_read(RFILE *f, void *d, int64_t n)
{ (void)f; (void)d; (void)n; return -1; }
int filestream_close(RFILE *f) { (void)f; return 0; }
void smc_prof_note_iwram(u32 o) { (void)o; }
void smc_prof_note_ewram(u32 o) { (void)o; }
cpu_alert_type flag_interrupt(irq_type irq) { (void)irq; return CPU_ALERT_NONE; }
cpu_alert_type check_interrupt(void) { return CPU_ALERT_NONE; }
cpu_alert_type write_siocnt(u16 value) { (void)value; return CPU_ALERT_NONE; }
cpu_alert_type write_rcnt(u16 value) { (void)value; return CPU_ALERT_NONE; }

static unsigned char st[8192];
static void store32(unsigned char *p, u32 v)
{ p[0]=v; p[1]=v>>8; p[2]=v>>16; p[3]=v>>24; }
static void make_state(void)
{
  unsigned n;
  init_sound();
  n = sound_write_savestate(st + 4) + 5;
  assert(n <= sizeof(st));
  st[n - 1] = 0; store32(st, n);
  assert(bson_validate_document(st, n, 0, false));
  assert(sound_check_savestate(st)); /* positive control: the actual writer */
}
static void bad_field(const char *channel, const char *key, u32 value)
{
  const u8 *d;
  make_state();
  d = bson_find_key(st, "sound");
  if (channel) d = bson_find_key(d, channel);
  store32((u8 *)bson_find_key(d, key), value);
#ifdef EXPECT_UNSAFE
  printf("baseline sound field %s/%s=%08x accepted=%d\n",
         channel ? channel : "global", key, value, sound_check_savestate(st));
#else
  assert(!sound_check_savestate(st));
#endif
}
static void noise_switch(void)
{
  unsigned i;
  init_sound();
  sound_on = 1;
  gbc_sound_master_volume_left = gbc_sound_master_volume_right = 7;
  gbc_sound_master_volume = 2;
  gbc_sound_channel[3].envelope_initial_volume = 15;
  gbc_sound_channel[3].status = GBC_SOUND_LEFTRIGHT;
  (void)write_io_register16(0x0400007c, 0x8000); /* long LFSR, trigger */
  for (i = 0; i < 500; i++) { cpu_ticks += 256; render_gbc_sound(); }
  assert(gbc_sound_channel[3].sample_index >= (126u << 16));
  /* Only width changes. Cartridge software need not retrigger. */
  (void)write_io_register16(0x0400007c, 0x0008);
  cpu_ticks += 256;
  render_gbc_sound();
#ifndef EXPECT_UNSAFE
  assert(gbc_sound_channel[3].sample_index < (126u << 16));
  for (i = 0; i < 4096; i++) {
    (void)write_io_register16(0x0400007c, i & 1 ? 8 : 0);
    cpu_ticks += 1024; render_gbc_sound();
  }
  puts("PSG width switches without retrigger: real MMIO/mixer, 4096 switches PASS");
#endif
}
int main(int argc, char **argv)
{
  if (argc > 1 && !strcmp(argv[1], "noise")) { noise_switch(); return 0; }
  bad_field(NULL, "gbc-ms-vol-left", 8);
  bad_field(NULL, "gbc-ms-vol-right", 8);
  bad_field(NULL, "gbc-ms-vol", 4);
  bad_field("gs0", "rate", 2048);
  bad_field("gs3", "sample-idx", 0xffffffffu);
#ifndef EXPECT_UNSAFE
  puts("PSG malformed-state safety: real writer positive control and 5 rejected fields PASS");
  noise_switch();
#endif
  return 0;
}
