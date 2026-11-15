/* test_padpair.c -- fe_padpair (one machine, two pads) under the real
 * fe_gblink session, both consoles in one process.
 *
 * Each "console" is a complete session stack (fe_gblink over fe_padpair over
 * a TOY deterministic machine), so what is proved is the adapter and the
 * session's use of it, not any core.  The toy machine is a pair of mixing
 * registers, a RAM and a battery that the pads change, so a wrong pad order,
 * a lost or doubled frame, a wrong start state or a skipped restore all show
 * as different bytes.
 *
 * The network is a deterministic simulation (no sockets, no threads, one
 * display frame a tick): the ordered channel is an ARQ over a lossy radio (a
 * lost message arrives `rto` frames late and holds everything behind it), and
 * the bulk lane is unreliable with loss, duplication and jitter (= reorder).
 * Built with -DPP_LAN it also runs a few scenarios over the REAL stack the
 * Vita uses -- netdrv over transport_lan on loopback sockets, with the fault
 * shim's loss / latency / jitter / reorder -- in real time.
 *
 *   gcc -std=gnu11 -O1 -g -Wall -Wextra -I gpsp/frontend-common \
 *       -o test_padpair gpsp/tools/tests/test_padpair.c \
 *       gpsp/frontend-common/fe_padpair.c gpsp/frontend-common/fe_gblink.c \
 *       gpsp/frontend-common/fe_util.c
 *   ./test_padpair [-v] [name ...]       (exit 0 = all passed)
 *
 *   LAN variant (run from gpsp/, adds the netdrv objects):
 *   gcc -std=gnu11 -O1 -g -DPP_LAN -pthread -I frontend-common -I netdrv \
 *       -o test_padpair_lan tools/tests/test_padpair.c \
 *       frontend-common/fe_padpair.c frontend-common/fe_gblink.c \
 *       frontend-common/fe_util.c netdrv/netdrv.c netdrv/transport_lan.c \
 *       netdrv/nat_hash.c netdrv/nat_stun.c netdrv/nat_turn.c
 *   ./test_padpair_lan lan_loss lan_clean
 */
#include "fe_padpair.h"
#include "fe_gblink.h"
#include "fe_util.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#ifdef PP_LAN
#include "netdrv.h"
#include "nd_thread.h"
#include "transport_lan.h"
#endif

/* ---- what fe_gblink needs from the rest of the program ---------------- */
static int verbose;
static unsigned evt_desync, evt_failed;

void fe_evt(const char *fmt, ...)
{
   va_list a;
   if (strstr(fmt, "gblink_desync"))
      evt_desync++;
   if (strstr(fmt, "gblink_failed"))
      evt_failed++;
   if (!verbose)
      return;
   va_start(a, fmt);
   printf("EVT ");
   vprintf(fmt, a);
   printf("\n");
   va_end(a);
}
void fe_log(const char *fmt, ...) { (void)fmt; }

/* The GB pair's functions: fe_gblink.c links them, a vtable session never
 * calls them (gbdual_destroy(NULL) in fe_gblink_destroy is a no-op). */
uint8_t *gbcore_rom_alloc(size_t n, size_t *alloc) { (void)n; (void)alloc; return NULL; }
gbdual_t *gbdual_create(const gbdual_config_t *c) { (void)c; return NULL; }
void gbdual_destroy(gbdual_t *d) { (void)d; }
int gbdual_advance(gbdual_t *d, gbdual_input_fn f, void *u) { (void)d; (void)f; (void)u; return -1; }
gbcore_t *gbdual_core(const gbdual_t *d, int s) { (void)d; (void)s; return NULL; }
const gbcore_api_t *gbdual_api(int s) { (void)s; return NULL; }
uint64_t gbdual_sync_hash(gbdual_t *d) { (void)d; return 0; }

/* ---- checks ----------------------------------------------------------- */
static int fails, checks;
static const char *scenario;
#define CHECK(c, ...) do { checks++; if (!(c)) { fails++; \
   printf("  FAIL [%s] %s:%d: %s -- ", scenario, "test_padpair.c", __LINE__, #c); \
   printf(__VA_ARGS__); printf("\n"); } } while (0)

/* ---- the toy machine --------------------------------------------------- */
#define FR_MAX 6000
#define BAT_N 256
#define RAM_N 512
typedef struct toy_state
{
   uint64_t a, b;
   uint32_t frame;
   uint8_t ram[RAM_N];
} toy_state;
#define STATE_N (sizeof(toy_state))

typedef struct toy
{
   toy_state s;
   uint8_t bat[BAT_N];
   int id;
   /* knobs */
   int corrupt_at;              /* flip a byte of RAM when s.frame == this */
   int fail_at;                 /* run_frame fails at the Nth session frame */
   int bat_quiet_after;         /* the battery stops changing after this frame */
   /* observations */
   unsigned runs;
   uint16_t pad1[FR_MAX], pad2[FR_MAX];
   uint32_t trace[FR_MAX];      /* CRC of the state after each frame */
   int clock_n;
   int64_t clock_log[8];
   int saved_n;                 /* save_write calls */
   unsigned commits;
   uint8_t *commit_img;
   size_t commit_n;
} toy;

