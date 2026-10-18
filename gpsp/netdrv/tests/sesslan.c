/* sesslan -- two consoles' REAL link sessions over transport_lan.
 *
 * tools/gblink/sesssim.c runs two complete fe_gblink stacks (the session
 * protocol over gbcore_dual over two TGB Dual instances each) in one process
 * over a simulated network that is reliable and ordered by construction.
 * This is the same two stacks -- sesssim.c is compiled into this file, its
 * main() and its queues unused -- over what a pair of Vitas would use:
 *
 *   fe_gblink  ->  netdrv (JOIN/WELCOME, ARQ, keepalive)  ->  transport_lan
 *   (real UDP sockets on the loopback network, each console's I/O thread,
 *   room discovery, the bulk lane, the fault shim for loss/latency/jitter)
 *
 * in real time: one display frame per `frame_us` microseconds.  The host
 * announces a room, the guest finds it with a scan and joins it by address.
 * Buttons are a fixed pseudo-random pattern per player.
 *
 *   sesslan rom_h=.. rom_g=.. [sav_h=..] [sav_g=..] [out_h=..] [out_g=..]
 *           [delay=N] [run=FRAMES] [frame_us=16667] [port=BASE]
 *           [loss=PCT] [lat=MS] [jitter=MS]        the fault shim, both ways
 *           [bulk=BYTES] [copies=N] [nack=1] [absorb=1] [batch=N]
 *           [rto_first=US] [rto_min=US] [rto_max=US]   nd_config timers
 *
 * Prints sesssim's EVT lines; exit 0 when both sessions end DONE.
 * Built and driven by netdrv/tests/run_sesslan.py.
 */
#define main sesssim_main_unused
#include "../../tools/gblink/sesssim.c"
#undef main

#include <arpa/inet.h>

#include "netdrv.h"
#include "nd_thread.h"
#include "transport_lan.h"

#define SL_PROTO "GBAdhoc GB link v2"       /* netpacket_host.c NP_GB_PROTOCOL */

static struct sl_con
{
   lan_transport *tp;
   netdrv *nd;
   int active, peers, peer_events, stop_reason;
   uint16_t peer_id;
   uint64_t tx_payloads, tx_bytes;
   unsigned bulk_rx_n;
} sl[2];

static void sl_deliver(void *u, const void *buf, size_t len, uint8_t src)
{
   int id = (struct sl_con *)u == &sl[1];
   (void)src;
   if (sess[id])
      STACK[id].receive(sess[id], buf, len);
}

static void sl_started(void *u, uint8_t id)
{
   (void)id;
   ((struct sl_con *)u)->active = 1;
}

static int sl_connected(void *u, uint8_t id)
{
   struct sl_con *c = (struct sl_con *)u;
   c->peers++;
   c->peer_id = id;
   c->peer_events++;
   return 0;
}

static void sl_disconnected(void *u, uint8_t id)
{
   struct sl_con *c = (struct sl_con *)u;
   int i = c == &sl[1];
   (void)id;
   c->peers--;
   if (sess[i])
      STACK[i].peer(sess[i], 0);
}

static void sl_stopped(void *u, int reason)
{
   struct sl_con *c = (struct sl_con *)u;
   int i = c == &sl[1];
   c->active = 0;
   c->stop_reason = reason;
   if (sess[i])
      STACK[i].peer(sess[i], 0);
}

/* What the platform's per-frame service does: netdrv first (its deliver
 * runs the session's receive), then the bulk lane. */
static void sl_pump(int id)
{
   uint8_t bk[LAN_BULK_MAX];
   int n, guard = 256;
   netdrv_pump(sl[id].nd, nd_now_us());
   while (guard-- && (n = lan_transport_bulk_recv(sl[id].tp, bk, sizeof(bk))) > 0)
   {
      sl[id].bulk_rx_n++;
      if (sess[id])
         STACK[id].receive_bulk(sess[id], bk, (size_t)n);
   }
}

