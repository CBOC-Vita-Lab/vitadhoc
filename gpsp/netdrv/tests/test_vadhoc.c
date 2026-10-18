/* test_vadhoc.c -- transport_vadhoc.c's own logic, on a PC.
 *
 * The transport is built unchanged with -DVADHOC_HOST_FAKE, against
 * tests/vadhoc_fake.c: the SCE calls it makes, implemented over UDP loopback
 * with the semantics the transport ASSUMES (the PSP library's).  What this
 * proves: the bring-up ladder and its unwinding, the dialog state machine,
 * the RX/TX threads and rings, the bulk lane, the fault shim and the
 * broadcast fallback are right GIVEN those semantics, and a whole netdrv
 * session runs over them.  What it cannot prove: that a Vita behaves like
 * the fake.  That list is at the top of transport_vadhoc.c.
 *
 * The transport is one static instance per process, like the PSP's, so the
 * two consoles of a session are two processes (fork).
 */
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include "netdrv.h"
#include "nd_thread.h"
#include "transport_vadhoc.h"
#include "vadhoc_fake.h"

static int g_fail;
#define CHECK(cond, ...) do { \
   if (!(cond)) { \
      g_fail++; \
      printf("  FAIL %s:%d: %s -- ", __FILE__, __LINE__, #cond); \
      printf(__VA_ARGS__); \
      printf("\n"); \
   } } while (0)

static uint16_t g_base;

static int count_dir(const char *path)
{
   DIR *d = opendir(path);
   struct dirent *e;
   int n = 0;
   if (!d)
      return -1;
   while ((e = readdir(d)))
      if (e->d_name[0] != '.')
         n++;
   closedir(d);
   return n;
}

static int run_dialog(int mode, uint32_t timeout_us)
{
   int st, frames = 0;
   int rc = vadhoc_dialog_start(mode, timeout_us);
   if (rc != VADHOC_OK)
      return -100 + rc;
   /* one poll per "frame", as the main loop does */
   while ((st = vadhoc_dialog_poll()) == VADHOC_DLG_RUNNING && frames++ < 1000)
      nd_sleep_us(500);
   vadhoc_dialog_end();
   return st;
}

/* ================================================================== ladder == */

