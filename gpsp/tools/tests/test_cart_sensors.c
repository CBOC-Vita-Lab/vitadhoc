/* Sensor cartridges (cart_sensor.h): Boktai's light sensor, WarioWare:
 * Twisted!'s gyro and Yoshi Topsy-Turvy's tilt sensor, driven the way the
 * games drive them (GBATEK's example routines), through the cartridge
 * window the CPU reads.  Also: the clock chip still works beside the light
 * sensor, a frame's reading is held, the state round-trips, and a game
 * without a sensor writes the same save state as before. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
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

#include "../../frontend-common/fe_sensor.h"

static u8 rom_page[0x8000];

static void port(u16 v)  { write_gpio(0xC4, v); }
static u16 pins(void)    { return read_memory16(0x080000C4); }

static void cart(u32 kinds)
{
  memset(gpio_regs, 0, sizeof(gpio_regs));
  cart_sensors = kinds;
  cart_sensor_reset();
  rtc_enabled = !!(kinds & CART_SENSOR_SOLAR);
}

/* GBATEK "GBA Cart Solar Sensor", example code. */
static unsigned solar_read(void)
{
  unsigned r0 = 0;
  write_gpio(0xC8, 1);
  write_gpio(0xC6, 7);
  port(2);
  port(0);
  for (;;)
  {
    port(1);
    port(0);
    if (pins() & 8)
      break;
    if (++r0 & 0x100)
      break;
  }
  return r0;
}

/* GBATEK "GBA Cart Gyro Sensor", read_gyro: sixteen bits, MSB first, the
 * data pin read before each falling clock edge. */
static unsigned gyro_read16(void)
{
  unsigned i, v = 0, r2;
  write_gpio(0xC8, 1);
  write_gpio(0xC6, 0x0B);
  r2 = pins() | 3;
  port(r2);                  /* gpio0 = 1: start */
  r2 &= ~1u;
  port(r2);                  /* gpio0 = 0, clock still high */
  for (i = 0; i < 16; i++)
  {
    v = (v << 1) | ((pins() >> 2) & 1);
    port(r2 & ~2u);          /* clock low: the next bit */
    port(r2 | 2u);           /* clock high */
  }
  return v;
}

static void tilt_sample(void)
{
  write_backup(0x8000, 0x55);
  write_backup(0x8100, 0xAA);
}

static unsigned tilt_x_read(void)
{
  return ((read_backup(0x8300) & 0xF) << 8) | read_backup(0x8200);
}

static unsigned tilt_y_read(void)
{
  return ((read_backup(0x8500) & 0xF) << 8) | read_backup(0x8400);
}

static unsigned save_state(u8 *state, size_t cap)
{
  unsigned n = memory_write_savestate(state + 4);
  assert(n > 0 && n + 4 < cap);
  bson_write_u32(state, n + 4);
  return n + 4;
}

