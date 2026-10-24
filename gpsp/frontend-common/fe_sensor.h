/* SENSOR CARTRIDGES: a console's own sensors in the cartridge's units.
 *
 * The emulated cartridges (gba_memory.h GPSP_SENSOR_*, cart_sensor.h; the
 * Game Boy's MBC7 in gbcore) read a still console as GBATEK and Pan Docs
 * give it, and respond to the console the way the real cartridge responds
 * to the console it is plugged into:
 *
 *   gyro (WarioWare: Twisted!)  rate of turn about the axis through the
 *        screen.  GBATEK's samples: 0x6C0 still, 0x73A a normal fast turn
 *        clockwise, 0x9E3 a sharp one -- about 55 units per rad/s.
 *   tilt (Yoshi Topsy-Turvy)    gravity along the screen's X and Y.  GBATEK:
 *        X 0x2AF..0x477 around 0x392, Y 0x2C3..0x480 around 0x3A0, i.e.
 *        about 225 units per g; lower X with the right side down, lower Y
 *        with the bottom down (the ADXL202 the MBC7 also uses; Pan Docs).
 *   MBC7 (Kirby Tilt 'n' Tumble) 16-bit, 0x81D0 level, 0x70 per g, the same
 *        directions (Pan Docs, MBC7).
 *   solar (Boktai)              a light level 0 (dark) .. 10 (full sun).
 *
 * Inputs: rate_z in rad/s, positive anticlockwise as the player looks at
 * the screen (the right-hand rule about an axis pointing at the player);
 * right_g / down_g = how much of gravity points to the screen's right /
 * bottom edge (0 = level, 1 = that edge straight down).  Pure functions:
 * the core and its tests use them as they are. */
#ifndef FE_SENSOR_H
#define FE_SENSOR_H

#include <stdint.h>

#define FE_SENSOR_VALID       0x80000000u
#define FE_GYRO_STILL         0x6C0
#define FE_GYRO_PER_RAD       55.0f
#define FE_TILT_X_LEVEL       0x392
#define FE_TILT_Y_LEVEL       0x3A0
#define FE_TILT_PER_G         225.0f
#define FE_MBC7_LEVEL         0x81D0
#define FE_MBC7_PER_G         112.0f      /* 0x70 */
#define FE_SOLAR_LEVELS       11          /* 0 .. 10 */

static inline int fe_sensor_round(float v)
{
   return (int)(v < 0.0f ? v - 0.5f : v + 0.5f);
}

static inline int fe_sensor_clamp(int v, int lo, int hi)
{
   return v < lo ? lo : v > hi ? hi : v;
}

static inline uint32_t fe_sensor_solar(int level)
{
   return FE_SENSOR_VALID | (uint32_t)fe_sensor_clamp(level, 0, FE_SOLAR_LEVELS - 1);
}

static inline uint32_t fe_sensor_gyro(float rate_z)
{
   int v = FE_GYRO_STILL - fe_sensor_round(rate_z * FE_GYRO_PER_RAD);
   return FE_SENSOR_VALID | (uint32_t)fe_sensor_clamp(v, 0, 0xFFF);
}

static inline uint32_t fe_sensor_tilt(float right_g, float down_g)
{
   int x = FE_TILT_X_LEVEL - fe_sensor_round(right_g * FE_TILT_PER_G);
   int y = FE_TILT_Y_LEVEL - fe_sensor_round(down_g * FE_TILT_PER_G);
   return FE_SENSOR_VALID | (uint32_t)fe_sensor_clamp(x, 0, 0xFFF) |
          ((uint32_t)fe_sensor_clamp(y, 0, 0xFFF) << 12);
}

/* The MBC7's two 16-bit values packed as x | y << 16. */
static inline uint32_t fe_sensor_mbc7(float right_g, float down_g)
{
   int x = FE_MBC7_LEVEL - fe_sensor_round(right_g * FE_MBC7_PER_G);
   int y = FE_MBC7_LEVEL - fe_sensor_round(down_g * FE_MBC7_PER_G);
   return (uint32_t)fe_sensor_clamp(x, 0, 0xFFFF) |
          ((uint32_t)fe_sensor_clamp(y, 0, 0xFFFF) << 16);
}

#endif
