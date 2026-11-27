/* chain.c -- a test cartridge for XLAT_ITERATIVE (libretro/gpsp 70707ce,
 * app_tests.py t_chain): a very long chain of blocks that each end in an
 * unconditional branch to the next, met for the first time.  Translating the
 * first block resolves its exit by translating the second, and so on: with
 * the exits resolved by recursion the host stack grows by a few hundred bytes
 * per block, and a chain of a few thousand blocks overflows the Vita's 1 MiB
 * main thread stack (the test runs the host with a 1 MiB stack limit).
 *
 * Battery RAM, written at the end:
 *   0x00..0x04  "CHAIN"
 *   0x08        1 when the ARM chain returned CHAIN_LEN, 2 when it did not
 *   0x09        the same for the Thumb chain
 *   0x0A        the same for the chain after a RAM copy (IWRAM code is
 *               resolved the same way through the RAM cache)
 *   0x10 + 4n   the value chain n returned
 * The backdrop turns green when all passed, red otherwise. */
typedef unsigned char u8;
typedef unsigned short u16;
typedef unsigned int u32;
typedef volatile u8 vu8;
typedef volatile u16 vu16;
typedef volatile u32 vu32;

#define REG(a) (*(vu16 *)(0x04000000 + (a)))
#define SRAM ((vu8 *)0x0E000000)
#ifndef CHAIN_LEN
#define CHAIN_LEN 6000
#endif

volatile u32 g_frames;              /* crt0.S counts vertical blanks here */

const char save_tag[] __attribute__((used)) = "SRAM_V113";

typedef u32 (*fn_t)(u32);
extern u32 chain_arm(u32), chain_thumb(u32);
extern u32 ram_chain[], ram_chain_end[];

#define STR2(x) #x
#define STR(x) STR2(x)

/* Each link: add 1 to r0, then branch over one dead word to the next link.
 * (The dead word keeps the assembler from folding the branch away and gives
 * the scan a gap, so every link is its own block.) */
__asm__(
   "  .arm\n"
   "  .global chain_arm\n  .type chain_arm, %function\n"
   "chain_arm:\n"
   "  .rept " STR(CHAIN_LEN) "\n"
   "  add r0, r0, #1\n"
   "  b 1f\n"
   "  nop\n"
   "1:\n"
   "  .endr\n"
   "  bx lr\n"
   "  .thumb\n"
   "  .thumb_func\n"
   "  .global chain_thumb\n  .type chain_thumb, %function\n"
   "chain_thumb:\n"
   "  .rept " STR(CHAIN_LEN) "\n"
   "  add r0, #1\n"
   "  b 1f\n"
   "  nop\n"
   "1:\n"
   "  .endr\n"
   "  bx lr\n"
   "  .arm\n"
   "  .balign 4\n"
   /* a short ARM chain copied to IWRAM: 400 links of "add r0, r0, #1; b +4" */
   "  .global ram_chain\n"
   "ram_chain:\n"
   "  .rept 400\n"
   "  add r0, r0, #1\n"
   "  b 1f\n"
   "  nop\n"
   "1:\n"
   "  .endr\n"
   "  bx lr\n"
   "  .global ram_chain_end\n"
   "ram_chain_end:\n"
   "  .align 2\n"
);

static void result(int n, u32 got, u32 want)
{
   int k;
   SRAM[0x08 + n] = got == want ? 1 : 2;
   for (k = 0; k < 4; k++)
      SRAM[0x10 + 4 * n + k] = (u8)(got >> (8 * k));
}

int main(void)
{
   u32 a, b, c;
   vu32 *d = (vu32 *)0x03002000;
   const u32 *s = ram_chain;
   int i, all = 1;

   REG(0x000) = 0x0000;
   *(vu16 *)0x05000000 = 0x7FFF;
   for (i = 0; i < 5; i++)
      SRAM[i] = (u8)"CHAIN"[i];
   for (i = 0; i < 3; i++)
      SRAM[0x08 + i] = 0;

   a = chain_arm(0);
   result(0, a, CHAIN_LEN);
   b = chain_thumb(0);
   result(1, b, CHAIN_LEN);
   while (s < ram_chain_end)
      *d++ = *s++;
   c = ((fn_t)0x03002000)(0);
   result(2, c, 400);

   for (i = 0; i < 3; i++)
      all &= SRAM[0x08 + i] == 1;
   *(vu16 *)0x05000000 = all ? 0x03E0 : 0x001F;
   for (;;)
      ;
}
