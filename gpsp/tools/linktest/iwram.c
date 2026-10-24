/* iwram.c -- the link protocol of the test ROM.  Compiled as ARM code and
 * linked into IWRAM (linktest.ld), so every interrupt and every transfer
 * runs RAM-resident code; main.c patches code in here while it is in use.
 *
 * Everything below runs in interrupt context (one handler at a time, no
 * nesting), except mp_complete() when the parent polls instead of taking the
 * serial interrupt.  README.md describes the phases.
 */
#include "linktest.h"

volatile u32 role, phase, loop_no, pace, vbl_count, vbl_flag;
volatile u32 sw_step, sw_kind, gpio_child_state, my_keys, peer_keys;

static volatile u32 mt;            /* multi transfers completed, this segment */
static volatile u32 mp_cfg;        /* SIOCNT for multi-player mode            */
static volatile u32 cur_tx;        /* the word this side sends next           */
static volatile u32 seg_ok, seg_fail;
static volatile u32 keys_sent, keys_got;
static volatile u32 sw_n, sw_done, sw_nx, sw_frames, sw_tail, sw_bits;
static volatile u32 sw_pending, back_pending;
static volatile u32 gpio_k, gpio_j, gpio_last_sc, gpio_wait;
static volatile u32 sd_wait;

#define OTHER(r) ((r) == ROLE_PARENT ? ROLE_CHILD : ROLE_PARENT)
#define SYNC_WORD(r, p) (((r) == ROLE_PARENT ? 0xF000u : 0xF100u) | (p))
#define MP_BASE 0x6000u            /* multi-player, IRQ on completion */

/* The word sequence both sides know: role r, phase p, index i. */
u32 seq(u32 r, u32 p, u32 i)
{
   u32 h = i * 0x9E3779B1u + p * 0x85EBCA6Bu + r * 0xC2B2AE35u +
           loop_no * 0x27D4EB2Fu;
   h ^= h >> 15;
   h *= 0x2C1B3C6Du;
   h ^= h >> 12;
   return h;
}

void account(u32 p, u32 good, u32 code, u32 idx, u32 got, u32 exp)
{
   if (good)
   {
      R.ok[p]++;
      R.ok_total++;
      seg_ok++;
      return;
   }
   R.fail[p]++;
   R.fail_total++;
   seg_fail++;
   if (!R.first_fail)
   {
      R.first_fail = (p << 24) | (code << 16) | (idx & 0xFFFF);
      R.first_got = got;
      R.first_exp = exp;
   }
}

static void timer2_on(void)
{
   TM2CNT = 0;
   TM2D = 0xC000;                  /* 16384 cycles to overflow */
   TM2CNT = 0x00C0;                /* run, IRQ */
}

static void timer2_off(void)
{
   TM2CNT = 0;
}

/* How many checks this side passes in phase p when everything works: the
 * count the other side expects in the report. */
static u32 expect_ok(u32 p, u32 r)
{
   const struct phdef *d = &phases[p];
   if (d->kind == K_GPIO)
      return r == ROLE_CHILD ? 1 + GPIO_A_STEPS + 1 : 1 + GPIO_B_STEPS;
   return 1 + d->n;
}

static u32 report_word(void)
{
   return (role == ROLE_PARENT ? 0xE000u : 0xD000u) |
          (seg_fail ? 0x0800u : 0) | (seg_ok & 0x7FF);
}

/* Parent: start the next multi-player transfer. */
void mp_start(void)
{
   SIOMLT_SEND = cur_tx;
   SIOCNT = mp_cfg | 0x80;
}

/* ---- leaving and re-entering multi-player mode --------------------------- */

static void apply_switch(void)     /* the child's half, at its safe line */
{
   switch (sw_kind)
   {
   case K_NORM:                    /* armed slave: start set, external clock */
      SIOCNT = 0x4080 | (sw_bits == 32 ? 0x1000 : 0);
      break;
   case K_NC:                      /* idle; this side will supply the clock */
      SIOCNT = 0x5000;
      break;
   case K_GPIO:                    /* every pin an input, SI interrupt on */
      RCNT = 0x8100;
      gpio_child_state = GC_POLL;
      break;
   }
}

