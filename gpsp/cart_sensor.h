/* SENSOR CARTRIDGES (included by gba_memory.c only).
 *
 * Three kinds of GBA cartridge carry a sensor (GBATEK "GBA Cart Solar /
 * Gyro / Tilt Sensor"; game list in gba_over.h):
 *
 *   SOLAR  Boktai 1-3.  A photodiode read through the GPIO port: bit 1 high
 *          resets an 8-bit counter, each rising edge of bit 0 counts one up,
 *          and bit 3 reads 1 once the counter has reached the light level
 *          (0xE8 is darkness, ~0x50 fills Boktai's sun gauge).  Bit 2 high
 *          selects the cartridge clock instead (the two share the port).
 *   GYRO   WarioWare: Twisted!.  A rotation-rate sensor behind a serial ADC
 *          on the GPIO port: bit 0 high starts a conversion, bit 2 shifts
 *          out the 16-bit result MSB first on each falling edge of bit 1
 *          (4 zero bits, then 12 data bits; 0x6C0 = still, higher =
 *          clockwise).  Bit 3 is the cartridge's rumble motor.
 *   TILT   Yoshi Topsy-Turvy, Koro Koro Puzzle.  A two-axis accelerometer
 *          mapped into the upper half of the SRAM window: 0x55 to E008000
 *          then 0xAA to E008100 samples it; E008200-E008500 read X and Y
 *          (12 bits each, bit 7 of E008300 = ready).
 *
 * The frontend publishes one reading per frame with gpsp_sensor_set(), in
 * the cartridge's own units (frontend-common/fe_sensor.h converts from the
 * console's sensors).  The value is held for the whole frame, so a frame
 * is a function of its input: the rewind records it beside the buttons and
 * gives it back when the frame is run again.  Nothing published (0) reads
 * as a still console in a dark room, which is also what the second machine
 * of a link session sees.
 *
 * The sensor's protocol state (counter, shift register, latched sample)
 * rides in save states as the optional "sensor" array, written only for a
 * sensor cartridge: every other game's states are byte-for-byte unchanged
 * and older states still load. */

extern u8 gpio_regs[3];

#define CART_SENSOR_SOLAR  GPSP_SENSOR_SOLAR
#define CART_SENSOR_GYRO   GPSP_SENSOR_GYRO
#define CART_SENSOR_TILT   GPSP_SENSOR_TILT

/* Cartridge units of a still console (GBATEK's example values). */
#define CART_GYRO_STILL    0x6C0
#define CART_TILT_X_LEVEL  0x392
#define CART_TILT_Y_LEVEL  0x3A0

static u32 cart_sensors;            /* CART_SENSOR_* the loaded game has   */
static u32 cart_sensor_frame;       /* this frame's published reading      */

/* Solar */
static u32 solar_counter;           /* the 74LV4040's low 8 bits (+carry)  */
static u32 solar_edge;              /* the clock was low: next high counts */
static u32 solar_level = 0xE8;      /* comparator threshold, latched at reset */
/* Gyro */
static u32 gyro_shift;              /* 16-bit result being shifted out     */
static u32 gyro_clock_high;
/* Tilt */
static u32 tilt_x = 0xFFF, tilt_y = 0xFFF;
static u32 tilt_armed;              /* 0x55 written, waiting for 0xAA      */

/* The light levels a frontend offers, darkest to brightest: 0 is darkness
 * (counter threshold 0xE9), 10 is full sun (0x4E; Boktai's gauge is full
 * from about 0x50).  The steps are mGBA's, so a level means the same in
 * both. */
static const u8 cart_solar_steps[10] = { 5, 11, 18, 27, 42, 62, 84, 109, 139, 183 };

u32 gpsp_sensor_kinds(void)
{
  return cart_sensors;
}

void gpsp_sensor_set(u32 reading)
{
  cart_sensor_frame = reading;
}

u32 gpsp_sensor_get(void)
{
  return cart_sensor_frame;
}

static u32 cart_solar_threshold(void)
{
  u32 level = cart_sensor_frame & GPSP_SENSOR_VALID
      ? (cart_sensor_frame & 0xF) : 0;
  if (level > 10)
    level = 10;
  return 0xFF - (0x16 + (level ? cart_solar_steps[level - 1] : 0));
}

static u32 cart_gyro_reading(void)
{
  return cart_sensor_frame & GPSP_SENSOR_VALID
      ? (cart_sensor_frame & 0xFFF) : CART_GYRO_STILL;
}

static void cart_sensor_reset(void)
{
  cart_sensor_frame = 0;
  solar_counter = 0;
  solar_edge = 0;
  solar_level = 0xE8;
  gyro_shift = 0;
  gyro_clock_high = 0;
  tilt_x = tilt_y = 0xFFF;
  tilt_armed = 0;
}

