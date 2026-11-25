/* GAME BOY CAMERA (cartridge type 0xFC, "Pocket Camera"): the cartridge's
 * register bank and its M64282FP image sensor, as the game uses them.
 *
 * Included by mbc.c only (the core's own object list stays as it is); a host
 * test may include it too, for the pure picture functions.  Written for this
 * project from the public hardware documentation (Pan Docs, "Game Boy
 * Camera"); no code from another emulator.
 *
 * What the cartridge does
 *   - Like an MBC3: ROM bank 0x2000 (0 is allowed here), RAM bank 0x4000.
 *     A RAM bank number with bit 4 set puts the CAMERA REGISTERS at A000
 *     (mirrored every 0x80 bytes) instead of cartridge RAM.
 *   - A000: bit 0 starts a capture and reads back 1 while it runs; bits 1
 *     and 2 choose the sensor's 1-D filter.  A001..A005 set the sensor
 *     (gain, edge mode, exposure, edge ratio and invert); A006..A035 are the
 *     4x4 dither matrix, three thresholds per cell.  Everything but A000
 *     reads 0.
 *   - A capture takes 32446 + (N bit clear: 512) + 16 * exposure machine
 *     cycles; the picture then appears at cartridge RAM bank 0, 0x0100: 14
 *     rows of 16 tiles, 128x112 pixels, 2 bits each.  RAM keeps the previous
 *     picture until then.
 *
 * What is modelled and what is not
 *   The picture is the frontend's 128x112 greyscale (gbcore_set_camera_image;
 *   a test pattern when none was given).  Exposure, gain, invert, the edge
 *   modes (1-D, horizontal / vertical, 2-D) and the dither matrix follow the
 *   documentation.  The sensor's analogue behaviour (reference voltage,
 *   zero-point calibration, infrared response, noise) is not modelled, so a
 *   real camera's picture differs in detail; the exposure scale is an
 *   estimate (the 20 dB gain at exposure 0x1000 passes the source through).
 *   RAM reads during a capture are not blanked.  A capture lasts the same
 *   number of scanlines in double speed. */
#ifndef GBCAM_HDR
#define GBCAM_HDR

#include <stdint.h>
#include <string.h>

#define GBCAM_W        128
#define GBCAM_H        112
#define GBCAM_REGS     0x36
#define GBCAM_IMG_OFF  0x100
#define GBCAM_IMG_SIZE 0xE00          /* 14 x 16 tiles x 16 bytes */
#define GBCAM_LINE_CYC 114            /* machine cycles per scanline */
#define GBCAM_STATE_SIZE 64

/* Gain steps of register A001 bits 0..3 in dB (G4 adds 6 dB), as Q8 factors
 * relative to 20 dB. */
static const uint16_t gbcam_gain_q8[16] = {
   128, 152, 181, 215, 256, 304, 362, 430,
   511, 722, 1019, 1440, 2033, 2872, 4822, 9621
};
/* Register A004 bits 4..6: edge enhancement ratio x 256. */
static const uint16_t gbcam_edge_q8[8] = {
   128, 192, 256, 320, 512, 768, 1024, 1280
};

static inline int gbcam_clampi(int v, int lo, int hi)
{
   return v < lo ? lo : v > hi ? hi : v;
}

/* The picture a camera with nothing to look at shows: a diagonal ramp with
 * a bright disc, so every exposure setting has something to work on. */
static inline uint8_t gbcam_pattern_pixel(int x, int y)
{
   int dx = x - 64, dy = y - 56;
   int v = (x * 2 + y * 2) * 255 / (2 * (GBCAM_W + GBCAM_H));
   if (dx * dx + dy * dy < 30 * 30)
      v = 235;
   return (uint8_t)gbcam_clampi(v, 0, 255);
}

/* The sensor and the controller: `src` is 128x112 greyscale (NULL = the test
 * pattern), `reg` the register bank (A000..A035).  `out` receives the 3584
 * bytes the cartridge writes to RAM (14x16 tiles, 2bpp planar). */