static void test_ladder(void)
{
   static const int peers[] = { 2 };
   vadhoc_fake_opts o = { 1, 5, SCE_COMMON_DIALOG_RESULT_OK, 0, 0 };
   nd_transport tp;
   vadhoc_peer pl[4];
   uint8_t mac[6];
   char why[160] = "", group[9], id[10];
   unsigned tmo = 0;
   int mode = 0, n, fds0, thr0, i;

   printf("[ladder: order, refusals, dialog outcomes, balanced teardown]\n");
   vadhoc_fake_setup(1, g_base, peers, 1);
   vadhoc_fake_options(&o);

   /* out of order */
   CHECK(vadhoc_dialog_start(VADHOC_DIALOG_CONN, 0) == VADHOC_ERR_STATE,
         "dialog before net_init is refused");
   CHECK(vadhoc_transport_open() == VADHOC_ERR_STATE,
         "open before net_init is refused");
   CHECK(vadhoc_dialog_poll() == VADHOC_DLG_IDLE, "no dialog: IDLE");
   CHECK(vadhoc_transport_connected() == 0, "not connected when down");

   CHECK(vadhoc_net_init(NULL) == VADHOC_OK, "net_init (%s, %08x)",
         vadhoc_stage(), vadhoc_last_sce_error());
   CHECK(vadhoc_net_init(NULL) == VADHOC_ERR_ALREADY, "second init refused");
   CHECK(vadhoc_transport_open() == VADHOC_ERR_STATE,
         "open before the dialog is refused");

   /* the player cancels */
   o.dialog_result = SCE_COMMON_DIALOG_RESULT_USER_CANCELED;
   vadhoc_fake_options(&o);
   CHECK(run_dialog(VADHOC_DIALOG_JOIN, 30000000) == VADHOC_DLG_CANCELLED,
         "cancel is CANCELLED");
   CHECK(vadhoc_transport_open() == VADHOC_ERR_STATE,
         "open after a cancelled dialog is refused");

   /* the dialog fails (no cell found: some SCE error in the result) */
   o.dialog_result = (int)0x80410B04;
   vadhoc_fake_options(&o);
   CHECK(run_dialog(VADHOC_DIALOG_JOIN, 5000000) == VADHOC_DLG_ERROR &&
         vadhoc_last_sce_error() == 0x80410B04u, "failure is ERROR + its code");

   /* abort mid-way (the application quits) */
   o.dialog_result = SCE_COMMON_DIALOG_RESULT_OK;
   o.dialog_frames = 1000000;
   vadhoc_fake_options(&o);
   CHECK(vadhoc_dialog_start(VADHOC_DIALOG_CREATE, 0) == VADHOC_OK, "start");
   CHECK(vadhoc_dialog_start(VADHOC_DIALOG_CREATE, 0) == VADHOC_ERR_ALREADY,
         "one dialog at a time");
   CHECK(vadhoc_dialog_poll() == VADHOC_DLG_RUNNING, "running");
   CHECK(vadhoc_transport_open() == VADHOC_ERR_STATE,
         "open while the dialog runs is refused");
   vadhoc_dialog_abort();
   CHECK(vadhoc_dialog_poll() == VADHOC_DLG_CANCELLED, "abort ends CANCELLED");
   CHECK(vadhoc_dialog_poll() == VADHOC_DLG_CANCELLED, "the outcome is latched");
   vadhoc_dialog_end();

   /* the real thing: a named room, as host */
   o.dialog_frames = 5;
   o.dialog_result = SCE_COMMON_DIALOG_RESULT_OK;
   vadhoc_fake_options(&o);
   vadhoc_dialog_set_group("GC07AMKE");
   CHECK(run_dialog(VADHOC_DIALOG_CREATE, 0) == VADHOC_DLG_OK, "dialog OK");
   vadhoc_fake_last_dialog(&mode, group, id, &tmo);
   CHECK(mode == SCE_NETCHECK_DIALOG_MODE_PSP_ADHOC_CREATE &&
         !strcmp(group, "GC07AMKE") && !strcmp(id, VADHOC_ADHOC_ID_DEFAULT),
         "dialog got mode %d group '%s' id '%s'", mode, group, id);
   CHECK(run_dialog(VADHOC_DIALOG_JOIN, 60000000) == VADHOC_DLG_OK, "again");
   vadhoc_fake_last_dialog(&mode, group, id, &tmo);
   CHECK(mode == SCE_NETCHECK_DIALOG_MODE_PSP_ADHOC_JOIN && tmo == 60000000u,
         "JOIN mode and its timeout reach the dialog");

   fds0 = thr0 = 0;
   for (i = 0; i < 31; i++)
   {
      if (i == 1)
      {
         /* baseline after one whole cycle: a sanitizer runtime starts a
          * helper thread of its own with the process's first thread */
         fds0 = count_dir("/proc/self/fd");
         thr0 = count_dir("/proc/self/task");
      }
      vadhoc_set_tx_thread(i & 1);
      if (i)
         CHECK(run_dialog(VADHOC_DIALOG_CONN, 0) == VADHOC_DLG_OK, "dialog %d", i);
      CHECK(vadhoc_transport_open() == VADHOC_OK, "open %d (%s %08x)", i,
            vadhoc_stage(), vadhoc_last_sce_error());
      CHECK(vadhoc_transport_open() == VADHOC_ERR_ALREADY, "second open");
      if (i == 0)
      {
         memset(&tp, 0xA5, sizeof(tp));
         vadhoc_transport_iface(&tp);
         CHECK(tp.send_to && tp.broadcast && tp.recv && tp.local_addr &&
               tp.pending, "full vtable incl. pending");
         CHECK(tp.ctx == NULL && tp.pending(NULL) == 0, "idle: nothing pending");
         tp.local_addr(NULL, mac);
         CHECK(mac[0] == 0x02 && mac[5] == 1, "local MAC from the stack");
         vadhoc_local_mac(mac);
         CHECK(mac[5] == 1, "vadhoc_local_mac");
         CHECK(vadhoc_transport_connected() == 1, "connected");
         n = vadhoc_peers(pl, 4);
         CHECK(n == 1 && pl[0].mac[5] == 2 && !strcmp(pl[0].nick, "Vita02"),
               "peer list: %d '%s'", n, n > 0 ? pl[0].nick : "");
         CHECK(vadhoc_bulk_open(16, "GBLK") == 0, "bulk open");
         CHECK(vadhoc_bulk_send(pl[0].mac, "GBLKx", VADHOC_BULK_MAX + 1) < 0,
               "over-long bulk refused");
      }
      vadhoc_transport_close();
      CHECK(vadhoc_transport_connected() == 0, "closed: not connected");
      {
         uint8_t b[8] = { 0 };
         CHECK(tp.send_to(NULL, mac, b, 8) < 0 && tp.broadcast(NULL, b, 8) < 0,
               "a closed transport refuses sends");
      }
   }
   CHECK(count_dir("/proc/self/fd") == fds0 && count_dir("/proc/self/task") == thr0,
         "30 open/close cycles: fds %d->%d threads %d->%d", fds0,
         count_dir("/proc/self/fd"), thr0, count_dir("/proc/self/task"));

   vadhoc_net_term();
   vadhoc_net_term();                     /* idempotent */
   CHECK(vadhoc_fake_balance(why, sizeof(why)) == 0, "teardown: %s", why);

   /* term with a dialog still open and a transport still open */
   CHECK(vadhoc_net_init("ABCD12345") == VADHOC_OK, "init with an id");
   CHECK(run_dialog(VADHOC_DIALOG_CONN, 0) == VADHOC_DLG_OK, "dialog");
   vadhoc_fake_last_dialog(NULL, NULL, id, NULL);
   CHECK(!strcmp(id, "ABCD12345"), "the caller's ad hoc id is used ('%s')", id);
   CHECK(vadhoc_transport_open() == VADHOC_OK, "open");
   vadhoc_net_term();
   CHECK(vadhoc_fake_balance(why, sizeof(why)) == 0, "term from OPEN: %s", why);
   CHECK(vadhoc_net_init("short") == VADHOC_OK, "a bad id falls back");
   o.dialog_frames = 1000000;
   vadhoc_fake_options(&o);
   CHECK(vadhoc_dialog_start(VADHOC_DIALOG_CONN, 0) == VADHOC_OK, "start");
   vadhoc_fake_last_dialog(NULL, NULL, id, NULL);
   CHECK(!strcmp(id, VADHOC_ADHOC_ID_DEFAULT), "default id ('%s')", id);
   vadhoc_net_term();
   CHECK(vadhoc_fake_balance(why, sizeof(why)) == 0,
         "term with the dialog running: %s", why);

   /* the stack was already up (net_vita.c / newlib): use it, leave it up */
   o.dialog_frames = 5;
   o.stack_already_up = 1;
   vadhoc_fake_options(&o);
   CHECK(vadhoc_net_init(NULL) == VADHOC_OK, "init on a running stack (%s %08x)",
         vadhoc_stage(), vadhoc_last_sce_error());
   CHECK(run_dialog(VADHOC_DIALOG_CONN, 0) == VADHOC_DLG_OK, "dialog");
   CHECK(vadhoc_transport_open() == VADHOC_OK, "open");
   vadhoc_net_term();
   CHECK(vadhoc_fake_balance(why, sizeof(why)) == 0,
         "did not terminate what it did not start: %s", why);
}