int main(void)
{
  static u8 state[512 * 1024], state2[512 * 1024];
  unsigned i, prev, n;

  gamepak_size = sizeof(rom_page);
  memory_map_read[0x08000000 >> 15] = rom_page;

  /* ---- solar ------------------------------------------------------- */
  cart(CART_SENSOR_SOLAR);
  assert(solar_read() == 0xE8);              /* nothing published: dark */
  prev = 0x100;
  for (i = 0; i <= 10; i++)
  {
    unsigned v;
    gpsp_sensor_set(fe_sensor_solar((int)i));
    v = solar_read();
    printf("solar level %2u -> %02X\n", i, v);
    assert(v < prev);                        /* brighter counts less */
    prev = v;
  }
  assert(prev <= 0x50);                      /* full sun fills Boktai's gauge */
  gpsp_sensor_set(fe_sensor_solar(0));
  assert(solar_read() == 0xE8);              /* GBATEK: E8h, darkness */
  gpsp_sensor_set(fe_sensor_solar(99));      /* clamped */
  assert(solar_read() == prev);

  /* The clock chip owns the port while bit 2 is high: a reset pulse sent
   * with it selected does not touch the counter. */
  gpsp_sensor_set(fe_sensor_solar(5));
  (void)solar_read();
  n = solar_counter;
  port(4 | 2);
  port(4 | 1);
  port(4);
  assert(solar_counter == n);
  port(0);

  /* A reading published after the reset does not change the threshold of
   * a measurement under way: the level is latched at reset. */
  gpsp_sensor_set(fe_sensor_solar(10));
  write_gpio(0xC8, 1);
  write_gpio(0xC6, 7);
  port(2); port(0);
  gpsp_sensor_set(fe_sensor_solar(0));
  for (i = 0; i < 0x60; i++) { port(1); port(0); }
  assert(pins() & 8);                        /* full sun's threshold held */

  /* ---- gyro -------------------------------------------------------- */
  cart(CART_SENSOR_GYRO);
  assert(gyro_read16() == CART_GYRO_STILL);  /* nothing published: still */
  gpsp_sensor_set(fe_sensor_gyro(0.0f));
  assert(gyro_read16() == CART_GYRO_STILL);
  gpsp_sensor_set(fe_sensor_gyro(-3.0f));    /* clockwise as the player sees */
  n = gyro_read16();
  printf("gyro clockwise 3 rad/s -> %03X\n", n);
  assert(n > CART_GYRO_STILL + 100 && n < 0x9E3);
  gpsp_sensor_set(fe_sensor_gyro(3.0f));
  n = gyro_read16();
  assert(n < CART_GYRO_STILL - 100 && n > 0x354);
  gpsp_sensor_set(fe_sensor_gyro(-1000.0f)); /* clamped to 12 bits */
  assert(gyro_read16() == 0xFFF);
  gpsp_sensor_set(fe_sensor_gyro(1000.0f));
  assert(gyro_read16() == 0x000);
  /* The four leading bits are zero, and the bit 3 rumble pin the game
   * drives is left alone. */
  gpsp_sensor_set(GPSP_SENSOR_VALID | 0xABC);
  assert(gyro_read16() == 0xABC);
  write_gpio(0xC6, 0x0B);
  port(8 | 1);
  assert(pins() & 8);
  port(0);

  /* A sample is taken at the start pulse: what is published while it is
   * shifted out arrives with the next one. */
  gpsp_sensor_set(GPSP_SENSOR_VALID | 0x123);
  write_gpio(0xC6, 0x0B);
  port(3); port(2);
  gpsp_sensor_set(GPSP_SENSOR_VALID | 0xFED);
  for (n = 0, i = 0; i < 16; i++)
  {
    n = (n << 1) | ((pins() >> 2) & 1);
    port(0); port(2);
  }
  assert(n == 0x123);
  assert(gyro_read16() == 0xFED);

  /* ---- tilt -------------------------------------------------------- */
  cart(CART_SENSOR_TILT);
  assert(tilt_x_read() == 0xFFF && tilt_y_read() == 0xFFF);  /* never sampled */
  tilt_sample();                                             /* nothing published */
  assert(tilt_x_read() == CART_TILT_X_LEVEL && tilt_y_read() == CART_TILT_Y_LEVEL);
  assert(read_backup(0x8300) & 0x80);                        /* ready */
  assert(!(read_backup(0x8500) & 0x80));
  gpsp_sensor_set(fe_sensor_tilt(0.0f, 0.0f));
  tilt_sample();
  assert(tilt_x_read() == CART_TILT_X_LEVEL && tilt_y_read() == CART_TILT_Y_LEVEL);
  /* Right side lowered half a g: lower X (GBATEK range 2AF-477). */
  gpsp_sensor_set(fe_sensor_tilt(0.5f, 0.0f));
  tilt_sample();
  printf("tilt right 0.5 g -> X %03X Y %03X\n", tilt_x_read(), tilt_y_read());
  assert(tilt_x_read() < CART_TILT_X_LEVEL - 80 && tilt_x_read() > 0x2AF - 20);
  assert(tilt_y_read() == CART_TILT_Y_LEVEL);
  gpsp_sensor_set(fe_sensor_tilt(0.0f, -1.0f));              /* top lowered */
  tilt_sample();
  assert(tilt_y_read() > CART_TILT_Y_LEVEL + 150 && tilt_y_read() <= 0x480 + 20);
  /* Only 0x55 then 0xAA samples; the registers keep the last sample. */
  gpsp_sensor_set(fe_sensor_tilt(-0.5f, 0.0f));
  write_backup(0x8100, 0xAA);
  assert(tilt_y_read() > CART_TILT_Y_LEVEL + 150);
  write_backup(0x8000, 0x54);
  write_backup(0x8100, 0xAA);
  assert(tilt_y_read() > CART_TILT_Y_LEVEL + 150);
  tilt_sample();
  assert(tilt_x_read() > CART_TILT_X_LEVEL + 80);
  /* The sensor's registers are not the save: EEPROM games still save. */
  backup_type = BACKUP_EEPROM;
  assert(read_backup(0x0000) == 0xFF);
  assert(read_backup(0x8200) == (u8)tilt_x);

  /* ---- save states -------------------------------------------------- */
  cart(CART_SENSOR_GYRO);
  gpsp_sensor_set(GPSP_SENSOR_VALID | 0x5A5);
  write_gpio(0xC8, 1);
  write_gpio(0xC6, 0x0B);
  port(3); port(2);
  for (i = 0; i < 6; i++) { port(0); port(2); }   /* six bits out */
  n = save_state(state, sizeof(state));
  assert(bson_find_key(state, "backup") &&
         bson_find_key(bson_find_key(state, "backup"), "sensor"));
  gyro_shift = 0; gyro_clock_high = 0;
  assert(memory_read_savestate(state));
  {
    unsigned v = 0;
    for (i = 0; i < 10; i++)
    {
      v = (v << 1) | ((pins() >> 2) & 1);
      port(0); port(2);
    }
    assert(v == (0x5A5u & 0x3FF));          /* the last ten bits of 0x05A5 */
  }

  /* A state with a damaged sensor array keeps a freshly reset sensor. */
  {
    u32 w[CART_SENSOR_WORDS] = { CART_SENSOR_GYRO, 0, 0, 0xE8, 0x1FFFF, 0, 0, 0 };
    gyro_shift = 0x1234;
    cart_sensor_state_load(w);
    assert(gyro_shift == 0);
    w[0] = CART_SENSOR_TILT; w[4] = 0;       /* another game's state */
    gyro_shift = 0x1234;
    cart_sensor_state_load(w);
    assert(gyro_shift == 0);
  }

  /* A game without a sensor writes no "sensor" key: its states are the
   * same bytes as before this change. */
  cart(0);
  n = save_state(state2, sizeof(state2));
  assert(!bson_find_key(bson_find_key(state2, "backup"), "sensor"));
  /* ... and a sensor state loads into a game without one (the key is
   * ignored). */
  assert(memory_read_savestate(state));

  puts("cart sensors: ok");
  return 0;
}