static inline void gbcam_render(const uint8_t *src, const uint8_t *reg,
                                uint8_t *out)
{
   static int16_t a[GBCAM_H][GBCAM_W], b[GBCAM_H][GBCAM_W];
   const unsigned gain = (unsigned)gbcam_gain_q8[reg[1] & 0x0F] *
                         ((reg[1] & 0x10) ? 2u : 1u);
   const unsigned expo = ((unsigned)reg[2] << 8) | reg[3];
   const int nbit = (reg[1] >> 7) & 1, vh = (reg[1] >> 5) & 3;
   const int alpha = gbcam_edge_q8[(reg[4] >> 4) & 7];
   const int invert = (reg[4] >> 3) & 1;
   const int pm = (reg[0] >> 1) & 3;
   int x, y;

   /* exposure and gain, then invert; signed around the mid level */
   for (y = 0; y < GBCAM_H; y++)
      for (x = 0; x < GBCAM_W; x++)
      {
         unsigned p = src ? src[y * GBCAM_W + x] : gbcam_pattern_pixel(x, y);
         uint64_t v = ((uint64_t)p * gain * expo) >> 20;
         int s = v > 255 ? 255 : (int)v;
         if (invert)
            s = 255 - s;
         a[y][x] = (int16_t)(s - 128);
      }

   /* edge enhancement: the neighbours of a cell are clamped at the edge */
   if (nbit && vh == 3)
   {
      for (y = 0; y < GBCAM_H; y++)
         for (x = 0; x < GBCAM_W; x++)
         {
            int px = a[y][x];
            int w = a[y][x > 0 ? x - 1 : 0], e = a[y][x < GBCAM_W - 1 ? x + 1 : x];
            int n = a[y > 0 ? y - 1 : 0][x], s = a[y < GBCAM_H - 1 ? y + 1 : y][x];
            b[y][x] = (int16_t)gbcam_clampi(px + (((4 * px - w - e - n - s) * alpha) >> 8), -128, 127);
         }
      memcpy(a, b, sizeof(a));
   }
   else if (!nbit && vh)
   {
      for (y = 0; y < GBCAM_H; y++)
         for (x = 0; x < GBCAM_W; x++)
         {
            int px = a[y][x], d = 0;
            if (vh & 1)
               d += 2 * px - a[y][x > 0 ? x - 1 : 0] -
                    a[y][x < GBCAM_W - 1 ? x + 1 : x];
            if (vh & 2)
               d += 2 * px - a[y > 0 ? y - 1 : 0][x] -
                    a[y < GBCAM_H - 1 ? y + 1 : y][x];
            b[y][x] = (int16_t)gbcam_clampi(px + ((d * alpha) >> 8), -128, 127);
         }
      memcpy(a, b, sizeof(a));
   }

   /* the 1-D filter (A000 bits 1..2): P adds, M subtracts the pixel (bit 0)
    * and the one below it (bit 1); 0b01 = the plain picture */
   {
      const int pb = pm == 0 ? 0 : 1, mb = pm == 0 ? 1 : pm >= 2 ? 2 : 0;
      for (y = 0; y < GBCAM_H; y++)
         for (x = 0; x < GBCAM_W; x++)
         {
            int px = a[y][x], ms = a[y < GBCAM_H - 1 ? y + 1 : y][x], v = 0;
            if (pb & 1) v += px;
            if (pb & 2) v += ms;
            if (mb & 1) v -= px;
            if (mb & 2) v -= ms;
            b[y][x] = (int16_t)gbcam_clampi(v, -128, 127);
         }
   }

   /* the dither matrix turns each cell's level into one of four shades */
   memset(out, 0, GBCAM_IMG_SIZE);
   for (y = 0; y < GBCAM_H; y++)
      for (x = 0; x < GBCAM_W; x++)
      {
         const uint8_t *t = reg + 6 + ((y & 3) * 4 + (x & 3)) * 3;
         int v = b[y][x] + 128;
         int level = v < t[0] ? 0 : v < t[1] ? 1 : v < t[2] ? 2 : 3;
         int col = 3 - level;
         uint8_t *tile = out + ((y >> 3) * 16 + (x >> 3)) * 16 + (y & 7) * 2;
         if (col & 1) tile[0] |= (uint8_t)(0x80 >> (x & 7));
         if (col & 2) tile[1] |= (uint8_t)(0x80 >> (x & 7));
      }
}