static int toy_run(void *u, uint16_t p1, uint16_t p2)
{
   toy *t = (toy *)u;
   toy_state *s = &t->s;
   if (t->fail_at && (int)t->runs + 1 == t->fail_at)
      return 1;
   if (t->runs < FR_MAX)
   {
      t->pad1[t->runs] = p1;
      t->pad2[t->runs] = p2;
   }
   s->frame++;
   s->a = s->a * 6364136223846793005ull + (uint64_t)p1 * 7919u + s->frame;
   s->b = (s->b ^ (uint64_t)(p2 + 0x9E37u)) * 0x100000001B3ull;
   s->ram[(s->a >> 8) % RAM_N] ^= (uint8_t)(s->b >> 16);
   if ((s->a & 0x1F) == 0 && (int)s->frame < t->bat_quiet_after)
      t->bat[(s->a >> 20) % BAT_N] ^= (uint8_t)(p1 ^ p2 ^ 1u);
   if (t->corrupt_at && (int)s->frame == t->corrupt_at)
      s->ram[7] ^= 1;
   if (t->runs < FR_MAX)
      t->trace[t->runs] = fe_crc32(0, s, sizeof(*s));
   t->runs++;
   return 0;
}
static size_t toy_state_size(void *u) { (void)u; return STATE_N; }
static int toy_state_save(void *u, void *b, size_t n)
{
   if (n != STATE_N) return -1;
   memcpy(b, &((toy *)u)->s, n);
   return 0;
}
static int toy_state_load(void *u, const void *b, size_t n)
{
   if (n != STATE_N) return -1;
   memcpy(&((toy *)u)->s, b, n);
   return 0;
}
static size_t toy_save_size(void *u) { (void)u; return BAT_N; }
static int toy_save_read(void *u, void *b, size_t n)
{
   if (n != BAT_N) return -1;
   memcpy(b, ((toy *)u)->bat, n);
   return 0;
}
static int toy_save_write(void *u, const void *b, size_t n)
{
   if (n != BAT_N) return -1;
   memcpy(((toy *)u)->bat, b, n);
   ((toy *)u)->saved_n++;
   return 0;
}
static void toy_clock(void *u, int64_t seed)
{
   toy *t = (toy *)u;
   if (t->clock_n < 8)
      t->clock_log[t->clock_n] = seed;
   t->clock_n++;
}
static uint32_t toy_cart_crc(void *u) { return fe_crc32(0, ((toy *)u)->bat, BAT_N); }

/* the two players' own games before linking: different progress */
static void toy_init(toy *t, int id, int frame0)
{
   unsigned i;
   memset(t, 0, sizeof(*t));
   t->id = id;
   t->s.a = 0x1234567800ull * (uint64_t)(id + 3) + 11u;
   t->s.b = 0xCAFEF00Dull + (uint64_t)id * 977u;
   t->s.frame = (uint32_t)frame0;
   for (i = 0; i < RAM_N; i++)
      t->s.ram[i] = (uint8_t)(i * (id + 5) + frame0);
   for (i = 0; i < BAT_N; i++)
      t->bat[i] = (uint8_t)(i * 3 + id * 41 + 1);
   t->bat_quiet_after = 1000000;
}

static fe_pad_machine toy_desc(toy *t)
{
   fe_pad_machine m;
   memset(&m, 0, sizeof(m));
   m.user = t;
   m.console = FE_CONSOLE_NES;
   m.rom_min = 0x20;
   m.rom_max = 1u << 20;
   m.save_max = 0x10000;
   m.run_frame = toy_run;
   m.state_size = toy_state_size;
   m.state_save = toy_state_save;
   m.state_load = toy_state_load;
   m.save_size = toy_save_size;
   m.save_read = toy_save_read;
   m.save_write = toy_save_write;
   m.set_clock = toy_clock;
   m.cart_crc = toy_cart_crc;
   return m;
}

/* ---- the network ------------------------------------------------------ */
typedef struct msg { uint64_t due; size_t len; uint8_t *d; struct msg *next; } msg_t;
typedef struct net_cfg
{
   unsigned lat, jit;                 /* frames */
   unsigned arq_loss, arq_rto;        /* % of ordered messages resent; frames late */
   unsigned bulk_loss, bulk_dup, bulk_jit;   /* % lost, % doubled, extra frames */
   uint32_t seed;
} net_cfg;
static net_cfg NC;
static uint64_t tick;
static uint32_t rng;
static msg_t *ord_q[2], *blk_q[2];       /* [to console] */
static uint64_t ord_last[2];
static unsigned ord_n[2];
static uint64_t sent_ord, sent_blk, lost_blk, late_ord;

static uint32_t rnd(void)
{
   rng = rng * 1664525u + 1013904223u;
   return rng >> 8;
}

static void enqueue_sorted(msg_t **head, msg_t *m)
{
   while (*head && (*head)->due <= m->due)
      head = &(*head)->next;
   m->next = *head;
   *head = m;
}

static msg_t *mk(const void *b, size_t n, uint64_t due)
{
   msg_t *m = (msg_t *)calloc(1, sizeof(*m));
   m->d = (uint8_t *)malloc(n);
   memcpy(m->d, b, n);
   m->len = n;
   m->due = due;
   return m;
}

static int sim_send(int from, const void *b, size_t n)
{
   int to = from ^ 1;
   uint64_t due = tick + NC.lat + (NC.jit ? rnd() % (NC.jit + 1) : 0);
   sent_ord++;
   if (NC.arq_loss && rnd() % 100 < NC.arq_loss)
   {
      due += NC.arq_rto;
      late_ord++;
   }
   if (due < ord_last[to])            /* ordered: nothing passes a late one */
      due = ord_last[to];
   ord_last[to] = due;
   enqueue_sorted(&ord_q[to], mk(b, n, due));
   ord_n[to]++;
   return 0;
}

static int sim_bulk(int from, const void *b, size_t n)
{
   int to = from ^ 1, copies = 1;
   sent_blk++;
   if (NC.bulk_loss && rnd() % 100 < NC.bulk_loss)
   {
      lost_blk++;
      return 0;
   }
   if (NC.bulk_dup && rnd() % 100 < NC.bulk_dup)
      copies = 2;
   while (copies--)
      enqueue_sorted(&blk_q[to], mk(b, n,
                     tick + NC.lat + (NC.bulk_jit ? rnd() % (NC.bulk_jit + 1) : 0)));
   return 0;
}

static void net_free(void)
{
   int i;
   for (i = 0; i < 2; i++)
   {
      msg_t *m;
      while ((m = ord_q[i]) != NULL) { ord_q[i] = m->next; free(m->d); free(m); }
      while ((m = blk_q[i]) != NULL) { blk_q[i] = m->next; free(m->d); free(m); }
      ord_n[i] = 0;
      ord_last[i] = 0;
   }
}