/* ================================================================= session == */

typedef struct app
{
   netdrv *nd;
   unsigned rx_ok, rx_bad;
   int started, stopped;
} app;

static size_t msg_build(uint8_t *b, uint32_t seq, uint8_t from)
{
   static const uint16_t L[] = { 16, 36, 104, 8, 240, ND_MAX_PAYLOAD, 61 };
   size_t n = L[seq % 7], i;
   if (n > ND_MAX_PAYLOAD)
      n = ND_MAX_PAYLOAD;
   b[0] = (uint8_t)seq; b[1] = (uint8_t)(seq >> 8); b[2] = from;
   for (i = 3; i < n; i++)
      b[i] = (uint8_t)(seq * 13u + from + i);
   return n;
}

static void app_deliver(void *u, const void *buf, size_t len, uint8_t src)
{
   app *a = (app *)u;
   uint8_t ref[ND_MAX_PAYLOAD];
   size_t n = msg_build(ref, a->rx_ok, src);
   if (len == n && !memcmp(ref, buf, n))
      a->rx_ok++;
   else
      a->rx_bad++;
}
static void app_started(void *u, uint8_t id) { (void)id; ((app *)u)->started = 1; }
static void app_stopped(void *u, int r) { (void)r; ((app *)u)->stopped = 1; }

