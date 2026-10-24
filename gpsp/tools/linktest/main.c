/* main.c -- link test ROM: start-up, the per-frame background work that
 * keeps the dynarec's self-modifying-code and interrupt paths busy while the
 * cable is in use, the two polled phases, and the screen.  Thumb, in ROM.
 * The protocol itself is iwram.c.  README.md has the whole picture.
 */
#include "linktest.h"

volatile struct results R __attribute__((section(".results")));

IWFN void link_init(void);

/* kind, transfers, pacing, baud bits (multi) or data bits (normal) */
const struct phdef phases[NPH + 1] = {
   { 0, 0, 0, 0 },
   { K_MPDATA, 32, PACE_VBL,  3 },   /*  1 multi, one transfer a frame        */
   { K_MPDATA, 96, PACE_TMR,  3 },   /*  2 multi, bursts paced by timer 2     */
   { K_MPDATA,  4, PACE_POLL, 0 },   /*  3 multi, parent polls, 9600 baud     */
   { K_MPDATA,  4, PACE_POLL, 1 },   /*  4 ... 38400                          */
   { K_MPDATA,  4, PACE_POLL, 2 },   /*  5 ... 57600                          */
   { K_MPKEYS, 32, PACE_VBL,  3 },   /*  6 multi, the buttons and their echo  */
   { K_NORM,   24, 0,         8 },   /*  7 normal 8-bit, parent clocks        */
   { K_NORM,   24, 0,        32 },   /*  8 normal 32-bit, parent clocks       */
   { K_NC,     12, 0,        32 },   /*  9 normal 32-bit, child clocks        */
   { K_GPIO,   32, 0,         0 },   /* 10 general purpose pins, both ways    */
};

/* gcc may call these even in a freestanding build */
void *memset(void *d, int c, u32 n)
{
   u8 *p = d;
   while (n--)
      *p++ = (u8)c;
   return d;
}

void *memcpy(void *d, const void *s, u32 n)
{
   u8 *p = d;
   const u8 *q = s;
   while (n--)
      *p++ = *q++;
   return d;
}

static void vblank_wait(void)
{
   __asm__ volatile("swi 5" ::: "r0", "r1", "r2", "r3");
}

/* Unsigned divide without libgcc: the reference for the BIOS Div calls. */
static u32 udiv(u32 n, u32 d, u32 *rem)
{
   u32 q = 0, bit = 1;
   while (d < n && !(d & 0x80000000u))
   {
      d <<= 1;
      bit <<= 1;
   }
   while (bit)
   {
      if (n >= d)
      {
         n -= d;
         q |= bit;
      }
      d >>= 1;
      bit >>= 1;
   }
   *rem = n;
   return q;
}

static u32 isqrt(u32 n)
{
   u32 r = 0, b = 1u << 30;
   while (b > n)
      b >>= 2;
   while (b)
   {
      if (n >= r + b)
      {
         n -= r + b;
         r = (r >> 1) + b;
      }
      else
         r >>= 1;
      b >>= 2;
   }
   return r;
}

/* ---- background: code that changes under the dynarec's feet -------------- */

static void smc_check(u32 good)
{
   if (good)
      R.smc_ok++;
   else
      R.smc_fail++;
}

static u32 mix_ref(u32 x, u32 op, u32 imm)
{
   x ^= x << 5;
   switch (op)
   {
   case 0:  x += imm; break;
   case 1:  x ^= imm; break;
   case 2:  x -= imm; break;
   default: x |= imm; break;
   }
   x ^= x >> 7;
   return x;
}

static void ew_install(u32 by_dma)
{
   u32 n = (u32)(ew_img_end - ew_img);      /* halfwords */
   if (by_dma)
   {
      /* DMA3, immediately, 16-bit units: a DMA that lands on translated
       * code */
      DMA3SAD = (u32)ew_img;
      DMA3DAD = (u32)EWCODE;
      DMA3CNT = 0x80000000u | n;
   }
   else
   {
      u16 *d = (u16 *)EWCODE;
      u32 i;
      for (i = 0; i < n; i++)
         d[i] = ew_img[i];
   }
}

