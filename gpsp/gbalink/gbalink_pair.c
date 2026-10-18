/* gbalink_pair.c -- the stepper for two linked GBA cores.  See
 * gbalink_pair.h for the rule and why it is deterministic. */
#include <string.h>

#include "gbalink_pair.h"

void gbalink_pair_init(gbalink_pair *p, const gbalink_inst_ops *s0,
                       const gbalink_inst_ops *s1,
                       const gbalink_pair_config *cfg)
{
   memset(p, 0, sizeof(*p));
   p->m[0] = *s0;
   p->m[1] = *s1;
   p->cfg = *cfg;
   if (!p->cfg.child_margin)
      p->cfg.child_margin = 1232;
   if (!p->cfg.lookahead)
      p->cfg.lookahead = GBALINK_DEFAULT_LOOKAHEAD;
   if (p->cfg.lookahead <= p->cfg.child_margin)
      p->cfg.lookahead = 2 * p->cfg.child_margin;   /* else nothing advances */
   p->last_ran = -1;
   p->running = -1;
   p->stress_state = p->cfg.stress_seed;
   gbalink_bus_reset();
   p->m[0].begin(0, p->cfg.headless[0]);
   p->m[1].begin(1, p->cfg.headless[1]);
}

void gbalink_pair_end(gbalink_pair *p)
{
   p->m[0].end();
   p->m[1].end();
}

void gbalink_pair_input(gbalink_pair *p, unsigned slot, uint32_t frame,
                        uint16_t keys)
{
   unsigned i = frame % GBALINK_IN_RING;
   slot &= 1;
   p->in_keys[slot][i] = keys;
   p->in_tag[slot][i] = frame + 1;
}

int gbalink_pair_has_input(const gbalink_pair *p, unsigned slot,
                           uint32_t frame)
{
   return p->in_tag[slot & 1][frame % GBALINK_IN_RING] == frame + 1;
}

/* Leave step() with an error, keeping the guard's view of this step. */
static int pair_fail(gbalink_pair *p, int err, uint32_t runs, uint32_t peer)
{
   p->error = err;
   p->running = -1;
   if (runs > p->st.step_runs_max)
      p->st.step_runs_max = runs;
   if (peer > p->st.step_peer_frames_max)
      p->st.step_peer_frames_max = peer;
   return GBALINK_PAIR_ERROR;
}

static uint32_t pair_rand(gbalink_pair *p)
{
   uint32_t x = p->stress_state;
   x ^= x << 13; x ^= x >> 17; x ^= x << 5;
   return p->stress_state = x;
}