typedef struct variant
{
   const char *name;
   int broadcast_works, bcast_mode, tx_thread, recv_returns_size;
   int lat, jit, loss;
} variant;

/* One console, start to finish.  0 = everything checked out. */
static int console_main(int is_host, const variant *v)
{
   enum { MSGS = 200, BULK = 60 };
   static const int host_peers[] = { 2 }, join_peers[] = { 1 };
   vadhoc_fake_opts o = { 0, 3, SCE_COMMON_DIALOG_RESULT_OK, 0, 0 };
   nd_transport tp;
   nd_callbacks cb;
   nd_config cfg;
   app a;
   vadhoc_stats st;
   nd_stats ns;
   uint8_t buf[VADHOC_BULK_MAX], peer_mac[6];
   unsigned sent = 0, bulk_sent = 0, bulk_got = 0, bulk_n = 0, bulk_bad = 0;
   uint64_t t_end, done_at = 0;
   char why[160] = "";
   int n;

   g_fail = 0;
   o.broadcast_works = v->broadcast_works;
   o.recv_returns_size = v->recv_returns_size;
   vadhoc_fake_setup(is_host ? 1 : 2, g_base, is_host ? host_peers : join_peers, 1);
   vadhoc_fake_options(&o);

   CHECK(vadhoc_net_init(NULL) == VADHOC_OK, "net_init");
   CHECK(run_dialog(is_host ? VADHOC_DIALOG_CREATE : VADHOC_DIALOG_JOIN,
                    30000000) == VADHOC_DLG_OK, "dialog");
   vadhoc_set_tx_thread(v->tx_thread);
   vadhoc_set_broadcast_mode(v->bcast_mode);
   vadhoc_set_fault(v->lat, v->jit, v->loss, is_host ? 0x51 : 0x52);
   CHECK(vadhoc_transport_open() == VADHOC_OK, "open (%s %08x)", vadhoc_stage(),
         vadhoc_last_sce_error());
   CHECK(vadhoc_bulk_open(64, "GBLK") == 0, "bulk open");

   vadhoc_transport_iface(&tp);
   memset(&a, 0, sizeof(a));
   memset(&cb, 0, sizeof(cb));
   cb.user = &a;
   cb.deliver = app_deliver;
   cb.session_started = app_started;
   cb.session_stopped = app_stopped;
   memset(&cfg, 0, sizeof(cfg));
   cfg.protocol = "GBAdhoc GB link v2";
   cfg.nick = is_host ? "host" : "join";
   cfg.join_nonce = is_host ? 0x1111 : 0x2222;
   a.nd = netdrv_create(&tp, &cb, &cfg);
   CHECK(a.nd != NULL, "netdrv");
   if (!a.nd)
      return 1;
   CHECK((is_host ? netdrv_host(a.nd) : netdrv_join(a.nd)) == 0, "host/join");

   t_end = nd_now_us() + 25000000ull;
   while (nd_now_us() < t_end)
   {
      uint64_t now = nd_now_us();
      /* the core's mid-frame poll: only pump when the gate says so */
      if (netdrv_poll_needed(a.nd) || (now / 1000u) % 4 == 0)
         netdrv_pump(a.nd, now);
      if (netdrv_active(a.nd) && netdrv_peer_count(a.nd) == 1)
      {
         uint16_t peer = is_host ? 1 : 0;
         if (sent < MSGS && netdrv_send_capacity(a.nd, peer) > 4)
         {
            size_t len = msg_build(buf, sent, (uint8_t)netdrv_local_id(a.nd));
            if (netdrv_send(a.nd, ND_RELIABLE | ND_FLUSH_HINT, buf, len, peer) == 0)
               sent++;
         }
         if (bulk_sent < BULK && netdrv_peer_mac(a.nd, peer, peer_mac) == 0)
         {
            memset(buf, 0x33, 1037);
            memcpy(buf, "GBLK", 4);
            buf[4] = (uint8_t)bulk_sent;
            buf[1036] = (uint8_t)(bulk_sent ^ 0x5A);
            if (vadhoc_bulk_send(peer_mac, buf, 1037) == 0)
               bulk_sent++;
         }
      }
      while ((n = vadhoc_bulk_recv(buf, sizeof(buf))) > 0)
      {
         /* the lane is unreliable: under injected loss some never arrive;
          * what does arrive must be intact and in order */
         if (n == 1037 && !memcmp(buf, "GBLK", 4) &&
             buf[4] >= (uint8_t)bulk_got && buf[1036] == (uint8_t)(buf[4] ^ 0x5A))
         {
            bulk_got = (unsigned)buf[4] + 1;
            bulk_n++;
         }
         else
            bulk_bad++;
      }
      if (!done_at && a.rx_ok == MSGS && sent == MSGS && bulk_sent == BULK &&
          netdrv_unacked(a.nd, is_host ? 1 : 0) == 0)
         done_at = now;
      /* stay up a while so the partner gets its last acknowledgements */
      if (done_at && now - done_at > 1500000u)
         break;
      if (a.stopped)
         break;                           /* the partner said BYE: it is done */
      nd_sleep_us(1000);
   }
   CHECK(a.started, "session started");
   CHECK(a.rx_ok == MSGS && !a.rx_bad, "got %u of %d reliable, %u bad", a.rx_ok,
         MSGS, a.rx_bad);
   CHECK(sent == MSGS, "sent %u", sent);
   CHECK(!bulk_bad, "bulk datagrams intact (%u bad)", bulk_bad);
   if (!v->loss)
      CHECK(bulk_n == BULK, "bulk lane: %u of %d", bulk_n, BULK);
   else
      CHECK(bulk_n > BULK / 2, "bulk lane under loss: %u of %d", bulk_n, BULK);

   netdrv_get_stats(a.nd, &ns);
   vadhoc_get_stats(&st);
   CHECK(ns.rx_drop_malformed == 0 && ns.rx_drop_crc == 0,
         "netdrv saw no bulk datagram (malformed %u)", ns.rx_drop_malformed);
   /* (frames the shim still holds back are in the ring, not yet netdrv's) */
   CHECK(st.rx_pkts == ns.rx_frames + vadhoc_rx_depth(),
         "ring = netdrv frames (%u == %u + %u)", st.rx_pkts, ns.rx_frames,
         vadhoc_rx_depth());
   CHECK(st.bulk_rx == bulk_n && st.bulk_tx == BULK, "bulk counters (%u/%u)",
         st.bulk_rx, bulk_n);
   CHECK(st.tx_fail == 0 && st.rx_err == 0, "no send/recv errors (%u/%u, %08x)",
         st.tx_fail, st.rx_err, vadhoc_last_sce_error());
   if (v->tx_thread)
      CHECK(st.tx_queued > 0 && st.tx_inline == 0,
            "every frame went through the TX thread (%u queued, %u inline)",
            st.tx_queued, st.tx_inline);
   else
      CHECK(st.tx_queued == 0, "inline sends");
   if (v->bcast_mode == 1)
      CHECK(st.bcast_unicast == 0, "broadcast only");
   else
      CHECK(st.bcast_unicast > 0, "per-peer copies were sent");
   if (v->lat | v->jit | v->loss)
      CHECK(st.fault_delayed + st.fault_dropped > 0, "the shim acted");
   printf("  %-4s %-22s rx=%u retx=%u srtt=%ums | pdp tx=%u (queued %u) "
          "bcast=%u+%u rx=%u bulk=%u/%u fault=%u/%u\n",
          is_host ? "host" : "join", v->name, ns.rx_frames, ns.retx,
          ns.srtt_us / 1000, st.tx_pkts, st.tx_queued, st.bcast_calls,
          st.bcast_unicast, st.rx_pkts, st.bulk_rx, st.bulk_tx,
          st.fault_delayed, st.fault_dropped);

   netdrv_leave(a.nd);
   netdrv_destroy(a.nd);
   vadhoc_transport_close();
   /* close drains the TX ring first: every queued frame (BYE included) was
    * handed to the stack */
   vadhoc_get_stats(&st);
   if (v->tx_thread)
      CHECK(st.tx_calls == st.tx_queued + st.tx_inline + BULK,
            "nothing was left in the TX ring (%u calls, %u queued)",
            st.tx_calls, st.tx_queued);
   vadhoc_net_term();
   CHECK(vadhoc_fake_balance(why, sizeof(why)) == 0, "teardown: %s", why);
   return g_fail ? 1 : 0;
}