static void background(void)
{
   static const u32 opc[4] = { 0xE2800000u, 0xE2200000u, 0xE2400000u,
                               0xE3800000u };   /* add eor sub orr r0,r0,# */
   u32 f = vbl_count, op = f & 3, imm = (f * 7 + 3) & 0xFF;
   u32 x = 0x1234567u + f * 0x10001u, q, rem;

   /* 1. ROM code rewrites an instruction of the IWRAM mixer the serial
    *    handler uses, then the mixer must compute with the new one. */
   mix_iw_patch[0] = opc[op] | imm;
   smc_check(mix_iw(x) == mix_ref(x, op, imm));

   /* 2. IWRAM code that rewrites itself. */
   smc_check(selfmod_iw(f) == (f & 0xFF) + 1000);

   /* 3. BIOS calls from IWRAM (the dynarec's Div helper and a real SWI). */
   if ((f & 3) == 1)
   {
      u32 num = 1000 + (f & 0x3FF), den = 7 + (f & 0x1F);
      q = udiv(num, den, &rem);
      smc_check(swi_iw(num, den) == q + rem + isqrt(num));
   }

   /* 4. Thumb code in EWRAM: installed by DMA or by the CPU, an immediate
    *    patched in place, and a Div SWI from inside it. */
   if ((f & 7) == 2)
   {
      u32 (*ew)(u32, u32) = (u32 (*)(u32, u32))(EWCODE + 1);
      u32 pimm = (f >> 3) & 0xFF, t, den = 13;
      if ((f & 0x3F) == 2)
         ew_install((f >> 6) & 1);
      ((volatile u16 *)EWCODE)[ew_img_patch - ew_img] = 0x2200 | pimm;
      t = (f & 0xFFF) + pimm;
      q = udiv(t, den, &rem);
      smc_check(ew(f & 0xFFF, den) == q + (rem << 8) + t);
   }
}

/* ---- the phases that run in the main loop -------------------------------- */

static void wait_lines(u32 n)
{
   u32 last = VCOUNT;
   while (n)
   {
      u32 v = VCOUNT;
      if (v != last)
      {
         last = v;
         n--;
      }
   }
}

/* Parent, phases 3-5: no serial interrupt; the busy bit is polled. */
static void poll_segment(void)
{
   while (pace == PACE_POLL)
   {
      u32 t0 = vbl_count;
      wait_lines(3);               /* the child reloads in its handler */
      mp_start();
      while (SIOCNT & 0x80)
      {
         if (vbl_count - t0 > 30)
         {
            IME = 0;
            R.timeouts++;
            account(phase, 0, 15, 0, SIOCNT, 0);
            IME = 1;
            return;
         }
      }
      mp_complete();
   }
}

/* Child, phase 10: follow the parent's clock on SC by polling RCNT, check
 * the data on SD and SI at every edge. */
static void gpio_poll(void)
{
   u32 k = 0, last = RCNT & 1, t0 = vbl_count, i, falls = 0, so = 1;
   u32 irq0 = R.gpio_irqs;
   while (k < GPIO_A_STEPS && vbl_count - t0 < 40)
   {
      u32 v = RCNT;
      if ((v ^ last) & 1)
      {
         u32 got = (v >> 1) & 3;                     /* SD, SI */
         u32 exp = GP_SD(k) | (GP_SO(k) << 1);       /* SI is the parent's SO */
         last = v & 1;
         IME = 0;
         account(phase, got == exp && last == (k & 1), 16, k, v, exp);
         IME = 1;
         k++;
      }
   }
   wait_lines(4);
   /* every fall of the parent's SO must have raised the SI interrupt */
   for (i = 0; i < GPIO_A_STEPS; i++)
   {
      if (so && !GP_SO(i))
         falls++;
      so = GP_SO(i);
   }
   IME = 0;
   if (k < GPIO_A_STEPS)
      R.timeouts++;
   account(phase, R.gpio_irqs - irq0 == falls && k == GPIO_A_STEPS, 17, k,
           R.gpio_irqs - irq0, falls);
   gpio_child_state = GC_POLLED;
   IME = 1;
}

/* ---- screen: mode 3, block digits ---------------------------------------- */

#define FB ((volatile u16 *)0x06000000)
#define RGB(r, g, b) ((r) | ((g) << 5) | ((b) << 10))

static void rect(u32 x, u32 y, u32 w, u32 h, u32 c)
{
   u32 i, j;
   for (j = 0; j < h; j++)
      for (i = 0; i < w; i++)
         FB[(y + j) * 240 + x + i] = (u16)c;
}

