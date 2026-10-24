/* cpufix.c -- a test cartridge for the emulator fixes taken from gpSP in
 * September 2026 (app_tests.py, t_cpufix).  Each test writes one byte of
 * the result to the battery RAM, so the test reads the .sav the app leaves:
 *
 *   0x00..0x05  "CPUFIX"
 *   0x06        number of tests
 *   0x08 + n    test n: 1 = passed, 2 = failed, 0 = never reached
 *   0x20 + 4n   test n: the value it saw (little endian), for the log
 *
 * The backdrop turns green when everything passed, red otherwise.
 *
 *  0  code in VRAM runs (Thumb), and runs again after it is rewritten
 *  1  the same in ARM state
 *  2  BG0CNT reads back without its unused bit 13
 *  3  WININ reads back without its unused bits
 *  4  an invalid I/O location (0x136) reads 0 and keeps no write
 *  5  DMA3CNT_H reads back without its unused low bits
 *  6  SOUNDCNT_X keeps its master bit readable (and no stray bits)
 *  7  the Z flag survives I/O reads on both the direct and the slow path
 *     of the ARM dynarec's load stub (an ARM-state routine in ROM)
 *  8  ...and so does a cleared Z flag
 *  9  an MSR that sets the T bit carries on in Thumb at the next
 *     instruction (an ARM-state routine in ROM; packed start-up code such
 *     as an added intro on Pokemon Pinball: Ruby & Sapphire relies on it)
 * 10  code in palette RAM runs (Thumb), and runs again after it is
 *     rewritten (the same game's last trampoline sits at 0x050003F0)
 * 11  code in OAM runs (ARM), and runs again after it is rewritten
 *
 * "SRAM_V113" below makes the emulator give the cartridge battery SRAM. */
typedef unsigned char u8;
typedef unsigned short u16;
typedef unsigned int u32;
typedef volatile u8 vu8;
typedef volatile u16 vu16;
typedef volatile u32 vu32;

#define REG(a) (*(vu16 *)(0x04000000 + (a)))
#define SRAM ((vu8 *)0x0E000000)
#define NTESTS 12

volatile u32 g_frames;              /* crt0.S counts vertical blanks here */

const char save_tag[] __attribute__((used)) = "SRAM_V113";

static void result(int n, int ok, u32 seen)
{
   int k;
   SRAM[0x08 + n] = ok ? 1 : 2;
   for (k = 0; k < 4; k++)
      SRAM[0x20 + 4 * n + k] = (u8)(seen >> (8 * k));
}

typedef u32 (*fn_t)(u32);

/* Code written into VRAM (BG tile memory far from anything shown). */
#define VRAM_CODE ((vu16 *)0x06008000)

static u32 call_vram_thumb(u32 x)
{
   fn_t f = (fn_t)(0x06008000 | 1);
   return f(x);
}

static u32 call_vram_arm(u32 x)
{
   fn_t f = (fn_t)0x06008100;
   return f(x);
}

/* ARM-state routines in ROM: Z set (movs #0) or clear (movs #1), then two
 * I/O reads -- KEYINPUT (direct) and SOUND1CNT_L (0x060, the slow path) --
 * then a conditional move tells whether Z is still what it was.  Returns 1
 * when the flag survived. */
u32 flags_z_set(void);
u32 flags_z_clear(void);
__asm__(
   "  .arm\n"
   "  .align 2\n"
   "  .global flags_z_set\n"
   "  .type flags_z_set, %function\n"
   "flags_z_set:\n"
   "  ldr r1, =0x04000130\n"
   "  ldr r3, =0x04000060\n"
   "  movs r0, #0\n"
   "  ldrh r2, [r1]\n"
   "  ldrh r2, [r3]\n"
   "  moveq r0, #1\n"
   "  bx lr\n"
   "  .global flags_z_clear\n"
   "  .type flags_z_clear, %function\n"
   "flags_z_clear:\n"
   "  ldr r1, =0x04000130\n"
   "  ldr r3, =0x04000060\n"
   "  movs r0, #1\n"
   "  ldrh r2, [r1]\n"
   "  ldrh r2, [r3]\n"
   "  movne r0, #1\n"
   "  moveq r0, #0\n"
   "  bx lr\n"
   "  .ltorg\n"
   /* Returns r0 + 1, the add done in Thumb after the MSR switched state. */
   "  .global msr_to_thumb\n"
   "  .type msr_to_thumb, %function\n"
   "msr_to_thumb:\n"
   "  mrs r1, cpsr\n"
   "  orr r1, r1, #0x20\n"
   "  msr cpsr_c, r1\n"
   "  .thumb\n"
   "  add r0, #1\n"
   "  bx lr\n"
   "  .align 2\n");
u32 msr_to_thumb(u32 x);

/* Palette RAM: the last OBJ palette's last entries, never shown here.
 * OAM: object 96 onwards, with no object layer on. */
