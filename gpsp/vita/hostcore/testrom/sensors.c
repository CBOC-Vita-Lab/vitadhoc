/* sensors.c -- hostcore/testrom: reads a sensor cartridge's sensors the way
 * the games do and draws what it read, for app_tests.py's sensor tests
 * (GPL-2.0-or-later, like the rest of gpsp/vita).
 *
 * The same program is built three times with a sensor game's code in the
 * header (Makefile: sensors), so the emulator gives it that cartridge's
 * hardware: KYGE (tilt), RZWE (gyro), U3IE (light).  Every frame it reads
 * all three -- the ones the cartridge does not have read as nothing in
 * particular -- with GBATEK's routines, and draws four bars on black, each
 * starting at x = 0, ten lines high:
 *
 *   y  20  tilt X / 8      (0x392 level -> 114 px)
 *   y  40  tilt Y / 8      (0x3A0 level -> 116 px)
 *   y  60  gyro / 8        (0x6C0 still -> 216 px)
 *   y  80  light counter   (0xE8 dark -> 232 px; full sun -> 49 px)
 *
 * so a screenshot's bar lengths are the values the cartridge gave. */
typedef unsigned char  u8;
typedef unsigned short u16;
typedef unsigned int   u32;

#define REG16(a) (*(volatile u16 *)(0x04000000u + (a)))
#define VRAM     ((volatile u16 *)0x06000000u)
#define CART16(a) (*(volatile u16 *)(0x08000000u + (a)))
#define SRAM     ((volatile u8 *)0x0E000000u)
#define RGB(r, g, b) ((u16)((r) | ((g) << 5) | ((b) << 10)))

volatile u32 g_frames;                       /* counted by crt0's handler */
const char backup_id[] = "EEPROM_V124";

static void vblank_wait(void)
{
   __asm__ volatile("swi 5" : : : "r0", "r1", "r2", "r3", "memory");
}

static void bar(int y, int w, u16 c)
{
   int i, j;
   if (w > 240) w = 240;
   if (w < 0) w = 0;
   for (j = y; j < y + 10; j++)
      for (i = 0; i < 240; i++)
         VRAM[j * 240 + i] = i < w ? c : 0;
}

/* GBATEK, GBA Cart Tilt Sensor: wait ready, read, start the next. */
static void tilt(u32 *x, u32 *y)
{
   int t;
   for (t = 0; t < 1000 && !(SRAM[0x8300] & 0x80); t++)
      ;
   *x = ((SRAM[0x8300] & 0x0F) << 8) | SRAM[0x8200];
   *y = ((SRAM[0x8500] & 0x0F) << 8) | SRAM[0x8400];
   SRAM[0x8000] = 0x55;
   SRAM[0x8100] = 0xAA;
}

/* GBATEK, GBA Cart Gyro Sensor, read_gyro. */
static u32 gyro(void)
{
   u32 v = 0, r2, i;
   CART16(0xC8) = 1;
   CART16(0xC6) = 0x0B;
   r2 = CART16(0xC4) | 3;
   CART16(0xC4) = r2;
   r2 &= ~1u;
   CART16(0xC4) = r2;
   r2 &= ~2u;
   for (i = 0; i < 16; i++)
   {
      u32 d = CART16(0xC4);
      CART16(0xC4) = r2;
      v = (v << 1) | ((d >> 2) & 1);
      CART16(0xC4) = r2 | 2;
   }
   return v & 0xFFF;
}

/* GBATEK, GBA Cart Solar Sensor, example code. */
static u32 solar(void)
{
   u32 r0 = 0;
   CART16(0xC8) = 1;
   CART16(0xC6) = 7;
   CART16(0xC4) = 2;
   CART16(0xC4) = 0;
   for (;;)
   {
      CART16(0xC4) = 1;
      CART16(0xC4) = 0;
      if (CART16(0xC4) & 8)
         break;
      if (++r0 & 0x100)
         break;
   }
   return r0;
}

int main(void)
{
   REG16(0x00) = 0x0403;                     /* mode 3, BG2 */
   bar(0, 0, 0);
   REG16(0x04) = 0x0008;                     /* vblank IRQ */
   REG16(0x200) = 0x0001;
   REG16(0x208) = 1;
   for (;;)
   {
      u32 x, y, g, s;
      vblank_wait();
      tilt(&x, &y);
      g = gyro();
      s = solar();
      bar(20, (int)(x / 8), RGB(31, 8, 8));
      bar(40, (int)(y / 8), RGB(8, 31, 8));
      bar(60, (int)(g / 8), RGB(8, 8, 31));
      bar(80, (int)s, RGB(31, 31, 8));
   }
}
