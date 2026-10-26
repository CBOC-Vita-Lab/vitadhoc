/* Portable, real-MMIO regressions for the PSG sequencer, routing and FIFO reset.
 * Reuse the existing production-core fixture, including its host-only stubs. */
#define main sound_safety_fixture_main
#include "test_gba_sound_safety.c"
#undef main

static u8 mixer_state[8192];

static void begin_rate(unsigned bits)
{
  unsigned i;
  memset(timer, 0, sizeof(timer));
  memset(dma, 0, sizeof(dma));
  for (i = 0; i < DMA_CHAN_CNT; i++)
    dma[i].direct_sound_channel = DMA_NO_DIRECT_SOUND;
  memset(io_registers, 0, sizeof(io_registers));
  cpu_ticks = 0;
  cph_dman = 0;
  sound_freq_bits = bits;
  sound_frequency = 1u << bits;
  init_sound();
  (void)write_io_register16(0x04000084, 0x0080);
  (void)write_io_register16(0x04000080, 0x3377);
  (void)write_io_register16(0x04000082, 0x0002);
}

static void advance_ticks(unsigned ticks)
{
  while (ticks) {
    unsigned step = ticks > 4096 ? 4096 : ticks;
    cpu_ticks += step;
    render_gbc_sound();
    ticks -= step;
  }
}

/* Preserve an active note through a runtime update or a real writer/reader
 * round trip. A restore must derive its clock from the current output rate,
 * not the output rate under which the document was written. */
static void apply_rate(unsigned bits, unsigned mode)
{
  unsigned n;
  if (mode == 2) {
    n = sound_write_savestate(mixer_state + 4) + 5;
    assert(n <= sizeof(mixer_state));
    mixer_state[n - 1] = 0;
    store32(mixer_state, n);
    assert(bson_validate_document(mixer_state, n, 0, false));
    assert(sound_check_savestate(mixer_state));
  }
  sound_freq_bits = bits;
  sound_frequency = 1u << bits;
  if (mode == 1)
    sound_frequency_changed();
  if (mode == 2)
    assert(sound_read_savestate(mixer_state));
}

static void test_sequencer(unsigned mode)
{
  unsigned bits;
  for (bits = 15; bits <= 16; bits++) {
    unsigned initial_bits = mode ? 31 - bits : bits;
    unsigned elapsed = 0;

    begin_rate(initial_bits);
    (void)write_io_register16(0x04000068, 0xf080); /* 64 length ticks */
    (void)write_io_register16(0x0400006c, 0xc400);
    apply_rate(bits, mode);
    while (gbc_sound_channel[1].active_flag && elapsed < GBC_BASE_RATE_INT) {
      advance_ticks(4096);
      elapsed += 4096;
    }
    printf("PSG mode=%u rate=%u length=%u clocks\n", mode,
           sound_frequency, elapsed);
    fflush(stdout);
    assert(elapsed == GBC_BASE_RATE_INT / 4);

    begin_rate(initial_bits);
    (void)write_io_register16(0x04000068, 0xf180); /* period 1, decrease */
    (void)write_io_register16(0x0400006c, 0x8400);
    apply_rate(bits, mode);
    advance_ticks(GBC_BASE_RATE_INT / 8);
    assert(gbc_sound_channel[1].envelope_volume == 7); /* 8 x 64 Hz */

    begin_rate(initial_bits);
    (void)write_io_register16(0x04000060, 0x0019); /* 128 Hz, halve */
    (void)write_io_register16(0x04000062, 0xf080);
    (void)write_io_register16(0x04000064, 0x8400); /* rate 1024 */
    apply_rate(bits, mode);
    advance_ticks(GBC_BASE_RATE_INT / 64);
    assert(gbc_sound_channel[0].rate == 256); /* two sweep clocks */
    printf("PSG mode=%u rate=%u envelope=7 sweep-rate=256 PASS\n",
           mode, sound_frequency);
  }
}

static void test_all_routing_masks(void)
{
  unsigned mask, channel;
  begin_rate(16);
  for (mask = 0; mask < 256; mask++) {
    (void)write_io_register16(0x04000080, (mask << 8) | 0x77);
    for (channel = 0; channel < 4; channel++) {
      unsigned want = 0;
      if (mask & (1u << channel)) want |= GBC_SOUND_RIGHT;
      if (mask & (0x10u << channel)) want |= GBC_SOUND_LEFT;
      if (gbc_sound_channel[channel].status != want)
        fprintf(stderr, "routing mask=%02x channel=%u got=%u want=%u\n",
                mask, channel, gbc_sound_channel[channel].status, want);
      assert(gbc_sound_channel[channel].status == want);
    }
  }
  /* The old neighboring-left leak is also checked at the rendered bus. */
  (void)write_io_register16(0x04000080, 0x1077);
  (void)write_io_register16(0x04000068, 0xf080);
  (void)write_io_register16(0x0400006c, 0x8400);
  advance_ticks(4096);
  for (channel = 0; channel < 32; channel++)
    assert(sound_buffer[channel] == 0);
  puts("PSG 256 stereo routing masks and unrouted rendered channel PASS");
}