/* ---- one console ------------------------------------------------------ */
typedef struct con
{
   int id;
   toy *t;
   fe_gblink *s;
   unsigned frames_done;
   uint8_t *state0;                  /* the own game before linking */
   uint8_t bat0[BAT_N];
   uint8_t *final_save, *final_state;
   size_t final_save_n, final_state_n;
} con_t;

static con_t C[2];
static int (*net_send)(int from, const void *b, size_t n);
static int (*net_bulk)(int from, const void *b, size_t n);
static void (*net_pump)(int id);

static int p_send(void *u, const void *b, size_t n) { return net_send(((con_t *)u)->id, b, n); }
static int sim_room(int id) { (void)id; return 48; }
static int (*net_room)(int id) = sim_room;
static size_t net_payload = 144;
static int p_room(void *u) { return net_room(((con_t *)u)->id); }
static int p_bulk(void *u, const void *b, size_t n) { return net_bulk(((con_t *)u)->id, b, n); }
static int p_find(void *u, uint32_t size, const uint8_t h[0x1C], char *path, size_t cap)
{
   (void)u; (void)size; (void)h; (void)path; (void)cap;
   return 0;
}
static int p_commit(void *u, const void *img, size_t n)
{
   toy *t = ((con_t *)u)->t;
   t->commits++;
   free(t->commit_img);
   t->commit_img = (uint8_t *)malloc(n);
   memcpy(t->commit_img, img, n);
   t->commit_n = n;
   return 0;
}
static uint64_t now_us(void) { return tick * 16667ull; }

static uint16_t btn(int id, uint32_t f)
{
   uint32_t x = (f / 3) * 2654435761u + (uint32_t)id * 40503u + 977u;
   x ^= x >> 13;
   return (uint16_t)((x & 0x3FFu) | 1u);       /* never 0: a missing pad shows */
}

/* ---- scenario description ---------------------------------------------- */
typedef struct scn
{
   const char *name;
   unsigned frames;                /* slot-0 frames before an end is asked */
   unsigned delay, hash_every;
   net_cfg net;
   unsigned absorb, copies, nack, bulk;
   int guest_ends;                 /* the guest asks for the end */
   int rom_differs;                /* the guest's image is not the host's */
   int host_no_state;              /* the host passes no state */
   int guest_gives_state;          /* the guest (wrongly) passes a state */
   int corrupt_guest_at;           /* desync: flip a byte of the guest's machine */
   int host_fails_at;              /* the host's run_frame fails */
   int bat_quiet_after;
   int expect_done;                /* 1 both DONE, 0 both not DONE */
   int expect_ran;                 /* the machines started */
   unsigned max_ticks;
} scn_t;

static char romdir[256];
static char rom_h[300], rom_g[300], rom_x[300];

static void write_rom(const char *p, int flip)
{
   uint8_t b[4096];
   unsigned i;
   FILE *f;
   for (i = 0; i < sizeof(b); i++)
      b[i] = (uint8_t)(i * 31 + (i >> 8) * 7);
   memcpy(b, "NES\x1a PADPAIR TEST", 17);
   if (flip)
      b[2000] ^= 0x40;
   f = fopen(p, "wb");
   if (f) { fwrite(b, 1, sizeof(b), f); fclose(f); }
}

static int g_absorb_wait(void *u, uint32_t waited)
{
   if (waited > 4000)
      return 0;
   net_pump(((con_t *)u)->id);
   return 1;
}

/* Run one scenario to its end; returns after both sessions are destroyed.
 * `hold` (LAN only) is the real-time pacing. */
static toy T[2];
static void (*net_tick_pace)(void);

