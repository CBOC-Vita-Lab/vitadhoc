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
 * 12  a timer that has just overflowed reads its reload value, not 0
 *     (the hang of Need for Speed: Underground at its language screen)
 * 13  register-specified shifts use Rs[7:0] only: ARM MOV(S) and Thumb
 *     LSL/LSR/ASR/ROR Rd, Rs in ROM (dynarec)
 * 14  ...the same routines run from VRAM (interpreter)
 * 15  Rm = PC in a register-specified shift reads the address + 12
 * 16  BX to an ARM address with bit 1 set fetches the word below it
 *     ("bx pc" in Camelot's IRQ veneer: Mario Golf, Mario Tennis, Golden
 *     Sun 2), in ROM
 * 17  a branch into the middle of a run of same-condition ARM instructions
 *     is conditional too (Mario Golf's stack loop), in ROM
 * 18  ...and in IWRAM
 * 19  FIQ has its own r8-r12: they survive an excursion into FIQ, FIQ keeps
 *     its own, and IRQ entered from FIQ sees the shared ones (OpenLara)
 * 20  LDM {pc}^ in a privileged mode loads the CPSR from the SPSR
 * 21  MSR CPSR_c (control field only) keeps the live N, Z, C, V flags
 *
 * "SRAM_V113" below makes the emulator give the cartridge battery SRAM. */
typedef unsigned char u8;
typedef unsigned short u16;
typedef unsigned int u32;
typedef unsigned long long u64;
typedef volatile u8 vu8;
typedef volatile u16 vu16;
typedef volatile u32 vu32;

#define REG(a) (*(vu16 *)(0x04000000 + (a)))
#define SRAM ((vu8 *)0x0E000000)
#define NTESTS 22
#ifndef VENEER_ITERS
#define VENEER_ITERS 50
#endif
#ifndef RUN_MASK
#define RUN_MASK 0xFFFFFFFFu   /* bit n: run test n (for hunting a hang) */
#endif

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


/* ---- register-specified shifts ------------------------------------------
 * Each routine takes r0 = Rm, r1 = Rs, r2 = the carry going in and returns
 * the result in r0 and the carry coming out in r1 (a 64-bit value).  The ARM
 * forms: "movs" (flags) and "mov" (the carry must come out untouched).  All
 * position independent, between sh_start and sh_end, so they can be copied
 * into VRAM and run by the interpreter. */
#define SH_ARM(name, op)                                                    \
   "  .global sha_" name "_f\n  .type sha_" name "_f, %function\n"          \
   "sha_" name "_f:\n"                                                      \
   "  cmp r2, #1\n  movs r0, r0, " op " r1\n  mov r1, #0\n"                \
   "  adc r1, r1, #0\n  bx lr\n"                                            \
   "  .global sha_" name "_n\n  .type sha_" name "_n, %function\n"          \
   "sha_" name "_n:\n"                                                      \
   "  cmp r2, #1\n  mov r0, r0, " op " r1\n  mov r1, #0\n"                  \
   "  adc r1, r1, #0\n  bx lr\n"
#define SH_THUMB(name, op)                                                  \
   "  .thumb_func\n  .global sht_" name "\n  .type sht_" name ", %function\n" \
   "sht_" name ":\n"                                                        \
   "  cmp r2, #1\n  " op " r0, r1\n  mov r1, #0\n  adc r1, r1\n  bx lr\n"   \
   "  .align 2\n  .arm\n"
__asm__(
   "  .arm\n"
   "  .align 2\n"
   "  .global sh_start\n"
   "sh_start:\n"
   SH_ARM("lsl", "lsl") SH_ARM("lsr", "lsr") SH_ARM("asr", "asr")
   SH_ARM("ror", "ror")
   SH_THUMB("lsl", "lsl") SH_THUMB("lsr", "lsr") SH_THUMB("asr", "asr")
   SH_THUMB("ror", "ror")
   "  .global sh_end\n"
   "sh_end:\n"
   "  .ltorg\n"
   /* Rm = PC: mov r0, pc, lsl r1 with r1 = 0 gives the address + 12. */
   "  .global pc_shift\n  .type pc_shift, %function\n"
   "pc_shift:\n"
   "  mov r0, pc, lsl r1\n"
   "  bx lr\n"
   "  .global pc_shift_f\n  .type pc_shift_f, %function\n"
   "pc_shift_f:\n"
   "  movs r0, pc, lsr r1\n"
   "  bx lr\n");

typedef u64 (*sh_fn_t)(u32, u32, u32);
extern u32 sh_start[], sh_end[];
#define SHARM(n) extern u64 sha_##n##_f(u32, u32, u32), sha_##n##_n(u32, u32, u32); \
                 extern u64 sht_##n(u32, u32, u32);
SHARM(lsl) SHARM(lsr) SHARM(asr) SHARM(ror)
extern u32 pc_shift(u32, u32), pc_shift_f(u32, u32);

/* ARM7TDMI: the amount is Rs[7:0] only. */
static void sh_ref(int op, u32 rm, u32 rs, u32 cin, u32 *res, u32 *c)
{
   u32 s = rs & 0xFF;
   *res = rm; *c = cin;
   if (s == 0)
      return;
   switch (op) {
   case 0:                                   /* LSL */
      if (s < 32)       { *c = (rm >> (32 - s)) & 1; *res = rm << s; }
      else if (s == 32) { *c = rm & 1; *res = 0; }
      else              { *c = 0; *res = 0; }
      break;
   case 1:                                   /* LSR */
      if (s < 32)       { *c = (rm >> (s - 1)) & 1; *res = rm >> s; }
      else if (s == 32) { *c = rm >> 31; *res = 0; }
      else              { *c = 0; *res = 0; }
      break;
   case 2:                                   /* ASR */
      if (s < 32) { *c = (rm >> (s - 1)) & 1; *res = (u32)((int)rm >> s); }
      else        { *c = rm >> 31; *res = (u32)((int)rm >> 31); }
      break;
   default: {                                /* ROR */
      u32 r = s & 31;
      *c = (rm >> ((s - 1) & 31)) & 1;
      *res = r ? (rm >> r) | (rm << (32 - r)) : rm;
      }
   }
}

static const u32 sh_rm[] = { 0x80000001, 0x40000002, 0xF0F0F00F, 0x7FFFFFFF, 0 };
static const u32 sh_rs[] = { 0, 1, 7, 31, 32, 33, 63, 64, 96, 128, 255,
                             0x100, 0x101, 0x11F, 0x120, 0x121, 0x1FF,
                             0xFFFFFF00, 0xFFFFFF20, 0xFFFFFFFF, 0x80000001 };

/* Run every case through the routines at `base` (offset from sh_start) and
 * count the mismatches; `seen` gets the first bad case. */
static u32 sh_run(u32 delta, u32 *seen)
{
   static sh_fn_t const arm_f[4] = { (sh_fn_t)sha_lsl_f, (sh_fn_t)sha_lsr_f,
                                     (sh_fn_t)sha_asr_f, (sh_fn_t)sha_ror_f };
   static sh_fn_t const arm_n[4] = { (sh_fn_t)sha_lsl_n, (sh_fn_t)sha_lsr_n,
                                     (sh_fn_t)sha_asr_n, (sh_fn_t)sha_ror_n };
   static sh_fn_t const thm[4] = { (sh_fn_t)sht_lsl, (sh_fn_t)sht_lsr,
                                   (sh_fn_t)sht_asr, (sh_fn_t)sht_ror };
   u32 bad = 0, op, i, j, cin, form;
   for (form = 0; form < 3; form++)
      for (op = 0; op < 4; op++)
         for (i = 0; i < sizeof sh_rm / 4; i++)
            for (j = 0; j < sizeof sh_rs / 4; j++)
               for (cin = 0; cin < 2; cin++) {
                  u32 res, c, gres, gc;
                  u64 g;
                  sh_fn_t f = form == 0 ? arm_f[op] : form == 1 ? arm_n[op]
                                                                : thm[op];
                  f = (sh_fn_t)((u32)f + delta);
                  g = f(sh_rm[i], sh_rs[j], cin);
                  gres = (u32)g; gc = (u32)(g >> 32);
                  sh_ref((int)op, sh_rm[i], sh_rs[j], cin, &res, &c);
                  /* the no-flags ARM form leaves the carry as it came in */
                  if (form == 1) c = cin;
                  if (gres != res || gc != c) {
                     if (!bad)
                        *seen = (form << 28) | (op << 24) | (j << 8) | i;
                     bad++;
                  }
               }
   return bad;
}

/* ---- Camelot's IRQ veneer: "bx pc" on a halfword boundary ---------------
 * The Thumb "bx pc" sits at an address = 2 (mod 4); PC reads as that + 4, a
 * halfword-aligned ARM target, and the ARM7TDMI fetches the word below
 * (bit 1 is ignored): the ARM code starts right after the "bx pc".  Returns 5
 * when the first ARM instruction ran.  bx_veneer_end - bx_veneer is the size
 * to copy (position independent). */
__asm__(
   "  .thumb\n"
   "  .align 2\n"
   "  .thumb_func\n"
   "  .global bx_veneer\n  .type bx_veneer, %function\n"
   "bx_veneer:\n"
   "  nop\n"
   "  bx pc\n"
   "  .arm\n"
   "  mov r0, #5\n"
   "  bx lr\n"
   "  .global bx_veneer_end\n"
   "bx_veneer_end:\n"
   "  .align 2\n"
   /* A run of EQ instructions entered by a branch in the middle with Z
    * clear: nothing in it may run.  Returns r0 (0 when right). */
   "  .arm\n"
   "  .global cond_run\n  .type cond_run, %function\n"
   "cond_run:\n"
   "  mov r0, #0\n"
   "  mov r3, #0\n"
   "  movs r1, #1\n"
   "  addeq r0, r0, #1000\n"
   "1:\n"
   "  addeq r0, r0, #10\n"
   "  addeq r0, r0, #100\n"
   "  add r3, r3, #1\n"
   "  cmp r3, #3\n"
   "  bne 1b\n"
   "  bx lr\n"
   "  .global cond_run_end\n"
   "cond_run_end:\n"
   "  .align 2\n"
   /* FIQ: r0 = 0 when everything held, else the number of the failed check.
    * Interrupts stay masked throughout (I and F set in every mode). */
   "  .global fiq_test\n  .type fiq_test, %function\n"
   "fiq_test:\n"
   "  push {r4-r12, lr}\n"
   "  mrs r4, cpsr\n"
   "  mov r8, #0x81\n  mov r9, #0x82\n  mov r10, #0x83\n"
   "  mov r11, #0x84\n  mov r12, #0x85\n"
   "  msr cpsr_c, #0xD1\n"               /* FIQ */
   "  mov r8, #0xF1\n  mov r9, #0xF2\n  mov r10, #0xF3\n"
   "  mov r11, #0xF4\n  mov r12, #0xF5\n"
   "  msr cpsr_c, #0xDF\n"               /* back to system */
   "  mov r0, #1\n"
   "  cmp r8, #0x81\n  cmpeq r9, #0x82\n  cmpeq r10, #0x83\n"
   "  cmpeq r11, #0x84\n  cmpeq r12, #0x85\n"
   "  bne 9f\n"                          /* shared r8-r12 survived FIQ */
   "  msr cpsr_c, #0xD1\n"               /* FIQ again: its own, kept */
   "  mov r0, #2\n"
   "  cmp r8, #0xF1\n  cmpeq r9, #0xF2\n  cmpeq r10, #0xF3\n"
   "  cmpeq r11, #0xF4\n  cmpeq r12, #0xF5\n"
   "  bne 8f\n"
   "  msr cpsr_c, #0xD2\n"               /* IRQ, straight from FIQ */
   "  mov r0, #3\n"
   "  cmp r8, #0x81\n  cmpeq r9, #0x82\n  cmpeq r10, #0x83\n"
   "  cmpeq r11, #0x84\n  cmpeq r12, #0x85\n"
   "  bne 8f\n"                          /* IRQ sees the shared ones */
   "  mov r8, #0xA1\n  mov r9, #0xA2\n"  /* ...and writes the shared ones */
   "  msr cpsr_c, #0xD1\n"
   "  mov r0, #4\n"
   "  cmp r8, #0xF1\n  cmpeq r9, #0xF2\n  cmpeq r10, #0xF3\n"
   "  cmpeq r11, #0xF4\n  cmpeq r12, #0xF5\n"
   "  bne 8f\n"                          /* FIQ's own not touched by IRQ */
   "  msr cpsr_c, #0xDF\n"
   "  mov r0, #5\n"
   "  cmp r8, #0xA1\n  cmpeq r9, #0xA2\n  cmpeq r10, #0x83\n"
   "  bne 9f\n"
   "  mov r0, #0\n"
   "  b 9f\n"
   "8:\n"
   "  msr cpsr_c, #0xDF\n"
   "9:\n"
   "  msr cpsr_c, r4\n"
   "  pop {r4-r12, lr}\n"
   "  bx lr\n"
   "  .ltorg\n"

   /* LDM {pc}^: from IRQ mode with SPSR = system mode + Z; returns the CPSR
    * as it is right after the load (the mode, the I and F masks and Z). */
   "  .global ldm_spsr\n  .type ldm_spsr, %function\n"
   "ldm_spsr:\n"
   "  push {r4-r6, lr}\n"
   "  mrs r4, cpsr\n"
   "  ldr r5, =ldm_buf\n"
   "  adr r6, 1f\n"
   "  str r6, [r5]\n"
   "  msr cpsr_c, #0xD2\n"               /* IRQ mode, masked */
   "  mov sp, r5\n"
   "  ldr r0, =0x400000DF\n"
   "  msr spsr_cxsf, r0\n"
   "  ldmfd sp!, {pc}^\n"
   "  b 2f\n"
   "1:\n"
   "  mrs r0, cpsr\n"
   "2:\n"
   "  msr cpsr_c, r4\n"
   "  pop {r4-r6, lr}\n"
   "  bx lr\n"
   "  .ltorg\n"
   /* MSR CPSR_c with Z live: returns CPSR flags | the live Z. */
   "  .global msr_flags\n  .type msr_flags, %function\n"
   "msr_flags:\n"
   "  push {r4, lr}\n"
   "  mrs r4, cpsr\n"
   "  mov r1, #0\n"
   "  adds r1, r1, #0\n"                /* N=0 Z=1 C=0 V=0 */
   "  msr cpsr_c, #0xDF\n"
   "  moveq r2, #1\n"
   "  movne r2, #0\n"
   "  mrs r0, cpsr\n"
   "  and r0, r0, #0xF0000000\n"
   "  orr r0, r0, r2\n"
   "  msr cpsr_c, r4\n"
   "  pop {r4, lr}\n"
   "  bx lr\n"
   "  .align 2\n"
);
extern u32 bx_veneer[], bx_veneer_end[], cond_run[], cond_run_end[];
extern u32 fiq_test(void), ldm_spsr(void), msr_flags(void);
u32 ldm_buf[4];

/* Copy [from, to) (word aligned) to IWRAM or VRAM at `dst` (4-aligned) and return the address to
 * call (with the Thumb bit when `thumb`). */
static u32 to_iwram(u32 *from, u32 *to, u32 dst, u32 thumb)
{
   /* (the Thumb bit is passed, not read from `from`: as a u32 array the
    * compiler may assume its address is even) */
   const vu32 *a = (const vu32 *)((u32)from & ~1u);
   const vu32 *e = (const vu32 *)((u32)to & ~1u);
   vu32 *d = (vu32 *)dst;       /* words: VRAM ignores byte writes */
   while (a < e)
      *d++ = *a++;
   return dst | thumb;
}

static u32 timer_min_read(void)
{
   u32 i, v, min = 0xFFFF;
   REG(0x106) = 0;                         /* TM1CNT_H: stopped */
   REG(0x104) = 0xFF00;                    /* reload: 256 cycles a period */
   REG(0x106) = 0x0080;                    /* start, prescale 1 */
   for (i = 0; i < 6000; i++) {
      v = REG(0x104);
      if (v < min)
         min = v;
   }
   REG(0x106) = 0;
   return min;
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


   if (RUN_MASK & (1u << 12)) {
   /* 12: a timer that has just overflowed reads its reload, never below. */
   v = timer_min_read();
   result(12, v >= 0xFF00, v);

   }

   if (RUN_MASK & (1u << 13)) {
   /* 13, 14: register-specified shifts, from ROM and from VRAM. */
   {
      u32 seen = 0, bad, dst;
      bad = sh_run(0, &seen);
      result(13, bad == 0, bad ? seen : 0);
      dst = to_iwram(sh_start, sh_end, 0x06008200, 0);
      bad = sh_run(dst - (u32)sh_start, &seen);
      result(14, bad == 0, bad ? seen : 0);
   }

   }

   if (RUN_MASK & (1u << 15)) {
   /* 15: Rm = PC reads the address + 12 in a register-specified shift. */
   a = 1;
   for (i = 0; i < 100; i++)
      a &= (pc_shift(0, 0) == (u32)pc_shift + 12) &&
           (pc_shift_f(0, 0) == (u32)pc_shift_f + 12);
   result(15, a == 1, pc_shift(0, 0) - (u32)pc_shift);

   }

   if (RUN_MASK & (1u << 16)) {
   /* 16: BX to an ARM address with bit 1 set (the same routine copied to
    * IWRAM never came back on the x86 twin even before the fix, and is not
    * tested). */
   for (a = 0, i = 0; i < 50; i++)
      a += ((fn_t)bx_veneer)(0);
   result(16, a == 250, a);
   }

   if (RUN_MASK & (1u << 17)) {
   /* 17, 18: a branch into a run of EQ instructions with Z clear. */
   for (a = 0, i = 0; i < 50; i++)
      a += ((fn_t)cond_run)(0);
   result(17, a == 0, a);
   {
      fn_t f = (fn_t)to_iwram(cond_run, cond_run_end, 0x03006100, 0);
      for (a = 0, i = 0; i < 50; i++)
         a += f(0);
      result(18, a == 0, a);
   }

   }

   if (RUN_MASK & (1u << 19)) {
   /* 19: FIQ's own r8-r12. */
   for (a = 0, i = 0; i < 20; i++)
      a |= fiq_test();
   result(19, a == 0, a);

   }

   if (RUN_MASK & (1u << 20)) {
   /* 20: LDM {pc}^ restores the CPSR from the SPSR. */
   for (a = 0, i = 0; i < 20; i++)
      a |= ldm_spsr() ^ 0x400000DF;
   result(20, a == 0, ldm_spsr());

   }

   if (RUN_MASK & (1u << 21)) {
   /* 21: MSR CPSR_c keeps the live flags. */
   for (a = 0, i = 0; i < 20; i++)
      a |= msr_flags() ^ 0x40000001;
   result(21, a == 0, msr_flags());

   }

   for (i = 0; i < NTESTS; i++)
      all &= SRAM[0x08 + i] == 1;
   *(vu16 *)0x05000000 = all ? 0x03E0 : 0x001F;   /* green / red */
   for (;;)
      ;
}