/* Machine cycles one capture takes with these registers. */
static inline unsigned gbcam_capture_cycles(const uint8_t *reg)
{
   unsigned expo = ((unsigned)reg[2] << 8) | reg[3];
   return 32446u + ((reg[1] & 0x80) ? 0u : 512u) + 16u * expo;
}

/* ------------------------------------------------------ cartridge state */

static uint8_t  gbcam_reg[GBCAM_REGS];
static uint8_t  gbcam_latch[GBCAM_REGS];   /* the registers at the start */
static int      gbcam_busy;                /* scanlines left, 0 = idle */

static inline void gbcam_reset(void)
{
   memset(gbcam_reg, 0, sizeof(gbcam_reg));
   memset(gbcam_latch, 0, sizeof(gbcam_latch));
   gbcam_busy = 0;
}

static inline uint8_t gbcam_read(unsigned adr)
{
   if ((adr & 0x7F) == 0)
      return (uint8_t)((gbcam_reg[0] & 6) | (gbcam_busy ? 1 : 0));
   return 0;
}

static inline void gbcam_write(unsigned adr, uint8_t dat)
{
   unsigned r = adr & 0x7F;
   if (r == 0)
   {
      gbcam_reg[0] = (uint8_t)(dat & 7);
      if (dat & 1)
      {
         if (!gbcam_busy)
         {
            unsigned cyc = gbcam_capture_cycles(gbcam_reg);
            memcpy(gbcam_latch, gbcam_reg, sizeof(gbcam_latch));
            gbcam_busy = (int)((cyc + GBCAM_LINE_CYC - 1) / GBCAM_LINE_CYC);
         }
      }
      else
         gbcam_busy = 0;           /* writing 0 stops a capture */
   }
   else if (r < GBCAM_REGS)
      gbcam_reg[r] = dat;
}

/* One scanline went by.  Returns 1 on the line the picture is complete: the
 * caller then renders it into cartridge RAM. */
static inline int gbcam_line(void)
{
   if (gbcam_busy && --gbcam_busy == 0)
      return 1;
   return 0;
}

/* State block (GBCAM_STATE_SIZE bytes): magic, scanlines left, registers.
 * Whether A000 is the register bank is the mapper's ext_is_ram flag, which
 * the state keeps already. */
static inline void gbcam_save(uint8_t *out)
{
   memset(out, 0, GBCAM_STATE_SIZE);
   memcpy(out, "CAM1", 4);
   out[4] = (uint8_t)(gbcam_busy & 0xFF);
   out[5] = (uint8_t)((gbcam_busy >> 8) & 0xFF);
   memcpy(out + 8, gbcam_reg, GBCAM_REGS);
}

/* A block that is not ours (an older state, garbage) leaves the camera idle
 * with cleared registers; the busy count is bounded by the longest capture. */
static inline void gbcam_load(const uint8_t *in)
{
   gbcam_reset();
   if (memcmp(in, "CAM1", 4) != 0)
      return;
   memcpy(gbcam_reg, in + 8, GBCAM_REGS);
   gbcam_reg[0] &= 7;
   memcpy(gbcam_latch, gbcam_reg, sizeof(gbcam_latch));
   gbcam_busy = in[4] | (in[5] << 8);
   if (gbcam_busy > 9500)
      gbcam_busy = 9500;
   if (!(gbcam_reg[0] & 1) && gbcam_busy)
      gbcam_reg[0] |= 1;
}
#endif