static void run_scenario(const scn_t *sc)
{
   int i;
   unsigned running_asked = 0;
   uint64_t t_end = 0;
   fe_gblink_stats st[2];
   int state[2];
   memset(C, 0, sizeof(C));
   scenario = sc->name;
   evt_desync = evt_failed = 0;
   toy_init(&T[0], 0, 1234);
   toy_init(&T[1], 1, 77);
   T[0].bat_quiet_after = T[1].bat_quiet_after = sc->bat_quiet_after ? sc->bat_quiet_after : 1500;
   T[1].corrupt_at = sc->corrupt_guest_at;
   T[0].fail_at = sc->host_fails_at;
   NC = sc->net;
   rng = NC.seed ? NC.seed : 12345;
   tick = 0;
   sent_ord = sent_blk = lost_blk = late_ord = 0;
   for (i = 0; i < 2; i++)
   {
      fe_pad_machine d = toy_desc(&T[i]);
      const fe_gblink_machines *v = fe_padpair_machines(&d);
      fe_gblink_platform p;
      fe_gblink_local l;
      uint8_t *st0 = (uint8_t *)malloc(STATE_N);
      CHECK(v != NULL, "machines for console %d", i);
      if (!v)
         return;
      C[i].id = i;
      C[i].t = &T[i];
      toy_state_save(&T[i], st0, STATE_N);
      C[i].state0 = (uint8_t *)malloc(STATE_N);
      memcpy(C[i].state0, st0, STATE_N);
      memcpy(C[i].bat0, T[i].bat, BAT_N);
      memset(&p, 0, sizeof(p));
      memset(&l, 0, sizeof(l));
      p.user = &C[i];
      p.send = p_send;
      p.max_payload = net_payload;
      p.send_room = p_room;
      p.find_rom = p_find;
      p.commit_save = p_commit;
      if (sc->bulk)
      {
         p.bulk_max = FE_GBLINK_BULK_HDR + sc->bulk;
         p.bulk_send = p_bulk;
         p.bulk_rate = 0;
         p.input_copies = sc->copies;
         p.nack = (int)sc->nack;
      }
      p.wait = sc->absorb ? g_absorb_wait : NULL;
      l.is_host = i == 0;
      l.rom_path = (i == 1 && sc->rom_differs) ? rom_x : (i == 0 ? rom_h : rom_g);
      l.console = FE_CONSOLE_NES;
      l.wallclock_now = 1790500000;
      l.input_delay = sc->delay;
      l.hash_interval = sc->hash_every;
      l.machines = v;
      if (i == 0 && !sc->host_no_state)
      {
         l.state = st0;
         l.state_size = STATE_N;
         l.save = T[0].bat;
         l.save_size = BAT_N;
      }
      if (i == 1 && sc->guest_gives_state)
      {
         l.state = st0;
         l.state_size = STATE_N;
      }
      C[i].s = fe_gblink_create(&p, &l);
      free(st0);
      CHECK(C[i].s != NULL, "create %d", i);
      if (!C[i].s)
         return;
      fe_gblink_set_clock(C[i].s, now_us);
   }
   /* a second registration of a machine whose session exists is refused
    * only once it runs; before that it is the same pair */
   fe_gblink_peer(C[0].s, 1);
   fe_gblink_peer(C[1].s, 1);

   for (tick = 0; tick < (sc->max_ticks ? sc->max_ticks : 30000); tick++)
   {
      for (i = 0; i < 2; i++)
      {
         net_pump(i);
         state[i] = fe_gblink_state(C[i].s);
         if (state[i] == FE_GBLINK_RUNNING || state[i] == FE_GBLINK_ENDING)
         {
            if (fe_gblink_step(C[i].s, btn(i, C[i].frames_done)) == FE_GBLINK_FRAME)
               C[i].frames_done++;
         }
         else
            fe_gblink_step(C[i].s, 0);
      }
      for (i = 0; i < 2; i++)
         state[i] = fe_gblink_state(C[i].s);
      if (!running_asked && state[0] == FE_GBLINK_RUNNING && state[1] == FE_GBLINK_RUNNING)
      {
         fe_gblink_get_stats(C[0].s, &st[0]);
         if (st[0].frames[0] >= sc->frames)
         {
            fe_gblink_request_end(C[sc->guest_ends ? 1 : 0].s);
            running_asked = 1;
         }
      }
      if (net_tick_pace)
         net_tick_pace();
      if ((state[0] == FE_GBLINK_DONE || state[0] == FE_GBLINK_FAILED) &&
          (state[1] == FE_GBLINK_DONE || state[1] == FE_GBLINK_FAILED))
      {
         if (!t_end)
            t_end = tick;
         if (tick > t_end + 3)
            break;
      }
   }
   CHECK(t_end != 0, "the session ended (state %d/%d after %llu ticks)", state[0],
         state[1], (unsigned long long)tick);
   for (i = 0; i < 2; i++)
   {
      size_t n;
      const void *p = fe_gblink_final_save(C[i].s, &n);
      if (p) { C[i].final_save = (uint8_t *)malloc(n); memcpy(C[i].final_save, p, n); C[i].final_save_n = n; }
      p = fe_gblink_final_state(C[i].s, &n);
      if (p) { C[i].final_state = (uint8_t *)malloc(n); memcpy(C[i].final_state, p, n); C[i].final_state_n = n; }
      fe_gblink_get_stats(C[i].s, &st[i]);
   }

   /* ---- the verdict ---- */
   if (sc->expect_done)
   {
      uint32_t f, end;
      CHECK(state[0] == FE_GBLINK_DONE && state[1] == FE_GBLINK_DONE,
            "both DONE (got %d/%d, errors %d/%d)", state[0], state[1],
            fe_gblink_error(C[0].s), fe_gblink_error(C[1].s));
      CHECK(T[0].runs == T[1].runs && T[0].runs >= sc->frames,
            "the same number of frames ran on both consoles: %u/%u (asked %u)",
            T[0].runs, T[1].runs, sc->frames);
      end = T[0].runs;
      CHECK(st[0].hashes_sent > 0 && st[0].hashes_matched == st[0].hashes_sent &&
            st[1].hashes_matched == st[1].hashes_sent,
            "every periodic hash matched: host %u/%u guest %u/%u",
            st[0].hashes_matched, st[0].hashes_sent, st[1].hashes_matched,
            st[1].hashes_sent);
      CHECK(st[0].hashes_sent >= sc->frames / (sc->hash_every ? sc->hash_every : 60) - 1,
            "hashes were exchanged (%u)", st[0].hashes_sent);
      for (f = 0; f < end && f < FR_MAX; f++)
      {
         uint16_t e1 = f < sc->delay ? 0 : btn(0, f - sc->delay);
         uint16_t e2 = f < sc->delay ? 0 : btn(1, f - sc->delay);
         if (T[0].pad1[f] != e1 || T[0].pad2[f] != e2 ||
             T[1].pad1[f] != e1 || T[1].pad2[f] != e2 ||
             T[0].trace[f] != T[1].trace[f])
         {
            CHECK(0, "frame %u: host pads %04x/%04x guest pads %04x/%04x "
                  "want %04x/%04x trace %08x/%08x", f, T[0].pad1[f],
                  T[0].pad2[f], T[1].pad1[f], T[1].pad2[f], e1, e2,
                  T[0].trace[f], T[1].trace[f]);
            break;
         }
      }
      checks++;                      /* the per-frame pad / trace comparison */
      /* the host commits its own battery; the guest never commits */
      CHECK(T[0].commits == 1 && T[1].commits == 0,
            "commits host %u guest %u", T[0].commits, T[1].commits);
      CHECK(C[0].final_save_n == BAT_N && C[1].final_save_n == 0 &&
            C[1].final_state_n == 0,
            "final images host save %zu guest save %zu state %zu",
            C[0].final_save_n, C[1].final_save_n, C[1].final_state_n);
      CHECK(T[0].commit_n == BAT_N && !memcmp(T[0].commit_img, C[0].final_save, BAT_N),
            "the committed save is the captured one");
      /* the host's machine stays exactly where the session ended */
      CHECK(C[0].final_state_n == STATE_N &&
            !memcmp(C[0].final_state, &T[0].s, STATE_N),
            "the host's machine is the final state");
      CHECK(!memcmp(T[0].bat, C[0].final_save, BAT_N),
            "the host's machine holds the committed battery");
      CHECK(T[0].clock_n >= 1 && T[0].clock_log[0] == 1790500000,
            "the clock seed is the host's (%lld)", (long long)T[0].clock_log[0]);
      CHECK(T[0].s.frame == 1234u + end,
            "the host's machine ran %u frames on from its own state", end);
      CHECK(!sc->bulk || st[0].inputs_fast + st[1].inputs_fast > 0 ||
            !sc->copies, "redundant input copies were used");
   }
   else
   {
      CHECK(state[0] != FE_GBLINK_DONE && state[1] != FE_GBLINK_DONE,
            "neither DONE (%d/%d)", state[0], state[1]);
      CHECK(T[0].commits == 0 && T[1].commits == 0,
            "nothing committed: host %u guest %u", T[0].commits, T[1].commits);
      if (sc->corrupt_guest_at)
      {
         /* the flipped byte is found by the periodic hash, on both consoles,
          * within one hash interval of the flip */
         CHECK(evt_desync >= 1 && (fe_gblink_error(C[0].s) == FE_GBLINK_E_DESYNC ||
               fe_gblink_error(C[1].s) == FE_GBLINK_E_DESYNC),
               "a desync was reported (%u events, errors %d/%d)", evt_desync,
               fe_gblink_error(C[0].s), fe_gblink_error(C[1].s));
         CHECK(T[1].runs >= (unsigned)(sc->corrupt_guest_at - 1234) &&
               T[1].runs <= (unsigned)(sc->corrupt_guest_at - 1234) + 2 * sc->hash_every + sc->delay + 4,
               "stopped within two hash intervals of the flip (%u frames)", T[1].runs);
      }
      if (sc->host_fails_at)
         CHECK(fe_gblink_error(C[0].s) == FE_GBLINK_E_START, "host error %d",
               fe_gblink_error(C[0].s));
      CHECK(!sc->expect_ran ? T[0].runs == 0 && T[1].runs == 0 : 1,
            "no frame ran (%u/%u)", T[0].runs, T[1].runs);
   }
   /* ---- end of session: both sessions destroyed, then each console's own
    * game must be back ---- */
   for (i = 0; i < 2; i++)
   {
      fe_gblink_destroy(C[i].s);
      C[i].s = NULL;
   }
   if (sc->expect_done)
   {
      /* a guest's machine is its own game again, byte for byte */
      toy_state g;
      memcpy(&g, &T[1].s, sizeof(g));
      CHECK(!memcmp(&T[1].s, C[1].state0, STATE_N),
            "the guest's own state is back (frame %u, want %u)", g.frame, 77u);
      CHECK(!memcmp(T[1].bat, C[1].bat0, BAT_N), "the guest's own battery is back");
      CHECK(T[1].clock_n >= 2 && T[1].clock_log[T[1].clock_n - 1] == 0,
            "the clock hook is released (%d calls)", T[1].clock_n);
   }
   else if (sc->expect_ran || T[0].runs || T[1].runs)
   {
      /* a failed session: BOTH machines are back on their own games */
      CHECK(!memcmp(&T[0].s, C[0].state0, STATE_N) &&
            !memcmp(T[0].bat, C[0].bat0, BAT_N),
            "the host's own game is back after a failed session");
      CHECK(!memcmp(&T[1].s, C[1].state0, STATE_N) &&
            !memcmp(T[1].bat, C[1].bat0, BAT_N),
            "the guest's own game is back after a failed session");
   }
   else
   {
      CHECK(!memcmp(&T[0].s, C[0].state0, STATE_N) &&
            !memcmp(&T[1].s, C[1].state0, STATE_N) &&
            !memcmp(T[0].bat, C[0].bat0, BAT_N) &&
            !memcmp(T[1].bat, C[1].bat0, BAT_N),
            "neither machine was touched");
      CHECK(T[0].clock_n == 0 && T[1].clock_n == 0, "no clock was set");
      CHECK(T[0].saved_n == 0 && T[1].saved_n == 0, "no battery was written");
   }
   if (verbose)
      printf("  [%s] ticks=%llu frames=%u/%u stalls=%u/%u hashes=%u/%u ord=%llu(+%llu late) "
             "bulk=%llu(-%llu) fast=%u/%u\n", sc->name, (unsigned long long)tick,
             T[0].runs, T[1].runs, st[0].stalls, st[1].stalls, st[0].hashes_matched,
             st[0].hashes_sent, (unsigned long long)sent_ord,
             (unsigned long long)late_ord, (unsigned long long)sent_blk,
             (unsigned long long)lost_blk, st[0].inputs_fast, st[1].inputs_fast);
   for (i = 0; i < 2; i++)
   {
      free(C[i].state0);
      free(C[i].final_save);
      free(C[i].final_state);
      free(T[i].commit_img);
      T[i].commit_img = NULL;
   }
   net_free();
}