static int sl_send(void *u, const void *b, size_t n)
{
   int id = ((console_t *)u)->id;
   sl[id].tx_payloads++;
   sl[id].tx_bytes += n;
   return netdrv_send(sl[id].nd, ND_RELIABLE | ND_FLUSH_HINT, b, n,
                      sl[id].peer_id);
}

static int sl_room(void *u)
{
   int id = ((console_t *)u)->id;
   return netdrv_send_capacity(sl[id].nd, sl[id].peer_id);
}

static int sl_bulk(void *u, const void *b, size_t n)
{
   int id = ((console_t *)u)->id;
   uint8_t mac[6];
   /* fe_host.c: the partner's address as netdrv knows it */
   if (netdrv_peer_mac(sl[id].nd, sl[id].peer_id, mac) != 0)
      return -1;
   return lan_transport_bulk_send(sl[id].tp, mac, b, n);
}

/* Stall absorption: deliver what the network has, as long as this display
 * frame still has time. */
static unsigned sl_absorb_budget_us = 6000;
static int sl_wait(void *u, uint32_t waited_us)
{
   int id = ((console_t *)u)->id;
   if (waited_us >= sl_absorb_budget_us)
      return 0;
   nd_sleep_us(500);
   sl_pump(id);
   return 1;
}

static uint64_t sl_now(void) { return nd_now_us(); }

static uint16_t sl_buttons(int id, uint64_t frame)
{
   /* changes every 6 frames, differently per player */
   uint32_t x = (uint32_t)(frame / 6) * 2654435761u + (uint32_t)id * 40503u;
   x ^= x >> 15;
   return gb_buttons(x & 0x1FF);
}

