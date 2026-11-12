/* smc.c -- a test cartridge for self-modifying RAM code (SMC_RANGE,
 * app_tests.py t_smc).  Every test runs code that the program wrote into
 * IWRAM or EWRAM, rewrites it in some way and runs it again.  The results are
 * gpSP's semantics of a code write, which the full-flush build defines and
 * smc_partial must reproduce exactly: a store that lands on translated code
 * completes, and execution resumes at the NEXT instruction from a fresh
 * lookup (so an instruction the store rewrote right after itself runs in its
 * NEW form, where an ARM7TDMI would still execute the prefetched old one:
 * test 6 records that, and the app test enforces what the full-flush build
 * does).
 *
 * Battery RAM, written as it goes:
 *   0x00..0x05  "SMCTST"
 *   0x06        number of tests
 *   0x08 + n    test n: 1 = passed, 2 = failed, 0 = never reached
 *   0x40 + 4n   test n: the value it saw (little endian)
 * The backdrop turns green when everything passed, red otherwise.
 *
 *  0  ARM code in IWRAM, rewritten by an ARM-state STR, runs in its new form
 *  1  Thumb code in IWRAM, rewritten by a Thumb-state STRH
 *  2  ARM code, one byte of it rewritten by STRB (the immediate)
 *  3  STM whose LAST word lands on code (the word STM checks)
 *  4  STM whose FIRST word lands on code and whose last does not: no trap
 *     in either build, and the block runs stale until the next code write
 *     elsewhere -- after which the new bytes must run (kept code is only
 *     revived when its source is unchanged)
 *  5  DMA3 copies a new routine over the old one
 *  6  the store rewrites the instruction right after itself (gpSP: the new
 *     one runs; the hardware's prefetch would run the old one)
 *  7  the store rewrites an instruction later in its own block
 *  8  a block's direct successor (linked) is rewritten; the predecessor is
 *     not and must reach the new successor
 *  9  the predecessor is rewritten, the successor not
 * 10  CpuSet (SWI 0x0B) copies a new routine over the old one
 * 11  a routine in EWRAM that straddles a 32 KiB page (0x02008000)
 * 12  1000 writes of DATA right after the code of a live routine (outside
 *     its extent): the routine keeps working (no trap in either build; the
 *     harness counts them)
 * 13  decompress-and-run: 300 rounds of copying one of three routines over
 *     the same buffer (each with a round-dependent immediate) and running it
 * 14  code written through the IWRAM mirror (+0x8000) and run at its home
 * 15  the same through the EWRAM mirror (+0x40000)
 * 16  a routine that patches its own immediate 2000 times in a loop (the
 *     1-3 SMC events a frame of the games that motivated SMC_RANGE)
 * 17  ARM and Thumb code at the same address, alternately written and run
 * 18  a chain of 20 linked IWRAM blocks; one in the middle is rewritten
 *     and the whole chain run again, 50 times, at different links
 *
 * "SRAM_V113" below makes the emulator give the cartridge battery SRAM. */
typedef unsigned char u8;
typedef unsigned short u16;
typedef unsigned int u32;
typedef volatile u8 vu8;
typedef volatile u16 vu16;
typedef volatile u32 vu32;

#define REG(a) (*(vu16 *)(0x04000000 + (a)))
#define REG32(a) (*(vu32 *)(0x04000000 + (a)))
#define SRAM ((vu8 *)0x0E000000)
#define NTESTS 19

volatile u32 g_frames;              /* crt0.S counts vertical blanks here */
const char save_tag[] __attribute__((used)) = "SRAM_V113";

typedef u32 (*fn_t)(u32);