static void begin_switch(u32 p)
{
   const struct phdef *d = &phases[p];
   pace = PACE_NONE;
   sw_kind = d->kind;
   sw_n = d->n;
   sw_bits = d->arg;
   sw_done = sw_nx = sw_frames = sw_tail = 0;
   gpio_k = gpio_j = gpio_wait = 0;
   sw_step = 1;
   if (role == ROLE_CHILD)
   {
      sw_pending = 1;              /* applied at the safe line */
      return;
   }
   switch (sw_kind)
   {
   case K_NORM:
      SIOCNT = 0x4001 | (sw_bits == 32 ? 0x1000 : 0);
      break;
   case K_NC:                      /* armed slave with the first word loaded */
      SIOCNT = 0x5000;
      SIODATA32 = seq(ROLE_PARENT, p, 0x100);
      SIOCNT = 0x5080;
      break;
   case K_GPIO:                    /* SC, SD, SO outputs, all high */
      RCNT = 0x80BB;
      break;
   }
}

static void back_to_multi(void)
{
   timer2_off();
   if (sw_kind == K_NORM || sw_kind == K_NC)
      SIOCNT = SIOCNT & ~0x0080;   /* an armed start must not survive */
   RCNT = 0;
   mp_cfg = MP_BASE | 3;
   SIOCNT = mp_cfg;
   mt = 1;
   cur_tx = report_word();
   SIOMLT_SEND = cur_tx;
   sw_step = 0;
   gpio_child_state = GC_IDLE;
   pace = PACE_VBL;
   sd_wait = 0;
}

/* ---- multi-player: one transfer has completed ---------------------------- */

void mp_complete(void)
{
   u32 w0 = SIOMULTI0, w1 = SIOMULTI1;
   u32 i, p, r, rx, own, kind, n, last, good = 1, exp = 0, code = 1;
   const struct phdef *d;

   if (role == ROLE_NONE)
   {
      /* Roles are decided by the first exchange that looks like one: the
       * parent's and the child's opening words, each in its own slot. */
      if (w0 != SYNC_WORD(ROLE_PARENT, 1) || w1 != SYNC_WORD(ROLE_CHILD, 1))
         return;
      role = (SIOCNT & 0x0004) ? ROLE_CHILD : ROLE_PARENT;
      R.role = role;
      phase = 1;
      R.phase = 1;
      mt = 0;
      cur_tx = SYNC_WORD(role, 1);
      seg_ok = seg_fail = 0;
      pace = PACE_VBL;
   }
   r = role;
   i = mt;
   p = phase;
   d = &phases[p];
   kind = d->kind;
   n = d->n;
   last = (kind <= K_MPKEYS) ? n + 1 : 1;
   rx  = (r == ROLE_PARENT) ? w1 : w0;
   own = (r == ROLE_PARENT) ? w0 : w1;
   R.xfers_mp++;

   /* ---- what should have come in ---- */
   if (own != cur_tx)
   {
      good = 0;
      code = 2;
      exp = cur_tx;
      rx = own;
   }
   else if (i == 0)
   {
      exp = SYNC_WORD(OTHER(r), p);
      good = (rx == exp);
   }
   else if (i == last)
   {
      exp = (r == ROLE_PARENT ? 0xD000u : 0xE000u) |
            (expect_ok(p, OTHER(r)) & 0x7FF);
      good = (rx == exp);
      code = 3;
      R.peer_ok += rx & 0x7FF;
      if (rx & 0x0800)
         R.peer_fail++;
   }
   else if (kind == K_MPDATA)
   {
      exp = seq(OTHER(r), p, i) & 0x7FFF;
      good = (rx == exp);
   }
   else if (i & 1)                 /* key word */
   {
      exp = 0x8000;
      good = ((rx & 0xFC00) == 0x8000);
      keys_got = rx & 0x3FF;
      peer_keys = keys_got;
      R.peer_keys_sum = R.peer_keys_sum * 33 + keys_got;
      code = 4;
   }
   else                            /* the echo of the key word just sent */
   {
      exp = 0xC000 | keys_sent;
      good = (rx == exp);
      code = 5;
   }
   account(p, good, code, i, rx, exp);
   R.rx_sum = mix_iw(R.rx_sum ^ rx);
   R.tmr_mix = R.tmr_mix * 5 + TM0D + (TM1D << 16);

   /* ---- what goes out next ---- */
   if (i == last)                  /* report exchanged: next phase */
   {
      p++;
      if (p > NPH)
      {
         p = 1;
         loop_no++;
         R.loops = loop_no;
      }
      phase = p;
      R.phase = p;
      mt = 0;
      seg_ok = seg_fail = 0;
      mp_cfg = MP_BASE | (phases[p].kind <= K_MPKEYS ? phases[p].arg : 3);
      SIOCNT = mp_cfg;
      cur_tx = SYNC_WORD(r, p);
      pace = PACE_VBL;
      if (r == ROLE_CHILD)
         SIOMLT_SEND = cur_tx;
      return;
   }
   mt = i + 1;
   if (i == 0)
   {
      if (kind >= K_NORM)
      {
         begin_switch(p);
         return;
      }
      if (r == ROLE_PARENT)
      {
         if (d->pace == PACE_TMR)
         {
            pace = PACE_TMR;
            timer2_on();
         }
         else if (d->pace == PACE_POLL)
         {
            pace = PACE_POLL;      /* main.c runs these, no interrupt */
            mp_cfg &= ~0x4000u;
            SIOCNT = mp_cfg;
         }
      }
   }
   if (i + 1 == last)
   {
      cur_tx = report_word();
      if (r == ROLE_PARENT)
      {
         timer2_off();
         pace = PACE_VBL;
         if (!(mp_cfg & 0x4000))
         {
            mp_cfg |= 0x4000;
            SIOCNT = mp_cfg;
         }
      }
   }
   else if (kind == K_MPDATA)
      cur_tx = seq(r, p, i + 1) & 0x7FFF;
   else if ((i + 1) & 1)
   {
      keys_sent = my_keys;
      R.keys_sum = R.keys_sum * 33 + keys_sent;
      cur_tx = 0x8000 | keys_sent;
   }
   else
      cur_tx = 0xC000 | keys_got;
   if (r == ROLE_CHILD)
      SIOMLT_SEND = cur_tx;
}