int main(int argc, char **argv)
{
   const char *rom[2] = { arg(argc, argv, "rom_h", NULL), arg(argc, argv, "rom_g", NULL) };
   const char *sav[2] = { arg(argc, argv, "sav_h", NULL), arg(argc, argv, "sav_g", NULL) };
   unsigned run_frames = (unsigned)atoi(arg(argc, argv, "run", "600"));
   unsigned frame_us = (unsigned)atoi(arg(argc, argv, "frame_us", "16667"));
   uint16_t base = (uint16_t)atoi(arg(argc, argv, "port", "0"));
   int loss = atoi(arg(argc, argv, "loss", "0"));
   int lat = atoi(arg(argc, argv, "lat", "0"));
   int jit = atoi(arg(argc, argv, "jitter", "0"));
   size_t bulk = (size_t)atoi(arg(argc, argv, "bulk", "0"));
   lan_config lc;
   lan_scan_config sc;
   lan_room_info info;
   lan_room rooms[4];
   nd_callbacks cb;
   nd_config nc;
   nd_transport tp;
   uint32_t compat = lan_compat_hash(SL_PROTO, "sesslan");
   uint64_t t0, next, running_since = 0, setup_t0, run_t0 = 0;
   uint64_t stall_ticks[2] = { 0, 0 };
   int i, n, rc = 0, end_asked = 0;
   uint64_t frame = 0;
   unsigned max_frames = (unsigned)atoi(arg(argc, argv, "max", "6000"));

   if (!rom[0] || !rom[1]) { fprintf(stderr, "usage: see sesslan.c\n"); return 2; }
   setvbuf(stdout, NULL, _IOLBF, 0);
   if (!base)
      base = (uint16_t)(30000 + (getpid() % 1500) * 16);
   con[0].id = 0; con[1].id = 1;
   con[0].out = arg(argc, argv, "out_h", NULL);
   con[1].out = arg(argc, argv, "out_g", NULL);
   sl_absorb_budget_us = frame_us > 4000 ? frame_us * 4 / 10 : 1500;

   /* ---- the room: host announces, guest scans and joins by address ---- */
   memset(&lc, 0, sizeof(lc));
   lc.mode = LAN_MODE_HOST;
   lc.disc_port = base;
   lc.flags = LAN_F_LOOPBACK;
   sl[0].tp = lan_transport_create(&lc);
   if (!sl[0].tp) { fprintf(stderr, "host transport: %s\n", lan_last_error()); return 2; }
   memset(&info, 0, sizeof(info));
   snprintf(info.name, sizeof(info.name), "GB07LINK");
   info.number = 7;
   info.kind = LAN_KIND_GB_CABLE;
   info.players = 1;
   info.max_players = 2;
   info.flags = LAN_ROOM_F_OPEN;
   snprintf(info.nick, sizeof(info.nick), "host");
   snprintf(info.title, sizeof(info.title), "LINK TEST");
   snprintf(info.code, sizeof(info.code), "LINK");
   info.compat = compat;
   lan_transport_announce(sl[0].tp, &info);

   memset(&sc, 0, sizeof(sc));
   sc.disc_port = base;
   sc.compat = compat;
   sc.flags = LAN_F_LOOPBACK;
   n = lan_discovery_scan(&sc, rooms, 4, 400);
   if (n != 1 || !rooms[0].compatible || rooms[0].info.kind != LAN_KIND_GB_CABLE)
   {
      sim("scan_failed rooms=%d", n);
      return 2;
   }
   sim("room_found name=%s room=%u addr=%s title=\"%s\" rtt_ms=%d",
       rooms[0].info.name, rooms[0].info.number, rooms[0].addr,
       rooms[0].info.title, rooms[0].rtt_ms);
   memset(&lc, 0, sizeof(lc));
   lc.mode = LAN_MODE_JOIN;
   lc.disc_port = base;
   lc.flags = LAN_F_LOOPBACK;
   lc.target_ip_be = rooms[0].ip_be;
   lc.target_port = rooms[0].port;
   sl[1].tp = lan_transport_create(&lc);
   if (!sl[1].tp) { fprintf(stderr, "join transport: %s\n", lan_last_error()); return 2; }
   for (i = 0; i < 2; i++)
      lan_transport_set_fault(sl[i].tp, loss, 0, lat, jit, 0x5E55 + (uint32_t)i);

   /* ---- netdrv, as fe_np_start_gb() sets it up ---- */
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
      nc.rto_first_max_us = (uint32_t)atoi(arg(argc, argv, "rto_first", "0"));
      nc.retx_us = (uint32_t)atoi(arg(argc, argv, "rto_min", "0"));
      nc.retx_max_us = (uint32_t)atoi(arg(argc, argv, "rto_max", "0"));
      sl[i].nd = netdrv_create(&tp, &cb, &nc);
      if (!sl[i].nd || (i ? netdrv_join(sl[i].nd) : netdrv_host(sl[i].nd)))
      { fprintf(stderr, "netdrv %d\n", i); return 2; }
   }
   t0 = nd_now_us();
   while (!(sl[0].peers == 1 && sl[1].peers == 1 && sl[1].active) &&
          nd_now_us() - t0 < 10000000ull)
   {
      sl_pump(0);
      sl_pump(1);
      nd_sleep_us(1000);
   }
   if (!(sl[0].peers == 1 && sl[1].peers == 1))
   {
      sim("join_failed host_peers=%d guest_peers=%d", sl[0].peers, sl[1].peers);
      return 2;
   }
   info.players = 2;
   info.flags = LAN_ROOM_F_BUSY;
   lan_transport_announce(sl[0].tp, &info);
   sim("net_up join_ms=%u", (unsigned)((nd_now_us() - t0) / 1000u));
   if (bulk)
      for (i = 0; i < 2; i++)
         if (lan_transport_bulk_open(sl[i].tp, 128, FE_GBLINK_BULK_MAGIC) != 0)
            return 2;

   /* ---- the sessions, as sesssim creates them ---- */
   for (i = 0; i < 2; i++)
   {
      fe_gblink_platform p;
      fe_gblink_local l;
      size_t sn = 0;
      uint8_t *sd = sav[i] ? slurp(sav[i], &sn) : NULL;
      memset(&p, 0, sizeof(p));
      memset(&l, 0, sizeof(l));
      p.user = &con[i];
      p.send = sl_send;
      p.max_payload = ND_MAX_PAYLOAD;        /* fe_host.c */
      p.send_room = sl_room;
      p.find_rom = p_find;
      p.commit_save = p_commit;
      p.bulk_max = bulk;
      p.bulk_send = bulk ? sl_bulk : NULL;
      p.bulk_rate = (unsigned)atoi(arg(argc, argv, "bulk_rate", "0"));
      p.input_copies = (unsigned)atoi(arg(argc, argv, "copies", "0"));
      p.wait = atoi(arg(argc, argv, "absorb", "0")) ? sl_wait : NULL;
      p.nack = atoi(arg(argc, argv, "nack", "0"));
      l.is_host = i == 0;
      l.rom_path = rom[i];
      l.console = console_of(rom[i]);
      l.save = sd;
      l.save_size = sn;
      l.wallclock_now = 1790500000;
      l.audio_rate = 32768;
      l.input_delay = (unsigned)atoi(arg(argc, argv, "delay", "4"));
      l.hash_interval = 60;
      l.batch_lines = (unsigned)atoi(arg(argc, argv, "batch", "0"));
      sess[i] = STACK[i].create(&p, &l);
      free(sd);
      if (!sess[i]) { fprintf(stderr, "create %d failed\n", i); return 2; }
      (i ? fe_gblink2_set_clock : fe_gblink_set_clock)(sess[i], sl_now);
   }
   STACK[0].peer(sess[0], 1);
   STACK[1].peer(sess[1], 1);
   sim("start delay=%s loss=%d lat=%d jitter=%d bulk=%u frame_us=%u payload=%u",
       arg(argc, argv, "delay", "4"), loss, lat, jit, (unsigned)bulk, frame_us,
       (unsigned)ND_MAX_PAYLOAD);

   setup_t0 = next = nd_now_us();
   for (frame = 0; frame < max_frames; frame++)
   {
      int st[2];
      for (i = 0; i < 2; i++)
      {
         int r;
         sl_pump(i);
         st[i] = STACK[i].state(sess[i]);
         if (st[i] == FE_GBLINK_RUNNING || st[i] == FE_GBLINK_ENDING)
         {
            fe_gblink_stats s;
            STACK[i].stats(sess[i], &s);
            r = STACK[i].step(sess[i], sl_buttons(i, s.frames[i]));
            if (r == FE_GBLINK_STALL)
               stall_ticks[i]++;
         }
         else
            STACK[i].step(sess[i], 0);
      }
      st[0] = STACK[0].state(sess[0]);
      st[1] = STACK[1].state(sess[1]);
      if (!running_since && st[0] == FE_GBLINK_RUNNING && st[1] == FE_GBLINK_RUNNING)
      {
         running_since = frame + 1;
         run_t0 = nd_now_us();
         sim("running setup_ms=%u", (unsigned)((run_t0 - setup_t0) / 1000u));
      }
      if (!end_asked && running_since && frame + 1 - running_since >= run_frames)
      {
         STACK[0].request_end(sess[0]);
         end_asked = 1;
      }
      if ((st[0] == FE_GBLINK_DONE || st[0] == FE_GBLINK_FAILED) &&
          (st[1] == FE_GBLINK_DONE || st[1] == FE_GBLINK_FAILED))
         break;
      /* the display's cadence */
      next += frame_us;
      while (nd_now_us() < next)
         nd_sleep_us(200);
   }
   /* let the last acknowledgements and BYE cross before the sockets close */
   for (i = 0; i < 100; i++)
   {
      sl_pump(0);
      sl_pump(1);
      nd_sleep_us(1000);
   }
   for (i = 0; i < 2; i++)
   {
      fe_gblink_stats s;
      nd_stats ns;
      lan_stats ls;
      STACK[i].stats(sess[i], &s);
      netdrv_get_stats(sl[i].nd, &ns);
      lan_transport_get_stats(sl[i].tp, &ls);
      sim("result console=%s state=%d error=%d committed=%d frames=%llu/%llu "
          "stall_ticks=%llu stalls=%u streak_max=%u episodes=%u hashes=%u/%u "
          "end=%u rom_tx=%u rom_rx=%u bulk_chunk=%u bulk_sent=%u resent=%u "
          "rx=%u dup=%u bad=%u inputs_fast=%u ordered=%u nacks=%u resends=%u",
          i ? "G" : "H", STACK[i].state(sess[i]), STACK[i].error(sess[i]),
          con[i].committed, (unsigned long long)s.frames[0],
          (unsigned long long)s.frames[1], (unsigned long long)stall_ticks[i],
          s.stalls, s.stall_streak_max, s.stall_episodes, s.hashes_matched,
          s.hashes_sent, s.end_frame, s.rom_tx_bytes, s.rom_rx_bytes,
          s.bulk_chunk, s.bulk_sent, s.bulk_resent, s.bulk_rx, s.bulk_dup,
          s.bulk_bad, s.inputs_fast, s.inputs_ordered, s.nacks, s.resends);
      sim("net console=%s nd_tx=%u nd_rx=%u retx=%u dup=%u acked=%u "
          "srtt_us=%u rto_us=%u malformed=%u crc=%u unknown=%u overflow=%u "
          "payloads=%llu payload_bytes=%llu lan_tx=%u lan_rx=%u ringdrop=%u "
          "foreign=%u bulk_tx=%u bulk_rx=%u bulk_drop=%u bulk_read=%u "
          "fault_drop=%u fault_delay=%u tx_us_avg=%u tx_us_max=%u stop=%d",
          i ? "G" : "H", ns.tx_frames, ns.rx_frames, ns.retx, ns.rx_dup,
          ns.acked, ns.srtt_us, ns.rto_us, ns.rx_drop_malformed, ns.rx_drop_crc,
          ns.rx_drop_unknown_peer, ns.tx_overflow,
          (unsigned long long)sl[i].tx_payloads,
          (unsigned long long)sl[i].tx_bytes, ls.tx_pkts, ls.rx_pkts,
          ls.rx_ring_drop, ls.rx_foreign, ls.bulk_tx, ls.bulk_rx, ls.bulk_drop,
          sl[i].bulk_rx_n, ls.fault_dropped, ls.fault_delayed,
          ls.tx_pkts ? ls.tx_us / ls.tx_pkts : 0, ls.tx_max_us,
          sl[i].stop_reason);
      if (STACK[i].state(sess[i]) != FE_GBLINK_DONE)
         rc = 1;
      /* the transport's invariants for a session */
      if (ns.rx_drop_malformed || ns.rx_drop_crc || ns.tx_overflow ||
          ls.rx_ring_drop || ls.rx_pkts != ns.rx_frames)
      {
         sim("transport_invariant_broken console=%s", i ? "G" : "H");
         rc = 1;
      }
   }
   sim("frames=%llu run_ms=%u", (unsigned long long)frame,
       run_t0 ? (unsigned)((nd_now_us() - run_t0) / 1000u) : 0);
   STACK[0].destroy(sess[0]);
   STACK[1].destroy(sess[1]);
   sess[0] = sess[1] = NULL;
   for (i = 1; i >= 0; i--)
   {
      netdrv_leave(sl[i].nd);
      netdrv_destroy(sl[i].nd);
      lan_transport_destroy(sl[i].tp);
   }
   return rc;
}