/* ARM encodings */
#define A_MOV_R0(i)   (0xE3A00000u | (i))           /* mov r0, #i          */
#define A_ADD_R0(i)   (0xE2800000u | (i))           /* add r0, r0, #i      */
#define A_BX_LR       0xE12FFF1Eu
#define A_NOP         0xE1A00000u                   /* mov r0, r0          */
#define A_STR_R1_R2   0xE5821000u                   /* str r1, [r2]        */
/* b from word address `at` to `to` */
#define A_B(at, to)   (0xEA000000u | ((((u32)(to) - ((u32)(at) + 8)) >> 2) & 0xFFFFFF))
/* Thumb encodings */
#define T_MOVS_R0(i)  (0x2000u | (i))
#define T_ADDS_R0(i)  (0x3000u | (i))
#define T_BX_LR       0x4770u

/* Helpers in ROM, ARM state: each does one store of its kind. */
extern void arm_str(u32 addr, u32 val);
extern void arm_strb(u32 addr, u32 val);
extern void arm_stm2(u32 addr, u32 a, u32 b);
extern void thumb_strh(u32 addr, u32 val);
/* Calls fn (ARM) with r0, r1, r2. */
extern u32 call3(u32 fn, u32 r0, u32 r1, u32 r2);
__asm__(
   "  .arm\n"
   "  .global call3\n  .type call3, %function\n"
   "call3:\n"
   "  push {r4, lr}\n"
   "  mov r12, r0\n  mov r0, r1\n  mov r1, r2\n  mov r2, r3\n"
   "  mov lr, pc\n  bx r12\n"
   "  pop {r4, lr}\n  bx lr\n"
);
__asm__(
   "  .arm\n"
   "  .global arm_str\n  .type arm_str, %function\n"
   "arm_str:\n  str r1, [r0]\n  bx lr\n"
   "  .global arm_strb\n  .type arm_strb, %function\n"
   "arm_strb:\n  strb r1, [r0]\n  bx lr\n"
   "  .global arm_stm2\n  .type arm_stm2, %function\n"
   "arm_stm2:\n  stmia r0, {r1, r2}\n  bx lr\n"
   "  .thumb\n  .thumb_func\n"
   "  .global thumb_strh\n  .type thumb_strh, %function\n"
   "thumb_strh:\n  strh r1, [r0]\n  bx lr\n"
   "  .arm\n  .balign 4\n"
);

static u32 vsum;     /* everything a test saw, for its log value */

static void result(int n, u32 got, u32 want)
{
   int k;
   SRAM[0x08 + n] = got == want ? 1 : 2;
   for (k = 0; k < 4; k++)
      SRAM[0x40 + 4 * n + k] = (u8)(got >> (8 * k));
}

static void wr32(u32 a, u32 v) { *(vu32 *)a = v; }
static void wr16(u32 a, u16 v) { *(vu16 *)a = v; }
static u32 call_arm(u32 a, u32 r0) { return ((fn_t)a)(r0); }
static u32 call_thumb(u32 a, u32 r0) { return ((fn_t)(a | 1))(r0); }

static void dma3(u32 src, u32 dst, u32 words)
{
   REG32(0xD4) = src;
   REG32(0xD8) = dst;
   REG32(0xDC) = words | 0x84000000u;   /* enable, 32-bit, immediately */
   while (REG32(0xDC) & 0x80000000u)
      ;
}

static void cpuset(u32 src, u32 dst, u32 words)
{
   register u32 r0 __asm__("r0") = src;
   register u32 r1 __asm__("r1") = dst;
   register u32 r2 __asm__("r2") = words | (1u << 26);   /* 32-bit copy */
   __asm__ volatile("swi 0x0B" : "+r"(r0), "+r"(r1), "+r"(r2) : : "r3", "memory");
}

/* A word in ROM to copy from. */
static const u32 rom_r5[2] = { A_MOV_R0(55), A_BX_LR };
static const u32 rom_r10[2] = { A_MOV_R0(77), A_BX_LR };

#define IW(off) (0x03004000u + (off))
#define EW(off) (0x02010000u + (off))