/* ---- simulated-net hooks ------------------------------------------------ */
static void sim_pump(int id)
{
   msg_t *m;
   while ((m = ord_q[id]) != NULL && m->due <= tick)
   {
      ord_q[id] = m->next;
      ord_n[id]--;
      if (C[id].s)
         fe_gblink_receive(C[id].s, m->d, m->len);
      free(m->d);
      free(m);
   }
   while ((m = blk_q[id]) != NULL && m->due <= tick)
   {
      blk_q[id] = m->next;
      if (C[id].s)
         fe_gblink_receive_bulk(C[id].s, m->d, m->len);
      free(m->d);
      free(m);
   }
}

/* ---- the scenarios -------------------------------------------------------- */
static const scn_t SC[] = {
   { .name = "clean", .frames = 2000, .delay = 4, .hash_every = 60,
     .net = { .lat = 1 }, .expect_done = 1, .expect_ran = 1 },
   { .name = "delay12_hash30", .frames = 1200, .delay = 12, .hash_every = 30,
     .net = { .lat = 3, .jit = 2, .seed = 7 }, .expect_done = 1, .expect_ran = 1 },
   { .name = "delay0", .frames = 600, .delay = 0, .hash_every = 60,
     .net = { .lat = 0 }, .expect_done = 1, .expect_ran = 1 },
   { .name = "loss_reorder", .frames = 2000, .delay = 8, .hash_every = 60,
     .net = { .lat = 2, .jit = 2, .arq_loss = 8, .arq_rto = 5, .bulk_loss = 20,
              .bulk_dup = 5, .bulk_jit = 4, .seed = 99 },
     .bulk = 512, .copies = 4, .nack = 1, .expect_done = 1, .expect_ran = 1 },
   { .name = "loss_ordered_only", .frames = 1500, .delay = 6, .hash_every = 45,
     .net = { .lat = 2, .jit = 1, .arq_loss = 12, .arq_rto = 7, .seed = 5 },
     .expect_done = 1, .expect_ran = 1 },
   { .name = "guest_ends", .frames = 900, .delay = 4, .hash_every = 60, .guest_ends = 1,
     .net = { .lat = 1, .arq_loss = 3, .arq_rto = 4, .seed = 3 },
     .expect_done = 1, .expect_ran = 1 },
   { .name = "desync_detected", .frames = 2000, .delay = 4, .hash_every = 60,
     .net = { .lat = 1 }, .corrupt_guest_at = 1234 + 400, .expect_done = 0,
     .expect_ran = 1 },
   { .name = "host_machine_fails", .frames = 2000, .delay = 4, .hash_every = 60,
     .net = { .lat = 1 }, .host_fails_at = 200, .expect_done = 0, .expect_ran = 1 },
   { .name = "other_image_refused", .frames = 100, .delay = 4, .hash_every = 60,
     .net = { .lat = 1 }, .rom_differs = 1, .expect_done = 0, .expect_ran = 0,
     .max_ticks = 600 },
   { .name = "host_without_state", .frames = 100, .delay = 4, .hash_every = 60,
     .net = { .lat = 1 }, .host_no_state = 1, .expect_done = 0, .expect_ran = 0,
     .max_ticks = 600 },
   { .name = "guest_with_state_refused", .frames = 100, .delay = 4, .hash_every = 60,
     .net = { .lat = 1 }, .guest_gives_state = 1, .expect_done = 0, .expect_ran = 0,
     .max_ticks = 600 },
};