/* 3x5 hex digits, one bit per cell, top row first */
static const u16 font[16] = {
   0x7B6F, 0x2492, 0x73E7, 0x73CF, 0x5BC9, 0x79CF, 0x79EF, 0x7249,
   0x7BEF, 0x7BCF, 0x7BED, 0x6BAE, 0x7927, 0x6B6E, 0x79A7, 0x79A4 };

static void digit(u32 x, u32 y, u32 d, u32 c)
{
   u32 bits = font[d & 15], r, k;
   for (r = 0; r < 5; r++)
      for (k = 0; k < 3; k++)
         rect(x + k * 2, y + r * 2, 2, 2,
              (bits >> (14 - (r * 3 + k))) & 1 ? c : 0);
}

#define ROWS 8
static u32 shown[ROWS];
static u8 drawn[ROWS];

static void row(u32 n, u32 v, u32 c)
{
   u32 i;
   for (i = 0; i < 8; i++)
   {
      u32 sh = 28 - 4 * i;
      if (drawn[n] && ((v ^ shown[n]) >> sh & 15) == 0)
         continue;
      digit(40 + i * 8, 20 + n * 14, v >> sh, c);
   }
   shown[n] = v;
   drawn[n] = 1;
}

static void draw(void)
{
   static const u16 tag[ROWS] = {
      RGB(31, 31, 31), RGB(0, 31, 0), RGB(31, 0, 0), RGB(0, 31, 31),
      RGB(31, 31, 0), RGB(31, 0, 31), RGB(15, 15, 31), RGB(31, 15, 0) };
   static u32 first = 1, last_state = ~0u;
   u32 bad = R.fail_total | R.smc_fail | R.timeouts | R.peer_fail;
   u32 state = (role << 8) | (bad ? 1 : 0) | (phase << 16);
   u32 i;

   if (first)
   {
      first = 0;
      for (i = 0; i < ROWS; i++)
         rect(24, 22 + i * 14, 8, 6, tag[i]);
   }
   if (state != last_state)
   {
      /* top band: left = who we are, right = verdict so far, and a marker
       * that walks with the phase */
      static const u16 who[4] = { RGB(8, 8, 8), RGB(31, 8, 0), RGB(0, 12, 31),
                                  RGB(20, 20, 20) };
      last_state = state;
      rect(0, 0, 80, 12, who[role & 3]);
      rect(80, 0, 160, 12, bad ? RGB(31, 0, 0) : RGB(0, 24, 0));
      rect(0, 14, 240, 3, 0);
      rect(phase * 20, 14, 20, 3, RGB(31, 31, 31));
   }
   row(0, (R.loops << 16) | (role << 8) | phase, RGB(31, 31, 31));
   row(1, R.ok_total, RGB(0, 31, 0));
   row(2, R.fail_total, RGB(31, 8, 8));
   row(3, R.peer_ok, RGB(0, 31, 31));
   row(4, R.rx_sum, RGB(31, 31, 0));
   row(5, (my_keys << 16) | peer_keys, RGB(31, 0, 31));
   row(6, (R.smc_ok << 8) | (R.smc_fail & 0xFF), RGB(15, 15, 31));
   row(7, (R.xfers_mp << 12) | (R.xfers_n & 0xFFF), RGB(31, 15, 0));
}

/* -------------------------------------------------------------------------- */

int main(void)
{
   u32 i;
   IME = 0;
   for (i = 0; i < sizeof(R) / 4; i++)
      ((volatile u32 *)&R)[i] = 0;
   R.magic = RESULT_MAGIC;

   DISPCNT = 0x0403;                          /* mode 3, BG2 */
   rect(0, 0, 240, 160, 0);
   ew_install(0);

   TM0D = 0;                                  /* a free-running clock */
   TM0CNT = 0x0080;
   TM1D = 0;
   TM1CNT = 0x0084;                           /* counts timer 0's overflows */
   TM3D = 0xFC00;                             /* background tick: 65536 cycles */
   TM3CNT = 0x00C1;

   link_init();
   DISPSTAT = 0x0028 | (SAFE_LINE << 8);      /* V-blank + V-count interrupts */
   IE = IRQ_VBLANK | IRQ_VCOUNT | IRQ_TIMER2 | IRQ_TIMER3 | IRQ_SERIAL;
   IME = 1;

   for (;;)
   {
      vblank_wait();
      if (role == ROLE_PARENT && pace == PACE_POLL)
         poll_segment();
      if (role == ROLE_CHILD && gpio_child_state == GC_POLL)
         gpio_poll();
      background();
      draw();
   }
   return 0;
}