/* ---- normal mode --------------------------------------------------------- */

/* Parent: clock out word sw_nx.  SO is set to a known level first (start
 * clear), which the child reads back as its SI. */
static void norm_start(void)
{
   u32 i = sw_nx, base, v;
   if (i >= sw_n || i != sw_done || (SIOCNT & 0x80))
      return;
   base = 0x4001 | (sw_bits == 32 ? 0x1000 : 0) | ((i & 1) ? 2 : 0) |
          (((i >> 1) & 1) << 3);
   v = seq(ROLE_PARENT, phase, i);
   SIOCNT = base;
   if (sw_bits == 32)
      SIODATA32 = v;
   else
      SIOMLT_SEND = v & 0xFF;
   SIOCNT = base | 0x80;
   sw_nx = i + 1;
}

static void switch_serial(void)
{
   u32 i = sw_done, p = phase, s = SIOCNT, rx, exp, good;

   if (sw_kind == K_GPIO)
   {
      if (role == ROLE_CHILD)
         R.gpio_irqs++;            /* SI fell */
      else
         account(p, 0, 9, 0, 0, 0);
      return;
   }
   if (sw_kind != K_NORM && sw_kind != K_NC)
      return;
   if (i >= sw_n)
   {
      account(p, 0, 8, i, s, 0);   /* a completion nobody asked for */
      return;
   }
   R.xfers_n++;
   rx = (sw_bits == 32) ? SIODATA32 : (u32)(SIOMLT_SEND & 0xFF);
   if (sw_kind == K_NORM)
   {
      if (role == ROLE_PARENT)
      {
         /* one-way cable: the parent receives zeroes, SI reads low */
         exp = 0;
         good = (rx == 0) && !(s & 0x0084);
      }
      else
      {
         exp = seq(ROLE_PARENT, p, i);
         if (sw_bits != 32)
            exp &= 0xFF;
         good = (rx == exp) && !(s & 0x0080) &&
                (((s >> 2) & 1) == ((i >> 1) & 1));
         R.rx_sum = mix_iw(R.rx_sum ^ rx);
         R.tmr_mix = R.tmr_mix * 5 + TM0D + (TM1D << 16);
      }
      account(p, good, 6, i, rx | ((s & 0x84) << 24), exp);
      sw_done = i + 1;
      if (role == ROLE_CHILD)
      {
         if (sw_done < sw_n)
            SIOCNT = 0x4080 | (sw_bits == 32 ? 0x1000 : 0);    /* re-arm */
         else
            back_pending = 1;
      }
      return;
   }
   /* K_NC: the child supplies the clock, the data still flows parent to
    * child.  The parent's completion reaches it late by up to the pair's
    * lookahead, so nothing timing-dependent is recorded on that side. */
   if (role == ROLE_PARENT)
   {
      exp = 0;
      good = (rx == 0) && !(s & 0x0080);
      account(p, good, 7, i, rx, exp);
      sw_done = i + 1;
      if (sw_done < sw_n)
      {
         SIODATA32 = seq(ROLE_PARENT, p, 0x100 + sw_done);
         SIOCNT = 0x5080;
      }
   }
   else
   {
      exp = seq(ROLE_PARENT, p, 0x100 + i);
      good = (rx == exp) && !(s & 0x0080);
      R.rx_sum = mix_iw(R.rx_sum ^ rx);
      account(p, good, 7, i, rx, exp);
      sw_done = i + 1;
      if (sw_done == sw_n)
         back_pending = 1;
   }
}