static void test_api(void)
{
   toy t[6];
   fe_pad_machine d;
   const fe_gblink_machines *v, *v2;
   int i;
   scenario = "api";
   CHECK(fe_padpair_machines(NULL) == NULL, "NULL description");
   toy_init(&t[0], 0, 1);
   d = toy_desc(&t[0]);
   d.run_frame = NULL;
   CHECK(fe_padpair_machines(&d) == NULL, "no run_frame");
   d = toy_desc(&t[0]);
   d.save_write = NULL;
   CHECK(fe_padpair_machines(&d) == NULL, "no save_write");
   d = toy_desc(&t[0]);
   d.rom_max = 1;
   CHECK(fe_padpair_machines(&d) == NULL, "rom_max below rom_min");
   d = toy_desc(&t[0]);
   v = fe_padpair_machines(&d);
   CHECK(v && v->console == FE_CONSOLE_NES && v->rom_min >= 0x1C &&
         v->header_off == 0 && v->partner_rom == NULL && v->start &&
         v->advance && v->stop, "vtable shape");
   v2 = fe_padpair_machines(&d);
   CHECK(v2 == v, "the same user gets the same pair");
   CHECK(fe_padpair_restored(v) == 1 && fe_padpair_restored(NULL) == 1,
         "restored() before any session");
   /* slot 1 has nothing */
   CHECK(v->state_size(v->user, 1) == 0 && v->save_size(v->user, 1) == 0 &&
         v->state_size(v->user, 0) == STATE_N && v->save_size(v->user, 0) == BAT_N,
         "slot 0 is the machine, slot 1 is empty");
   CHECK(v->cart_crc(v->user, 1) == 0 && v->cart_crc(v->user, 0) == toy_cart_crc(&t[0]),
         "cart crc of slot 0 only");
   CHECK(v->state_save(v->user, 1, NULL, 0) == 0 && v->save_read(v->user, 1, NULL, 0) == 0 &&
         v->state_save(v->user, 1, t, 4) != 0, "slot 1 reads give nothing");
   /* pool: more machines than slots, none running */
   for (i = 1; i < 6; i++)
   {
      toy_init(&t[i], i, 1);
      d = toy_desc(&t[i]);
      CHECK(fe_padpair_machines(&d) != NULL, "machine %d registers (a finished pair is reused)", i);
   }
   /* start() rejects a payload of the wrong size without touching anything */
   d = toy_desc(&t[0]);
   v = fe_padpair_machines(&d);
   {
      uint8_t st[STATE_N], sv[BAT_N], mine[STATE_N];
      const uint8_t *sp[2] = { st, NULL };
      const uint8_t *vp[2] = { sv, NULL };
      size_t sn[2] = { STATE_N - 1, 0 }, vn[2] = { BAT_N, 0 };
      toy_state_save(&t[0], mine, STATE_N);
      memset(st, 0, sizeof(st));
      memset(sv, 0, sizeof(sv));
      CHECK(v->start(v->user, 1, 1, 5, sp, sn, vp, vn, NULL, NULL) != 0,
            "a short state is refused");
      sn[0] = STATE_N;
      vn[0] = BAT_N - 1;
      CHECK(v->start(v->user, 1, 1, 5, sp, sn, vp, vn, NULL, NULL) != 0,
            "a short battery image is refused");
      sp[0] = NULL;
      vn[0] = BAT_N;
      CHECK(v->start(v->user, 1, 1, 5, sp, sn, vp, vn, NULL, NULL) != 0,
            "no state is refused");
      CHECK(!memcmp(&t[0].s, mine, STATE_N) && t[0].clock_n == 0 && t[0].saved_n == 0,
            "refusals touched nothing");
   }
}


