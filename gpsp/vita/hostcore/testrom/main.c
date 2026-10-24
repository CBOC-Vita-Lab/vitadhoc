/* main.c -- hostcore/testrom: a small GBA program written for this port's
 * tests (GPL-2.0-or-later, like the rest of gpsp/vita).
 *
 * The public test ROMs prove the CPU cores decode instructions.  This one
 * exercises what a GAME does to an emulator, and draws the result so that a
 * screenshot and the sound stream are the test's output:
 *
 *   - Thumb code calling ARM code and back; a vertical-blank interrupt
 *     through the BIOS (the built-in open BIOS); BIOS calls (VBlankIntrWait,
 *     Div)
 *   - code copied to RAM and executed there, then PATCHED and executed
 *     again every frame: the dynarec's RAM translation cache and its
 *     self-modifying-code flush
 *   - sound: a square-wave channel changing pitch, and a Direct Sound
 *     channel fed by timer 0 and FIFO DMA
 *   - the buttons: each lights a box (the pad path, the host driver's
 *     script and the frontend's chords)
 *   - battery RAM: every press of A writes a new pattern, which the
 *     frontend must put in saves/<rom>.sav
 */
typedef unsigned char  u8;
typedef unsigned short u16;
typedef unsigned int   u32;
typedef signed char    s8;

#define REG16(a) (*(volatile u16 *)(0x04000000u + (a)))
#define REG32(a) (*(volatile u32 *)(0x04000000u + (a)))
#define VRAM     ((volatile u16 *)0x06000000u)
#define SRAM     ((volatile u8 *)0x0E000000u)
#define EWRAM    ((s8 *)0x02000000u)
#define RGB(r, g, b) ((u16)((r) | ((g) << 5) | ((b) << 10)))

volatile u32 g_frames;                       /* counted by crt0's handler */
const char backup_id[] = "SRAM_V113";        /* what save-type scans look for */

static u16 ramcode[4];                       /* Thumb: movs r0, #imm ; bx lr */

static void rect(int x, int y, int w, int h, u16 c)
{
   int i, j;
   for (j = y; j < y + h; j++)
      for (i = x; i < x + w; i++)
         VRAM[j * 240 + i] = c;
}

static int bios_div(int num, int den)
{
   register int r0 __asm__("r0") = num;
   register int r1 __asm__("r1") = den;
   __asm__ volatile("swi 6" : "+r"(r0), "+r"(r1) : : "r3", "memory");
   return r0;
}

static void vblank_wait(void)
{
   __asm__ volatile("swi 5" : : : "r0", "r1", "r2", "r3", "memory");
}

/* ARM state, so every call from main (Thumb) is an interworking call. */
__attribute__((target("arm"), noinline))
static u32 mix_arm(u32 a, u32 b)
{
   return (a * 0x9E3779B1u) ^ (b >> 3) ^ (b << 7);
}

static void sound_start(void)
{
   int i;
   /* one second of a 256 Hz triangle whose loudness swells: 16384 samples */
   for (i = 0; i < 16384 + 64; i++)
   {
      int ph = i & 63, tri = ph < 32 ? ph * 4 - 64 : 190 - ph * 4;
      EWRAM[i] = (s8)((tri * ((i >> 6) & 127)) >> 7);
   }
   REG16(0x84) = 0x0080;                     /* master enable            */
   REG16(0x80) = 0x1177;                     /* square 1 to both sides   */
   REG16(0x82) = 0x0B06;                     /* PSG 100 %, DS A 100 % L+R,
                                              * timer 0, FIFO reset      */
   REG16(0x60) = 0x0000;
   REG16(0x62) = 0xF080;                     /* volume 15, 50 % duty     */
   REG16(0x100) = 0xFC00;                    /* timer 0: 16384 Hz        */
   REG16(0x102) = 0x0080;
}

static void sound_frame(u32 f)
{
   if ((f % 30) == 0)                        /* a new note every half second */
      REG16(0x64) = (u16)(0x8000 | (1700 + ((f / 30) % 8) * 30));
   if ((f % 60) == 0)                        /* re-arm the FIFO DMA at the
                                              * start of the sample        */
   {
      REG16(0xC6) = 0;
      REG32(0xBC) = (u32)EWRAM;
      REG32(0xC0) = 0x040000A0u;
      REG16(0xC6) = 0xB640;                  /* enable, FIFO, 32-bit,
                                              * repeat, fixed destination */
   }
}

int main(void)
{
   static const u16 keycol[10] = {
      RGB(31, 0, 0), RGB(31, 16, 0), RGB(20, 20, 20), RGB(31, 31, 31),
      RGB(0, 31, 0), RGB(0, 31, 31), RGB(0, 0, 31), RGB(31, 0, 31),
      RGB(31, 31, 0), RGB(16, 31, 16) };
   u32 presses = 0, last_keys = 0, acc = 1;
   int (*ramfn)(void) = (int (*)(void))((u32)ramcode | 1);
   int i;

   REG16(0x00) = 0x0403;                     /* mode 3, BG2 */
   rect(0, 0, 240, 160, RGB(2, 2, 6));
   REG16(0x04) = 0x0008;                     /* vblank IRQ */
   REG16(0x200) = 0x0001;
   REG16(0x208) = 1;
   sound_start();

   ramcode[0] = 0x2000;
   ramcode[1] = 0x4770;

   for (;;)
   {
      u32 f, keys;
      int v, q;

      vblank_wait();
      f = g_frames;
      keys = ~REG16(0x130) & 0x3FF;
      sound_frame(f);

      /* a bar that crosses the screen: the picture moves every frame */
      rect(0, 8, 240, 8, RGB(2, 2, 6));
      rect((int)(f % 224), 8, 16, 8, RGB(31, 31, 31));

      /* the ten buttons */
      for (i = 0; i < 10; i++)
         rect(8 + i * 23, 28, 18, 18,
              (keys >> i) & 1 ? keycol[i] : RGB(6, 6, 6));

      /* RAM code, patched every frame: the box takes the value it returns */
      ramcode[0] = (u16)(0x2000 | (f & 0x1F));
      v = ramfn();
      rect(8, 58, 40, 20, RGB(v, 31 - v, 8));

      /* BIOS Div and an ARM helper, folded into a running value */
      q = bios_div((int)f, 7);
      acc = mix_arm(acc, f + (u32)q);
      rect(60, 58, 40, 20, RGB(q & 31, (acc >> 5) & 31, (acc >> 11) & 31));

      /* A pressed: a new pattern in battery RAM; the box shows what reads
       * back */
      if ((keys & 1) && !(last_keys & 1))
      {
         presses++;
         for (i = 0; i < 64; i++)
            SRAM[i] = (u8)(i * 7 + presses * 16);
      }
      rect(112, 58, 40, 20, RGB(SRAM[1] & 31, SRAM[2] & 31, SRAM[3] & 31));

      /* the frame counter in binary: a screenshot says which frame it is */
      for (i = 0; i < 16; i++)
         rect(8 + i * 14, 100, 10, 10,
              (f >> (15 - i)) & 1 ? RGB(31, 31, 31) : RGB(6, 6, 6));
      last_keys = keys;
   }
   return 0;
}