static void test_pair(const variant *v)
{
   pid_t pid;
   int status = 0, rc;

   printf("[session: %s]\n", v->name);
   fflush(stdout);
   pid = fork();
   if (pid == 0)
   {
      nd_sleep_us(150000);                /* the joiner arrives second */
      _exit(console_main(0, v));
   }
   rc = console_main(1, v);
   waitpid(pid, &status, 0);
   g_fail = 0;
   CHECK(rc == 0, "host console");
   CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0, "join console (%d)",
         WIFEXITED(status) ? WEXITSTATUS(status) : -1);
}

int main(void)
{
   static const variant V[] = {
      /* name                      bc  mode tx size lat jit loss */
      { "plain",                   1,  0,   0, 0,   0,  0,  0 },
      { "broadcast dead, peers",   0,  0,   0, 0,   0,  0,  0 },
      { "broadcast only",          1,  1,   0, 0,   0,  0,  0 },
      { "per-peer only",           0,  2,   0, 0,   0,  0,  0 },
      { "tx thread",               1,  0,   1, 0,   0,  0,  0 },
      { "recv returns size",       1,  0,   0, 1,   0,  0,  0 },
      { "radio 20ms+10ms, 3% loss", 1, 0,   1, 0,  20, 10,  3 },
   };
   unsigned i;
   int total = 0;

   setvbuf(stdout, NULL, _IOLBF, 0);
   g_base = (uint16_t)(20000 + (getpid() % 400) * 16);
   test_ladder();
   total += g_fail;
   for (i = 0; i < sizeof(V) / sizeof(V[0]); i++)
   {
      test_pair(&V[i]);
      total += g_fail;
   }
   if (total)
   {
      printf("FAILED: %d check(s)\n", total);
      return 1;
   }
   printf("ALL VADHOC TESTS PASSED (against the fake stack, not a console)\n");
   return 0;
}