int gbalink_pair_step(gbalink_pair *p)
{
   unsigned blocked_run = 0, idle_run = 0;
   uint32_t runs = 0, peer = 0;
   uint64_t blocked_at[2] = { ~(uint64_t)0, ~(uint64_t)0 };  /* per slot */
   uint64_t t0 = (p->cfg.now_us && p->cfg.step_us_max) ? p->cfg.now_us() : 0;

   if (p->error)
      return GBALINK_PAIR_ERROR;
   p->step_runs = 0;

   for (;;)
   {
      uint64_t n0 = p->m[0].now(), n1 = p->m[1].now();
      unsigned s = (n1 + p->cfg.child_margin < n0) ? 1 : 0;
      uint64_t horizon = s ? n0 - p->cfg.child_margin
                           : n1 + p->cfg.lookahead;
      int r;

      /* No step may run forever (section 22): every run is bounded by the
       * core's own guard, the runs by STEP_RUNS_MAX, and the whole step by
       * the wall clock. */
      /* the clock every 8 runs.  Every 32 (as first built) never read it
       * in a healthy step (~20 runs), so a step made of a few very slow
       * runs was bounded only by the core's 2,000,000-call guard per run
       * (review finding 6, 2026-10-11).  Two reads in a healthy step. */
      if (p->cfg.now_us && p->cfg.step_us_max && runs && !(runs & 7) &&
          p->cfg.now_us() - t0 > p->cfg.step_us_max)
         return pair_fail(p, GBALINK_PAIR_E_TIMEOUT, runs, peer);
      if (p->stress_state)
      {
         /* TEST ONLY: any horizon inside the rule's limits, and a pump of
          * either machine at a time the rule would not make one. */
         uint32_t x = pair_rand(p);
         if (p->cfg.stress_seed & 0x80000000u)
            ;                           /* pumps only: the rule's horizons */
         else if (s)
         {
            uint64_t span = n0 - p->cfg.child_margin - n1;
            horizon = n1 + 1 + (span > 1 ? x % span : 0);
         }
         else
         {
            uint64_t lo = (n0 > n1 ? n0 : n1) + 1, hi = n1 + p->cfg.lookahead;
            horizon = hi > lo ? lo + x % (hi - lo + 1) : hi;
         }
         if (x & 0x30000)
            p->m[(x >> 18) & 1].pump();
      }

      if (p->m[s].need_begin())
      {
         uint32_t f = p->frames[s];
         uint16_t k;
         if (!gbalink_pair_has_input(p, s, f) && p->cfg.input &&
             p->cfg.input(p->cfg.user, s, f, &k))
            gbalink_pair_input(p, s, f, k);
         if (!gbalink_pair_has_input(p, s, f))
         {
            p->st.stalls++;
            return GBALINK_PAIR_STALL;
         }
         p->m[s].frame_begin(p->in_keys[s][f % GBALINK_IN_RING]);
      }

      if (p->last_ran != (int)s)
         p->st.switches++;
      p->last_ran = (int)s;
      p->st.runs[s]++;
      p->step_runs = ++runs;
      if (runs > GBALINK_PAIR_STEP_RUNS_MAX)
         return pair_fail(p, GBALINK_PAIR_E_RUNAWAY, runs, peer);
      p->running = (int)s;
      r = p->m[s].run(horizon);
      p->running = -1;

      {
         gbalink_bus_stats bs;
         gbalink_bus_get_stats(&bs);
         if (bs.overflow)
            return pair_fail(p, GBALINK_PAIR_E_BUS, runs, peer);
      }

      switch (r)
      {
      case GBALINK_RUN_FRAME:
         blocked_run = 0;
         idle_run = 0;
         p->frames[s]++;
         if (p->cfg.on_frame)
            p->cfg.on_frame(p->cfg.user, s, p->frames[s]);
         if (s == p->cfg.local_slot)
         {
            if (runs > p->st.step_runs_max)
               p->st.step_runs_max = runs;
            if (peer > p->st.step_peer_frames_max)
               p->st.step_peer_frames_max = peer;
            return GBALINK_PAIR_FRAME;
         }
         if (++peer > GBALINK_PAIR_STEP_PEER_MAX)
            return pair_fail(p, GBALINK_PAIR_E_RUNAWAY, runs, peer);
         break;
      case GBALINK_RUN_HORIZON:
         blocked_run = 0;
         if (p->m[0].now() == n0 && p->m[1].now() == n1)
         {
            p->st.idle_runs++;
            if (++idle_run > GBALINK_PAIR_IDLE_MAX)
               return pair_fail(p, GBALINK_PAIR_E_STUCK, runs, peer);
         }
         else
            idle_run = 0;
         break;
      case GBALINK_RUN_BLOCKED:
         /* The other side has the bits we need, or will once it reaches the
          * transfer's start: let it apply what is due; the rule above then
          * runs it if it is behind. */
         p->st.blocked[s]++;
         p->m[s ^ 1].pump();
         /* Only blocking again at the SAME time is "no way forward".  A
          * child that clocks normal-mode transfers itself (the RFU-adapter
          * probe in Mario Golf / Tennis / Emerald: one every ~32,000 cycles
          * while the parent is a lookahead ahead) completes each one with the reply the pump just
          * produced, runs on, and blocks on the next: progress, not a
          * deadlock -- counting those ended the session (E_DEADLOCK). */
         if (p->m[s].now() != blocked_at[s])
         {
            blocked_at[s] = p->m[s].now();
            blocked_run = 0;
         }
         if (++blocked_run > 4)
            return pair_fail(p, GBALINK_PAIR_E_DEADLOCK, runs, peer);
         break;
      case GBALINK_RUN_OVERRUN:
         return pair_fail(p, GBALINK_PAIR_E_OVERRUN, runs, peer);
      default:
         return pair_fail(p, GBALINK_PAIR_E_RUN, runs, peer);
      }
   }
}

int gbalink_pair_sync(gbalink_pair *p)
{
   unsigned l = p->cfg.local_slot & 1, o = l ^ 1;
   unsigned blocked = 0, runs = 0;

   if (p->error)
      return -1;
   if (!p->m[l].need_begin())
      return 0;
   while (p->frames[o] < p->frames[l] || !p->m[o].need_begin())
   {
      int r;
      if (p->m[o].need_begin())
      {
         uint32_t f = p->frames[o];
         uint16_t k;
         if (!gbalink_pair_has_input(p, o, f) && p->cfg.input &&
             p->cfg.input(p->cfg.user, o, f, &k))
            gbalink_pair_input(p, o, f, k);
         if (!gbalink_pair_has_input(p, o, f))
            return 0;
         p->m[o].frame_begin(p->in_keys[o][f % GBALINK_IN_RING]);
      }
      if (p->last_ran != (int)o)
         p->st.switches++;
      p->last_ran = (int)o;
      p->st.runs[o]++;
      if (++runs > GBALINK_PAIR_STEP_RUNS_MAX)
      {
         p->error = GBALINK_PAIR_E_RUNAWAY;
         return -1;
      }
      p->running = (int)o;
      r = p->m[o].run(~(uint64_t)0);
      p->running = -1;
      {
         gbalink_bus_stats bs;
         gbalink_bus_get_stats(&bs);
         if (bs.overflow)
         {
            p->error = GBALINK_PAIR_E_BUS;
            return -1;
         }
      }
      if (r == GBALINK_RUN_FRAME)
      {
         blocked = 0;
         p->frames[o]++;
         if (p->cfg.on_frame)
            p->cfg.on_frame(p->cfg.user, o, p->frames[o]);
      }
      else if (r == GBALINK_RUN_BLOCKED)
      {
         /* As in step(): the local machine applies what is due and the
          * reply is there -- unless the transfer began in the cycles this
          * machine is now ahead by. */
         p->st.blocked[o]++;
         p->m[l].pump();
         if (++blocked > 4)
            return 0;
      }
      else
      {
         p->error = GBALINK_PAIR_E_RUN;
         return -1;
      }
   }
   return 1;
}