/* Drive the input pins (those the game set as inputs) to `pins`. */
static void cart_sensor_output(u32 pins, u32 mask)
{
  u32 in = (u32)~gpio_regs[1] & mask;
  gpio_regs[0] = (u8)((gpio_regs[0] & ~in) | (pins & in));
}

/* A write to the GPIO data register: the pins as the game left them. */
static void cart_sensor_pins(void)
{
  u32 pins = gpio_regs[0];

  if (cart_sensors & CART_SENSOR_SOLAR)
  {
    if (!(pins & 4))            /* bit 2 high: the clock chip's turn */
    {
      if (pins & 2)
      {
        solar_counter = 0;
        solar_edge = 1;
        solar_level = cart_solar_threshold();
      }
      if ((pins & 1) && solar_edge && solar_counter < 0xFFF)
        solar_counter++;
      solar_edge = !(pins & 1);
      cart_sensor_output(solar_counter >= solar_level ? 8 : 0, 8);
    }
  }

  if (cart_sensors & CART_SENSOR_GYRO)
  {
    u32 shift = gyro_clock_high && !(pins & 2);    /* falling edge */
    if (pins & 1)
    {
      gyro_shift = cart_gyro_reading();
      shift = 1;
    }
    if (shift)
    {
      cart_sensor_output(((gyro_shift >> 15) & 1) << 2, 4);
      gyro_shift = (gyro_shift << 1) & 0xFFFF;
    }
    gyro_clock_high = !!(pins & 2);
  }
}

/* E008000-E0085FF of a tilt cartridge.  Return 1 when the access was the
 * sensor's (the SRAM window is otherwise unused: these games save to
 * EEPROM). */
static int cart_tilt_write(u32 address, u32 value)
{
  if (!(cart_sensors & CART_SENSOR_TILT) || (address & 0xF000) != 0x8000)
    return 0;
  if ((address & 0xFF00) == 0x8000)
    tilt_armed = (value & 0xFF) == 0x55;
  else if ((address & 0xFF00) == 0x8100)
  {
    if ((value & 0xFF) == 0xAA && tilt_armed)
    {
      if (cart_sensor_frame & GPSP_SENSOR_VALID)
      {
        tilt_x = cart_sensor_frame & 0xFFF;
        tilt_y = (cart_sensor_frame >> 12) & 0xFFF;
      }
      else
      {
        tilt_x = CART_TILT_X_LEVEL;
        tilt_y = CART_TILT_Y_LEVEL;
      }
    }
    tilt_armed = 0;
  }
  return 1;
}

static int cart_tilt_read(u32 address, u8 *value)
{
  if (!(cart_sensors & CART_SENSOR_TILT) || (address & 0xF000) != 0x8000)
    return 0;
  switch (address & 0xFF00)
  {
    case 0x8200: *value = (u8)tilt_x; break;
    case 0x8300: *value = (u8)(((tilt_x >> 8) & 0xF) | 0x80); break;
    case 0x8400: *value = (u8)tilt_y; break;
    case 0x8500: *value = (u8)((tilt_y >> 8) & 0xF); break;
    default:     *value = 0xFF; break;
  }
  return 1;
}

/* Save states: [kinds, solar counter, solar edge, solar level, gyro shift,
 * gyro clock, tilt x | y << 16, tilt armed]. */
#define CART_SENSOR_WORDS 8

static void cart_sensor_state_words(u32 *w)
{
  w[0] = cart_sensors;
  w[1] = solar_counter;
  w[2] = solar_edge;
  w[3] = solar_level;
  w[4] = gyro_shift;
  w[5] = gyro_clock_high;
  w[6] = (tilt_x & 0xFFFF) | ((tilt_y & 0xFFFF) << 16);
  w[7] = tilt_armed;
}

/* A state of another game, or damaged, keeps nothing (the cartridge just
 * looks freshly reset); absent from older states, the same. */
static void cart_sensor_state_load(const u32 *w)
{
  u32 frame = cart_sensor_frame;
  cart_sensor_reset();
  cart_sensor_frame = frame;
  if (!w || w[0] != cart_sensors || w[1] > 0xFFF || w[2] > 1 ||
      w[3] > 0xFF || w[4] > 0xFFFF || w[5] > 1 || w[7] > 1 ||
      (w[6] & 0xFFFF) > 0xFFF || (w[6] >> 16) > 0xFFF)
    return;
  solar_counter = w[1];
  solar_edge = w[2];
  solar_level = w[3];
  gyro_shift = w[4];
  gyro_clock_high = w[5];
  tilt_x = w[6] & 0xFFFF;
  tilt_y = w[6] >> 16;
  tilt_armed = w[7];
}