/* Parent, timer 2: one step of the general-purpose pattern.  SC toggles on
 * every step (the child's clock), SD and SO carry the data. */
static void gpio_step(void)
{
   u32 k = gpio_k;
   if (k >= GPIO_A_STEPS)
      return;
   RCNT = 0x80B0 | (k & 1) | (GP_SD(k) << 1) | (GP_SO(k) << 3);
   gpio_k = k + 1;
   if (gpio_k == GPIO_A_STEPS)
   {
      timer2_off();
      pace = PACE_NONE;
   }
}

static void sw_timeout(void)
{
   R.timeouts++;
   account(phase, 0, 10, sw_done, sw_frames, 0);
}

/* Parent, every V-blank while out of multi-player mode. */
static void sw_frame_parent(void)
{
   sw_frames++;
   switch (sw_kind)
   {
   case K_NORM:
      if (sw_done == sw_n)
      {
         if (++sw_tail >= 2)
            back_to_multi();
         break;
      }
      if (sw_frames > 90)
      {
         sw_timeout();
         back_to_multi();
         break;
      }
      if (sw_frames < 3)
         break;
      if (sw_nx < sw_n / 2)
         norm_start();             /* one a frame */
      else if (pace != PACE_NTMR)
      {
         pace = PACE_NTMR;         /* the rest from timer 2 */
         timer2_on();
      }
      break;
   case K_NC:
      if (sw_done == sw_n)
      {
         if (++sw_tail >= 2)
            back_to_multi();
      }
      else if (sw_frames > 60)
      {
         sw_timeout();
         back_to_multi();
      }
      break;
   case K_GPIO:
      if (sw_frames > 110)
      {
         sw_timeout();
         back_to_multi();
         break;
      }
      switch (sw_step)
      {
      case 1:
         if (sw_frames >= 3)
         {
            sw_step = 2;
            pace = PACE_GTMR;
            timer2_on();
         }
         break;
      case 2:
         if (gpio_k == GPIO_A_STEPS && ++sw_tail >= 2)
         {
            RCNT = 0x8000;         /* every pin an input */
            gpio_last_sc = 1;      /* an undriven SC reads high */
            sw_step = 3;
            sw_tail = 0;
         }
         break;
      case 3:
      {
         /* The child drives SC and SD now; sampled once a frame. */
         u32 v = RCNT, sc = v & 1;
         if (sc != gpio_last_sc)
         {
            account(phase, ((v >> 1) & 1) == GP_B(gpio_j), 11, gpio_j, v,
                    GP_B(gpio_j));
            gpio_last_sc = sc;
            if (++gpio_j == GPIO_B_STEPS)
               sw_step = 4;
         }
         break;
      }
      default:
         if (++sw_tail >= 2)
            back_to_multi();
         break;
      }
      break;
   }
}

/* Child, at its safe line: everything it changes on the cable. */
static void on_vcount(void)
{
   if (role != ROLE_CHILD)
      return;
   if (sw_pending)
   {
      sw_pending = 0;
      apply_switch();
      return;
   }
   if (back_pending)
   {
      back_pending = 0;
      back_to_multi();
      return;
   }
   if (!sw_step)
      return;
   if (sw_kind == K_NC)
   {
      /* one child-clocked transfer a frame */
      if (sw_frames >= 3 && sw_nx < sw_n && sw_nx == sw_done &&
          !(SIOCNT & 0x80))
      {
         sw_nx++;
         SIOCNT = 0x5081;
      }
   }
   else if (sw_kind == K_GPIO && gpio_child_state == GC_POLLED)
   {
      if (++gpio_wait >= 6)
      {
         gpio_child_state = GC_DRIVE;
         gpio_wait = 1;            /* drive on this very line */
      }
   }
   if (sw_kind == K_GPIO && gpio_child_state == GC_DRIVE)
   {
      if (gpio_wait & 1)           /* every second frame */
      {
         if (gpio_j < GPIO_B_STEPS)
         {
            RCNT = 0x8030 | (gpio_j & 1) | (GP_B(gpio_j) << 1);
            gpio_j++;
         }
         else
         {
            gpio_child_state = GC_DONE;
            back_pending = 1;
         }
      }
      gpio_wait++;
   }
}

/* ---- no partner: the cable code on its own -------------------------------- */

static volatile u32 solo_i, solo_busy;