#ifdef PP_LAN
/* NOTE the fault shim here is mild on purpose (2 % loss, 25 ms of jitter at
 * most).  With input_copies on, the host's buttons past the agreed end frame
 * can reach the guest on the unreliable lane before the ordered END_AT does
 * (fe_gblink.c take_resend's comment says the same of BK_RESEND): if END_AT
 * is held up for more than END_MARGIN + 2 * delay frames the guest passes the
 * end and the session ends safely as `end_late` (a DESYNC verdict, nothing
 * saved).  At 6 % loss and 40 ms jitter that happened in 2 runs of 7; a flaky
 * test proves nothing, and the simulated net covers heavier loss.
 *
 * ---- the real stack: netdrv over transport_lan, loopback sockets ---------
 * (netdrv/tests/sesslan.c's plumbing): the fault shim's loss, latency and
 * jitter act on every datagram, ordered ones (netdrv's ARQ resends) and the
 * bulk lane's alike, so reordering is real.  Real time: one display frame (16.7 ms) a tick. */
#define SL_PROTO "GBAdhoc GB link v2"
static struct sl_con
{
   lan_transport *tp;
   netdrv *nd;
   int active, peers;
   uint16_t peer_id;
} sl[2];

static void sl_deliver(void *u, const void *buf, size_t len, uint8_t src)
{
   int id = (struct sl_con *)u == &sl[1];
   (void)src;
   if (C[id].s)
      fe_gblink_receive(C[id].s, buf, len);
}
static void sl_started(void *u, uint8_t id) { (void)id; ((struct sl_con *)u)->active = 1; }
static int sl_connected(void *u, uint8_t id)
{
   ((struct sl_con *)u)->peers++;
   ((struct sl_con *)u)->peer_id = id;
   return 0;
}
static void sl_disconnected(void *u, uint8_t id)
{
   int i = (struct sl_con *)u == &sl[1];
   (void)id;
   ((struct sl_con *)u)->peers--;
   if (C[i].s)
      fe_gblink_peer(C[i].s, 0);
}
static void sl_stopped(void *u, int reason)
{
   int i = (struct sl_con *)u == &sl[1];
   (void)reason;
   ((struct sl_con *)u)->active = 0;
   if (C[i].s)
      fe_gblink_peer(C[i].s, 0);
}
static void lan_pump(int id)
{
   uint8_t bk[LAN_BULK_MAX];
   int n, guard = 256;
   netdrv_pump(sl[id].nd, nd_now_us());
   while (guard-- && (n = lan_transport_bulk_recv(sl[id].tp, bk, sizeof(bk))) > 0)
      if (C[id].s)
         fe_gblink_receive_bulk(C[id].s, bk, (size_t)n);
}
static int lan_send(int from, const void *b, size_t n)
{
   return netdrv_send(sl[from].nd, ND_RELIABLE | ND_FLUSH_HINT, b, n, sl[from].peer_id);
}
static int pl_room(int id) { return netdrv_send_capacity(sl[id].nd, sl[id].peer_id); }
static int lan_bulk(int from, const void *b, size_t n)
{
   uint8_t mac[6];
   if (netdrv_peer_mac(sl[from].nd, sl[from].peer_id, mac) != 0)
      return -1;
   return lan_transport_bulk_send(sl[from].tp, mac, b, n);
}
static uint64_t lan_next;
static unsigned lan_frame_us = 16667;
static void lan_pace(void)
{
   lan_next += lan_frame_us;
   while (nd_now_us() < lan_next)
      nd_sleep_us(500);
}

static int lan_up(uint16_t base, int loss, int dup, int lat, int jit, int bulk)
{
   lan_config lc;
   lan_scan_config sc;
   lan_room_info info;
   lan_room rooms[4];
   nd_callbacks cb;
   nd_config nc;
   nd_transport tp;
   uint32_t compat = lan_compat_hash(SL_PROTO, "padpair");
   uint64_t t0;
   int i, n;
   memset(sl, 0, sizeof(sl));
   memset(&lc, 0, sizeof(lc));
   lc.mode = LAN_MODE_HOST;
   lc.disc_port = base;
   lc.flags = LAN_F_LOOPBACK | LAN_F_IO_KEEPALIVE;
   sl[0].tp = lan_transport_create(&lc);
   if (!sl[0].tp)
      return -1;
   memset(&info, 0, sizeof(info));
   snprintf(info.name, sizeof(info.name), "PAD07LINK");
   info.number = 7;
   info.kind = LAN_KIND_GB_CABLE;
   info.players = 1;
   info.max_players = 2;
   info.flags = LAN_ROOM_F_OPEN;
   snprintf(info.nick, sizeof(info.nick), "host");
   snprintf(info.title, sizeof(info.title), "PAD TEST");
   snprintf(info.code, sizeof(info.code), "PADP");
   info.compat = compat;
   lan_transport_announce(sl[0].tp, &info);
   memset(&sc, 0, sizeof(sc));
   sc.disc_port = base;
   sc.compat = compat;
   sc.flags = LAN_F_LOOPBACK;
   n = lan_discovery_scan(&sc, rooms, 4, 400);
   if (n != 1)
      return -2;
   memset(&lc, 0, sizeof(lc));
   lc.mode = LAN_MODE_JOIN;
   lc.disc_port = base;
   lc.flags = LAN_F_LOOPBACK | LAN_F_IO_KEEPALIVE;
   lc.target_ip_be = rooms[0].ip_be;
   lc.target_port = rooms[0].port;
   sl[1].tp = lan_transport_create(&lc);
   if (!sl[1].tp)
      return -3;
   for (i = 0; i < 2; i++)
      lan_transport_set_fault(sl[i].tp, loss, dup, lat, jit, 0x5E55 + (uint32_t)i);
   for (i = 0; i < 2; i++)
   {
      lan_transport_iface(sl[i].tp, &tp);
      memset(&cb, 0, sizeof(cb));
      cb.user = &sl[i];
      cb.deliver = sl_deliver;
      cb.session_started = sl_started;
      cb.peer_connected = sl_connected;
      cb.peer_disconnected = sl_disconnected;
      cb.session_stopped = sl_stopped;
      memset(&nc, 0, sizeof(nc));
      nc.protocol = SL_PROTO;
      nc.nick = i ? "guest" : "host";
      nc.join_nonce = 0xA000 + (uint32_t)i;
      sl[i].nd = netdrv_create(&tp, &cb, &nc);
      if (!sl[i].nd || (i ? netdrv_join(sl[i].nd) : netdrv_host(sl[i].nd)))
         return -4;
   }
   t0 = nd_now_us();
   while (!(sl[0].peers == 1 && sl[1].peers == 1 && sl[1].active) &&
          nd_now_us() - t0 < 10000000ull)
   {
      lan_pump(0);
      lan_pump(1);
      nd_sleep_us(1000);
   }
   if (!(sl[0].peers == 1 && sl[1].peers == 1))
      return -5;
   if (bulk)
      for (i = 0; i < 2; i++)
         if (lan_transport_bulk_open(sl[i].tp, 128, FE_GBLINK_BULK_MAGIC) != 0)
            return -6;
   return 0;
}