static u32 call_pal_thumb(u32 x)
{
   fn_t f = (fn_t)(0x050003F0 | 1);
   return f(x);
}

static u32 call_oam_arm(u32 x)
{
   fn_t f = (fn_t)0x07000300;
   return f(x);
}

int main(void)
{
   u32 a, b, v;
   int i, all = 1;

   REG(0x000) = 0x0000;              /* mode 0, nothing on: the backdrop */
   *(vu16 *)0x05000000 = 0x7FFF;      /* white while running */
   for (i = 0; i < 6; i++)
      SRAM[i] = (u8)"CPUFIX"[i];
   SRAM[6] = NTESTS;
   for (i = 0; i < NTESTS; i++)
      SRAM[0x08 + i] = 0;

   /* 0: Thumb in VRAM -- "adds r0, #1; bx lr", then "adds r0, #2". */
   VRAM_CODE[0] = 0x3001;
   VRAM_CODE[1] = 0x4770;
   for (a = 0, i = 0; i < 100; i++)
      a = call_vram_thumb(a);
   VRAM_CODE[0] = 0x3002;
   for (b = 0, i = 0; i < 100; i++)
      b = call_vram_thumb(b);
   result(0, a == 100 && b == 200, (b << 16) | a);

   /* 1: ARM in VRAM -- "add r0, r0, #3; bx lr", then "#5". */
   ((vu32 *)0x06008100)[0] = 0xE2800003;
   ((vu32 *)0x06008100)[1] = 0xE12FFF1E;
   for (a = 0, i = 0; i < 100; i++)
      a = call_vram_arm(a);
   ((vu32 *)0x06008100)[0] = 0xE2800005;
   for (b = 0, i = 0; i < 100; i++)
      b = call_vram_arm(b);
   result(1, a == 300 && b == 500, (b << 16) | a);

   /* 2: BG0CNT, bit 13 does not exist there. */
   REG(0x008) = 0xFFFF;
   v = REG(0x008);
   result(2, v == 0xDFFF, v);
   REG(0x008) = 0;

   /* 3: WININ, bits 6-7 and 14-15 do not exist. */
   REG(0x048) = 0xFFFF;
   v = REG(0x048);
   result(3, v == 0x3F3F, v);
   REG(0x048) = 0;

   /* 4: 0x136 is not a register: it reads 0 and forgets a write. */
   REG(0x136) = 0x1234;
   v = REG(0x136);
   result(4, v == 0, v);

   /* 5: DMA3CNT_H, bits 0-4 do not exist (a write without bit 15 starts
    * nothing). */
   REG(0x0DE) = 0x001F;
   v = REG(0x0DE);
   result(5, v == 0, v);

   /* 6: SOUNDCNT_X, the master enable (bit 7) reads back; bits 4-6 and
    * 8-15 never read as 1.  (Bits 0-3 are the channels' status.) */
   REG(0x084) = 0x0080;
   v = REG(0x084);
   result(6, (v & 0xFF70) == 0 && (v & 0x80), v);

   /* 7, 8: the guest's flags across I/O reads (100 runs each, so the
    * routine is the translated one, not the first pass). */
   for (a = 1, i = 0; i < 100; i++)
      a &= flags_z_set();
   result(7, a == 1, a);
   for (a = 1, i = 0; i < 100; i++)
      a &= flags_z_clear();
   result(8, a == 1, a);

   /* 9: MSR into Thumb (100 runs: the translated routine too). */
   for (a = 0, i = 0; i < 100; i++)
      a = msr_to_thumb(a);
   result(9, a == 100, a);

   /* 10: Thumb in palette RAM -- "adds r0, #4; bx lr", then "#6". */
   ((vu16 *)0x050003F0)[0] = 0x3004;
   ((vu16 *)0x050003F0)[1] = 0x4770;
   for (a = 0, i = 0; i < 100; i++)
      a = call_pal_thumb(a);
   ((vu16 *)0x050003F0)[0] = 0x3006;
   for (b = 0, i = 0; i < 100; i++)
      b = call_pal_thumb(b);
   result(10, a == 400 && b == 600, (b << 16) | a);

   /* 11: ARM in OAM -- "add r0, r0, #7; bx lr", then "#9". */
   ((vu32 *)0x07000300)[0] = 0xE2800007;
   ((vu32 *)0x07000300)[1] = 0xE12FFF1E;
   for (a = 0, i = 0; i < 100; i++)
      a = call_oam_arm(a);
   ((vu32 *)0x07000300)[0] = 0xE2800009;
   for (b = 0, i = 0; i < 100; i++)
      b = call_oam_arm(b);
   result(11, a == 700 && b == 900, (b << 16) | a);

   for (i = 0; i < NTESTS; i++)
      all &= SRAM[0x08 + i] == 1;
   *(vu16 *)0x05000000 = all ? 0x03E0 : 0x001F;   /* green / red */
   for (;;)
      ;
}