static void test_fifo_reset(void)
{
  static const unsigned positions[][2] = {{0, 28}, {7, 27}, {29, 25}};
  unsigned channel, position, i;
  for (channel = 0; channel < 2; channel++) {
    for (position = 0; position < 3; position++) {
      direct_sound_struct *ds = &direct_sound_channel[channel];
      begin_rate(16);
      memset(ds->fifo, 0x20, sizeof(ds->fifo));
      ds->fifo_base = positions[position][0];
      ds->fifo_top = positions[position][1];
      ds->fifo_fractional = 0x00234567;
      ds->buffer_index = 400;
      gbc_sound_buffer_index = 400;
      sound_buffer_base = 100;
      sound_buffer[398] = 1234;
      (void)write_io_register16(0x04000082,
          channel ? 0xb008 : 0x0b04); /* reset selected FIFO, stereo */
      printf("FIFO %u reset from %u/%u -> %u/%u\n", channel,
             positions[position][0], positions[position][1],
             ds->fifo_base, ds->fifo_top);
      fflush(stdout);
      assert(ds->fifo_base == 0 && ds->fifo_top == 0);
      assert(ds->fifo_fractional == 0x00234567);
      assert(ds->buffer_index == 400);
      assert(gbc_sound_buffer_index == 400 && sound_buffer_base == 100);
      assert(sound_buffer[398] == 1234);
      for (i = 0; i < 32; i++) assert(ds->fifo[i] == 0);
      sound_timer_queue32(channel, 0x40404040);
      assert(ds->fifo_top == 4);
      assert(sound_timer(0x01000000, channel) == 0);
      assert(ds->fifo_base == 1);
      assert(sound_buffer[400] == 16384 && sound_buffer[401] == 16384);
    }
  }
  puts("FIFO A/B reset empties nonzero/wrapped queues and preserves timeline PASS");
}

static void arm_fifo_dma(unsigned channel)
{
  dma_transfer_type *d = &dma[1];
  memset(ewram, 0x60, 16);
  d->source_address = 0x02000000;
  d->dest_address = channel ? 0x040000a4 : 0x040000a0;
  d->length = 4;
  d->repeat_type = DMA_REPEAT;
  d->direct_sound_channel = channel;
  d->source_direction = DMA_INCREMENT;
  d->dest_direction = DMA_FIXED;
  d->length_type = DMA_32BIT;
  d->start_type = DMA_START_SPECIAL;
}

static void test_fifo_dma_threshold(void)
{
  unsigned channel;
  for (channel = 0; channel < 2; channel++) {
    direct_sound_struct *ds = &direct_sound_channel[channel];
    begin_rate(16);
    arm_fifo_dma(channel);
    ds->status = DIRECT_SOUND_LEFTRIGHT;
    ds->volume_halve = 0;
    memset(ds->fifo, 0x40, sizeof(ds->fifo));
    ds->fifo_base = 29;
    ds->fifo_top = 15; /* wrapped: 18 bytes, then 17 -> no refill */
    (void)sound_timer(0x01000000, channel);
    assert(cph_dman == 0 && ds->fifo_top == 15);
    (void)sound_timer(0x01000000, channel); /* exactly 16 -> refill */
    assert(cph_dman == 1 && ds->fifo_base == 31 && ds->fifo_top == 31);
    assert(dma[1].source_address == 0x02000010);
    assert(ds->fifo[15] == 0x60 && ds->fifo[30] == 0x60);

    begin_rate(16);
    arm_fifo_dma(channel);
    ds->fifo_base = 29;
    ds->fifo_top = 25; /* wrapped, stale 28-byte queue before reset */
    (void)write_io_register16(0x04000082, channel ? 0xb008 : 0x0b04);
    sound_timer_queue32(channel, 0x40404040);
    (void)sound_timer(0x01000000, channel);
    printf("FIFO %u reset/new-write first=%d DMA-refills=%u\n",
           channel, sound_buffer[0], cph_dman);
    fflush(stdout);
    assert(sound_buffer[0] == 16384 && sound_buffer[1] == 16384);
    assert(cph_dman == 1); /* consume before DMA, then refill the empty queue */
    assert(ds->fifo_base == 1 && ds->fifo_top == 20);
    assert(ds->fifo[0] == 0x40 && ds->fifo[3] == 0x40);
    assert(ds->fifo[4] == 0x60 && ds->fifo[19] == 0x60);
    assert(dma[1].source_address == 0x02000010);
  }
  puts("FIFO wrapped 17/16-byte DMA threshold and reset/refill ordering PASS");
}

int main(int argc, char **argv)
{
  const char *which = argc > 1 ? argv[1] : "all";
  if (!strcmp(which, "init") || !strcmp(which, "all")) test_sequencer(0);
  if (!strcmp(which, "changed") || !strcmp(which, "all")) test_sequencer(1);
  if (!strcmp(which, "restore") || !strcmp(which, "all")) test_sequencer(2);
  if (!strcmp(which, "routing") || !strcmp(which, "all")) test_all_routing_masks();
  if (!strcmp(which, "fifo") || !strcmp(which, "all")) test_fifo_reset();
  if (!strcmp(which, "dma") || !strcmp(which, "all")) test_fifo_dma_threshold();
  puts("PASS audio mixer regressions");
  return 0;
}