int main(void)
{
   u32 v, i, w, all = 1;

   REG(0x000) = 0x0000;
   *(vu16 *)0x05000000 = 0x7FFF;
   for (i = 0; i < 6; i++)
      SRAM[i] = (u8)"SMCTST"[i];
   SRAM[6] = NTESTS;
   for (i = 0; i < NTESTS; i++)
      SRAM[0x08 + i] = 0;

   /* 0: ARM STR */
   wr32(IW(0x000), A_MOV_R0(1)); wr32(IW(0x004), A_BX_LR);
   v = call_arm(IW(0x000), 0);
   arm_str(IW(0x000), A_MOV_R0(2));
   v = v * 10 + call_arm(IW(0x000), 0);
   result(0, v, 12);

   /* 1: Thumb STRH */
   wr16(IW(0x100), T_MOVS_R0(3)); wr16(IW(0x102), T_BX_LR);
   v = call_thumb(IW(0x100), 0);
   thumb_strh(IW(0x100), T_MOVS_R0(4));
   v = v * 10 + call_thumb(IW(0x100), 0);
   result(1, v, 34);

   /* 2: ARM STRB of the immediate */
   wr32(IW(0x200), A_MOV_R0(5)); wr32(IW(0x204), A_BX_LR);
   v = call_arm(IW(0x200), 0);
   arm_strb(IW(0x200), 6);
   v = v * 10 + call_arm(IW(0x200), 0);
   result(2, v, 56);

   /* 3: STM, last word on code */
   wr32(IW(0x300), A_NOP); wr32(IW(0x304), A_MOV_R0(7)); wr32(IW(0x308), A_BX_LR);
   v = call_arm(IW(0x300), 0);
   arm_stm2(IW(0x300), A_NOP, A_MOV_R0(8));
   v = v * 10 + call_arm(IW(0x300), 0);
   result(3, v, 78);

   /* 4: STM whose first word rewrites live code (bx lr -> add r0, #5) and
    * whose last word (bx lr) lands just past the block, where nothing is
    * tagged: no trap, the block runs stale (1); after a code write to
    * another routine the new bytes run (6). */
   wr32(IW(0x400), A_MOV_R0(1)); wr32(IW(0x404), A_BX_LR);
   wr32(IW(0x500), A_MOV_R0(2)); wr32(IW(0x504), A_BX_LR);
   v = call_arm(IW(0x400), 0);                /* 1 */
   v = v * 10 + call_arm(IW(0x500), 0);       /* 2 */
   arm_stm2(IW(0x404), A_ADD_R0(5), A_BX_LR);
   v = v * 10 + call_arm(IW(0x400), 0);       /* 1: stale, as in gpSP */
   arm_str(IW(0x500), A_MOV_R0(3));           /* traps */
   v = v * 10 + call_arm(IW(0x500), 0);       /* 3 */
   v = v * 10 + call_arm(IW(0x400), 0);       /* 6 */
   result(4, v, 12136);

   /* 5: DMA3 over the routine */
   wr32(IW(0x700), A_MOV_R0(9)); wr32(IW(0x704), A_BX_LR);
   v = call_arm(IW(0x700), 0);
   dma3((u32)rom_r5, IW(0x700), 2);
   v = v * 100 + call_arm(IW(0x700), 0);
   result(5, v, 955);

   /* 6: the store rewrites the next instruction */
   wr32(IW(0x800), A_STR_R1_R2);
   wr32(IW(0x804), A_MOV_R0(1));
   wr32(IW(0x808), A_BX_LR);
   v = call3(IW(0x800), 0, A_MOV_R0(2), IW(0x804));
   result(6, v, 2);

   /* 7: the store rewrites an instruction later in its own block */
   wr32(IW(0x900), A_STR_R1_R2);
   wr32(IW(0x904), A_MOV_R0(1));
   wr32(IW(0x908), A_ADD_R0(0));
   wr32(IW(0x90C), A_BX_LR);
   v = call3(IW(0x900), 0, A_ADD_R0(5), IW(0x908));
   v = v * 10 + call_arm(IW(0x904), 0);       /* the new form again: 6 */
   result(7, v, 66);

   /* 8: the linked successor is rewritten */
   wr32(IW(0xA00), A_MOV_R0(1));
   wr32(IW(0xA04), A_B(IW(0xA04), IW(0xA40)));
   wr32(IW(0xA40), A_ADD_R0(1));
   wr32(IW(0xA44), A_BX_LR);
   v = call_arm(IW(0xA00), 0);                /* 2 */
   v = v * 100 + call_arm(IW(0xA00), 0);      /* 2 (linked now) */
   arm_str(IW(0xA40), A_ADD_R0(10));
   v = v * 100 + call_arm(IW(0xA00), 0);      /* 11 */
   v = v * 100 + call_arm(IW(0xA00), 0);      /* 11 */
   result(8, v, 2021111);

   /* 9: the predecessor is rewritten, the successor not */
   arm_str(IW(0xA00), A_MOV_R0(20));
   v = call_arm(IW(0xA00), 0);                /* 30 */
   v = v * 100 + call_arm(IW(0xA40), 0);      /* 10 */
   result(9, v, 3010);

   /* 10: CpuSet over the routine */
   wr32(IW(0xB00), A_MOV_R0(11)); wr32(IW(0xB04), A_BX_LR);
   v = call_arm(IW(0xB00), 0);
   cpuset((u32)rom_r10, IW(0xB00), 2);
   v = v * 100 + call_arm(IW(0xB00), 0);
   result(10, v, 1177);

   /* 11: EWRAM routine across 0x02008000 */
   wr32(0x02007FF8, A_MOV_R0(1));
   wr32(0x02007FFC, A_ADD_R0(2));
   wr32(0x02008000, A_ADD_R0(4));
   wr32(0x02008004, A_BX_LR);
   v = call_arm(0x02007FF8, 0);               /* 7 */
   arm_str(0x02008000, A_ADD_R0(40));
   v = v * 100 + call_arm(0x02007FF8, 0);     /* 43 */
   arm_str(0x02007FFC, A_ADD_R0(20));
   v = v * 100 + call_arm(0x02007FF8, 0);     /* 61 */
   result(11, v, 74361);

   /* 12: data right after a live routine */
   wr32(IW(0xC00), A_MOV_R0(12)); wr32(IW(0xC04), A_BX_LR);
   v = 0;
   for (i = 0; i < 1000; i++) {
      wr32(IW(0xC08), i);
      wr32(IW(0xC0C), ~i);
      v += call_arm(IW(0xC00), 0) + *(vu32 *)IW(0xC08) - i;
   }
   result(12, v, 12000);

   /* 13: decompress-and-run */
   v = 0;
   for (i = 0; i < 300; i++) {
      u32 k = i % 3, imm = i & 0xFF;
      if (k == 0) {
         wr32(EW(0x000), A_MOV_R0(imm)); wr32(EW(0x004), A_BX_LR);
      } else if (k == 1) {
         wr32(EW(0x000), A_MOV_R0(imm)); wr32(EW(0x004), A_ADD_R0(1));
         wr32(EW(0x008), A_BX_LR);
      } else {
         wr32(EW(0x000), A_MOV_R0(1)); wr32(EW(0x004), A_ADD_R0(imm));
         wr32(EW(0x008), A_ADD_R0(2)); wr32(EW(0x00C), A_BX_LR);
      }
      w = call_arm(EW(0x000), 0);
      v += w;
   }
   {
      u32 want = 0;
      for (i = 0; i < 300; i++) {
         u32 k = i % 3, imm = i & 0xFF;
         want += k == 0 ? imm : k == 1 ? imm + 1 : imm + 3;
      }
      result(13, v, want);
   }

   /* 14: IWRAM mirror */
   wr32(IW(0xD00), A_MOV_R0(13)); wr32(IW(0xD04), A_BX_LR);
   v = call_arm(IW(0xD00), 0);
   arm_str(IW(0xD00) + 0x8000, A_MOV_R0(14));
   v = v * 100 + call_arm(IW(0xD00), 0);
   v = v * 100 + call_arm(IW(0xD00) + 0x8000, 0);
   result(14, v, 131414);

   /* 15: EWRAM mirror */
   wr32(EW(0x100), A_MOV_R0(15)); wr32(EW(0x104), A_BX_LR);
   v = call_arm(EW(0x100), 0);
   arm_str(EW(0x100) + 0x40000, A_MOV_R0(16));
   v = v * 100 + call_arm(EW(0x100), 0);
   v = v * 100 + call_arm(EW(0x100) + 0x40000, 0);
   result(15, v, 151616);

   /* 16: a routine that patches its own immediate, 2000 times:
    *   0: ldrb r1, [pc, #8]    (the immediate byte of word 4: 0x14)
    *      no -- keep it simple: r0 = count; the routine adds the current
    *      immediate of word 2 to r0 and stores imm+1 back into it. */
   wr32(IW(0xE00), 0xE59F2010u);   /* ldr r2, [pc, #16] -> word at 0xE18  */
   wr32(IW(0xE04), 0xE5D21000u);   /* ldrb r1, [r2]                       */
   wr32(IW(0xE08), A_ADD_R0(0));   /* add r0, r0, #imm (patched)          */
   wr32(IW(0xE0C), 0xE2811001u);   /* add r1, r1, #1                      */
   wr32(IW(0xE10), 0xE5C21000u);   /* strb r1, [r2]                       */
   wr32(IW(0xE14), A_BX_LR);
   wr32(IW(0xE18), IW(0xE08));     /* the literal                          */
   v = 0;
   for (i = 0; i < 2000; i++)
      v = call_arm(IW(0xE00), v);
   {
      u32 want = 0;
      for (i = 0; i < 2000; i++)
         want += i & 0xFF;
      result(16, v, want);
   }

   /* 17: ARM and Thumb at one address */
   v = 0;
   for (i = 0; i < 20; i++) {
      if (i & 1) {
         wr16(IW(0xF00), T_MOVS_R0(i)); wr16(IW(0xF02), T_BX_LR);
         v = v * 3 + call_thumb(IW(0xF00), 0);
      } else {
         wr32(IW(0xF00), A_MOV_R0(i)); wr32(IW(0xF04), A_BX_LR);
         v = v * 3 + call_arm(IW(0xF00), 0);
      }
   }
   {
      u32 want = 0;
      for (i = 0; i < 20; i++)
         want = want * 3 + i;
      result(17, v, want);
   }

   /* 18: a chain of 20 linked blocks, links rewritten */
   for (i = 0; i < 20; i++) {
      u32 a = IW(0x1000 + i * 0x40);
      wr32(a, A_ADD_R0(1));
      wr32(a + 4, i < 19 ? A_B(a + 4, a + 0x40) : A_BX_LR);
   }
   v = 0;
   for (i = 0; i < 50; i++) {
      u32 k = (i * 7) % 20;
      w = call_arm(IW(0x1000), 0);
      v += w;
      arm_str(IW(0x1000 + k * 0x40), A_ADD_R0((i & 3) + 1));
   }
   {
      u32 imm[20], want = 0;
      for (i = 0; i < 20; i++)
         imm[i] = 1;
      for (i = 0; i < 50; i++) {
         u32 k = (i * 7) % 20, j, s = 0;
         for (j = 0; j < 20; j++)
            s += imm[j];
         want += s;
         imm[k] = (i & 3) + 1;
      }
      result(18, v, want);
   }

   (void)vsum;
   for (i = 0; i < NTESTS; i++)
      all &= SRAM[0x08 + i] == 1;
   *(vu16 *)0x05000000 = all ? 0x03E0 : 0x001F;
   for (;;)
      ;
}
