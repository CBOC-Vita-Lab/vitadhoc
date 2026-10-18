/* main.c -- the cheat test cartridge (GPL-2.0-or-later, like the rest of
 * gpsp/vita).  A "game" with the things cheat codes are written against,
 * at addresses the tests know (cheat_e2e.c):
 *
 *   0x02000000 u16 lives     5, minus one every eight frames down to 0
 *   0x02000004 u16 gold      0; only a code changes it
 *   0x02000006 u16 gold2     0; only a code changes it
 *   0x02000008 u16 trigger   0x1234 while A is held, else 0
 *   0x0200000C u32 big       0; only a code changes it
 *   0x02000010 u16 keys      KEYINPUT as the game read it
 *   0x02000020 u32 res[0]    rom_const()   5   (ROM code at 0x08002000)
 *   0x02000024 u32 res[1]    ram_func()    7   (RAM code at 0x03004000)
 *   0x02000028 u32 res[2]    rom_lit()     0x11223344 (pool at 0x08002008)
 *   0x0200002C u32 res[3]    frames
 *
 * and a picture of them: five blocks whose colours are the low four bits of
 * lives, gold, res[0], res[1] and res[2].
 */
typedef unsigned char  u8;
typedef unsigned short u16;
typedef unsigned int   u32;

#define REG16(a) (*(volatile u16 *)(0x04000000u + (a)))
#define VRAM     ((volatile u16 *)0x06000000u)
#define V16(a)   (*(volatile u16 *)(a))
#define V32(a)   (*(volatile u32 *)(a))
#define RGB(r, g, b) ((u16)((r) | ((g) << 5) | ((b) << 10)))

int rom_const(void);
int rom_lit(void);
extern const u16 ram_template[2];

static const u16 palette[16] = {
   RGB(0, 0, 0),    RGB(31, 0, 0),   RGB(0, 31, 0),   RGB(0, 0, 31),
   RGB(31, 31, 0),  RGB(31, 0, 31),  RGB(0, 31, 31),  RGB(31, 31, 31),
   RGB(16, 0, 0),   RGB(0, 16, 0),   RGB(0, 0, 16),   RGB(16, 16, 0),
   RGB(16, 0, 16),  RGB(0, 16, 16),  RGB(16, 16, 16), RGB(31, 16, 0)
};

static void block(int n, u32 value)
{
   int x, y;
   u16 c = palette[value & 15];
   for (y = 60; y < 100; y += 4)          /* every fourth line and column: */
      for (x = n * 48 + 4; x < n * 48 + 44; x += 4)   /* cheap, and enough */
         VRAM[y * 240 + x] = c;
}

static void vblank_wait(void)
{
   __asm__ volatile("swi 5" : : : "r0", "r1", "r2", "r3", "memory");
}

int main(void)
{
   int (*ram_func)(void) = (int (*)(void))0x03004001;   /* Thumb */
   u32 frames = 0;

   V16(0x03004000) = ram_template[0];
   V16(0x03004002) = ram_template[1];

   REG16(0x00) = 0x0403;                  /* mode 3, BG2 */
   REG16(0x04) = 0x0008;                  /* vertical-blank interrupt */
   REG16(0x200) = 0x0001;
   REG16(0x208) = 0x0001;

   V16(0x02000000) = 5;
   V16(0x02000004) = 0;
   V16(0x02000006) = 0;
   V32(0x0200000C) = 0;

   for (;;)
   {
      u16 keys;
      vblank_wait();
      frames++;
      keys = REG16(0x130);
      V16(0x02000010) = keys;
      V16(0x02000008) = (keys & 1) ? 0 : 0x1234;
      if ((frames & 7) == 0 && V16(0x02000000) > 0)
         V16(0x02000000)--;
      V32(0x02000020) = (u32)rom_const();
      V32(0x02000024) = (u32)ram_func();
      V32(0x02000028) = (u32)rom_lit();
      V32(0x0200002C) = frames;
      block(0, V16(0x02000000));
      block(1, V16(0x02000004));
      block(2, V32(0x02000020));
      block(3, V32(0x02000024));
      block(4, V32(0x02000028));
   }
}