static void solo_serial(void)
{
   u32 s = SIOCNT;
   u32 rx = (s & 0x1000) ? SIODATA32 : (u32)(SIOMLT_SEND & 0xFF);
   R.xfers_n++;
   solo_busy = 0;
   /* nothing on the other end: the master reads zeroes */
   account(PH_SOLO, rx == 0 && !(s & 0x80), 12, solo_i, rx, 0);
   R.tmr_mix = R.tmr_mix * 5 + TM0D + (TM1D << 16);
}

static void solo_frame(void)
{
   u32 i = solo_i, v = seq(ROLE_SOLO, PH_SOLO, i);
   switch (vbl_count & 3)
   {
   case 0:                         /* 8-bit, 256 kHz, interrupt */
   case 1:                         /* 32-bit, 2 MHz, interrupt */
   {
      u32 base = (vbl_count & 1) ? 0x5003 : 0x4001;
      if (solo_busy)
      {
         R.timeouts++;
         account(PH_SOLO, 0, 13, i, SIOCNT, 0);
         solo_busy = 0;
      }
      RCNT = 0;
      SIOCNT = base;
      if (base & 0x1000)
         SIODATA32 = v;
      else
         SIOMLT_SEND = v & 0xFF;
      solo_busy = 1;
      solo_i = i + 1;
      SIOCNT = base | 0x80;
      break;
   }
   case 2:                         /* general purpose: drive, then read */
   {
      u32 got;
      RCNT = 0x80F0 | (v & 0xF);
      RCNT = 0x8000;
      got = RCNT & 0xF;
      /* undriven SC, SD, SO read high; the parent end's SI is grounded */
      account(PH_SOLO, got == 0xB, 14, i, got, 0xB);
      RCNT = 0;
      break;
   }
   default:
      break;
   }
}

/* ---- interrupt dispatch --------------------------------------------------- */

static void on_serial(void)
{
   R.irq_sio++;
   if (role == ROLE_SOLO)
      solo_serial();
   else if (sw_step)
      switch_serial();
   else
      mp_complete();
}

static void on_timer2(void)
{
   R.irq_tm2++;
   if (role != ROLE_PARENT)
      return;
   if (pace == PACE_TMR)
   {
      if ((SIOCNT & 0x88) == 0x08)
         mp_start();
   }
   else if (pace == PACE_NTMR)
      norm_start();
   else if (pace == PACE_GTMR)
      gpio_step();
}

static void on_vblank(void)
{
   u32 s;
   vbl_count++;
   vbl_flag = 1;
   R.irq_vbl++;
   R.frame = vbl_count;
   my_keys = ~KEYINPUT & 0x3FF;
   R.keys_seen |= my_keys;

   switch (role)
   {
   case ROLE_NONE:
      s = SIOCNT;
      if (s & 0x80)
         break;
      if (vbl_count > 150)
      {
         role = ROLE_SOLO;         /* nobody answered */
         R.role = ROLE_SOLO;
         R.phase = PH_SOLO;
         break;
      }
      if ((s & 0x0C) == 0x08)      /* all ready, SI low: open as the parent */
      {
         cur_tx = SYNC_WORD(ROLE_PARENT, 1);
         mp_start();
      }
      else
      {
         cur_tx = SYNC_WORD(ROLE_CHILD, 1);
         SIOMLT_SEND = cur_tx;
      }
      break;
   case ROLE_PARENT:
      if (sw_step)
         sw_frame_parent();
      else if (pace == PACE_VBL)
      {
         if ((SIOCNT & 0x88) == 0x08)
         {
            sd_wait = 0;
            mp_start();
         }
         else if (++sd_wait == 300)
            R.timeouts++;          /* the child never came back */
      }
      break;
   case ROLE_CHILD:
      if (sw_step && ++sw_frames == 130)
      {
         sw_timeout();
         back_pending = 1;
      }
      break;
   default:
      solo_frame();
      break;
   }
}

void irq_main(u32 flags)
{
   if (flags & IRQ_SERIAL)
      on_serial();
   if (flags & IRQ_TIMER2)
      on_timer2();
   if (flags & IRQ_VCOUNT)
   {
      R.irq_vcnt++;
      on_vcount();
   }
   if (flags & IRQ_VBLANK)
      on_vblank();
   if (flags & IRQ_TIMER3)
      R.irq_tm3++;
}

void link_init(void)
{
   RCNT = 0;
   mp_cfg = MP_BASE | 3;
   SIOCNT = mp_cfg;
   cur_tx = SYNC_WORD(ROLE_CHILD, 1);
   SIOMLT_SEND = cur_tx;
}