static void lan_down(void)
{
   int i;
   for (i = 0; i < 2; i++)
   {
      if (sl[i].nd) netdrv_destroy(sl[i].nd);
      if (sl[i].tp) lan_transport_destroy(sl[i].tp);
      sl[i].nd = NULL;
      sl[i].tp = NULL;
   }
}

static void pp_lan_main(int argc, char **argv)
{
   static const struct { const char *name; int loss, dup, lat, jit; scn_t sc; } L[] = {
      { "lan_clean", 0, 0, 0, 0,
        { .name = "lan_clean", .frames = 500, .delay = 6, .hash_every = 60,
          .expect_done = 1, .expect_ran = 1, .max_ticks = 6000 } },
      { "lan_loss", 2, 2, 8, 17,
        { .name = "lan_loss", .frames = 500, .delay = 8, .hash_every = 60,
          .bulk = 1000, .copies = 4, .nack = 1, .absorb = 1,
          .expect_done = 1, .expect_ran = 1, .max_ticks = 9000 } },
   };
   unsigned k;
   int i;
   net_send = lan_send;
   net_bulk = lan_bulk;
   net_pump = lan_pump;
   net_room = pl_room;
   net_payload = ND_MAX_PAYLOAD;
   net_tick_pace = lan_pace;
   for (k = 0; k < sizeof(L) / sizeof(L[0]); k++)
   {
      int want = 1, rc;
      for (i = 1; i < argc; i++)
         if (strcmp(argv[i], "-v"))
            want = 0;
      for (i = 1; i < argc; i++)
         if (!strcmp(argv[i], L[k].name))
            want = 1;
      if (!want)
         continue;
      scenario = L[k].name;
      rc = lan_up((uint16_t)(31000 + (getpid() % 1500) * 8 + k * 3), L[k].loss,
                  L[k].dup, L[k].lat, L[k].jit, L[k].sc.bulk != 0);
      CHECK(rc == 0, "the network came up (%d)", rc);
      if (rc)
      {
         lan_down();
         continue;
      }
      lan_next = nd_now_us();
      run_scenario(&L[k].sc);
      lan_down();
   }
}
#endif

int main(int argc, char **argv)
{
   int i, j, ran = 0, want_any = 0, lan_only = 0;
   char d[] = "/tmp/padpairXXXXXX";
   for (i = 1; i < argc; i++)
      if (!strcmp(argv[i], "-v"))
         verbose = 1;
      else
         want_any = 1;
   for (i = 1; i < argc; i++)
      lan_only = !strncmp(argv[i], "lan_", 4) ? 1 : lan_only;
   if (lan_only)
      want_any = 1;
   setvbuf(stdout, NULL, _IOLBF, 0);
   if (!mkdtemp(d))
      return 2;
   snprintf(romdir, sizeof(romdir), "%s", d);
   snprintf(rom_h, sizeof(rom_h), "%s/h.rom", d);
   snprintf(rom_g, sizeof(rom_g), "%s/g.rom", d);
   snprintf(rom_x, sizeof(rom_x), "%s/x.rom", d);
   write_rom(rom_h, 0);
   write_rom(rom_g, 0);
   write_rom(rom_x, 1);
   net_send = sim_send;
   net_bulk = sim_bulk;
   net_pump = sim_pump;
   for (j = 0; j < (int)(sizeof(SC) / sizeof(SC[0])); j++)
   {
      int want = !want_any;
      if (want && lan_only)
         continue;
      for (i = 1; i < argc; i++)
         if (!strcmp(argv[i], SC[j].name))
            want = 1;
      if (!want)
         continue;
      ran++;
      run_scenario(&SC[j]);
   }
   {
      int api = !want_any;
      for (i = 1; i < argc; i++)
         if (!strcmp(argv[i], "api"))
            api = 1;
      if (api)
      {
         test_api();
         ran++;
      }
   }
#ifdef PP_LAN
   pp_lan_main(argc, argv);
#endif
   remove(rom_h); remove(rom_g); remove(rom_x); rmdir(d);
   printf("test_padpair: %d scenario(s), %d checks, %d failed\n", ran, checks, fails);
   return fails ? 1 : 0;
}
