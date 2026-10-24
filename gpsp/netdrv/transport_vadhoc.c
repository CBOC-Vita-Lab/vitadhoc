/* transport_vadhoc.c -- PS Vita ad hoc (ScePspnetAdhoc PDP) backend for
 * netdrv.  See transport_vadhoc.h for the contract and the call sequence.
 *
 * STATUS: compiled and linked against vitasdk; its logic (ladder, dialog
 * state, rings, threads, bulk lane, fault shim, broadcast fallback) runs on
 * a PC against netdrv/tests/vadhoc_fake.c.  It has NEVER run on a console.
 * Every SCE call below was checked against /opt/vitasdk's psp2 headers for
 * existence and signature; what the firmware does behind those signatures is
 * taken from three places, in this order of trust:
 *   - the PSP library this one is a port of, whose behaviour docs/
 *     ADHOC-NOTES.md pins down against PPSSPP and against two real PSPs
 *     (transport_adhoc.c);
 *   - VitaShell (init.c, netcheck_dialog.c, adhoc_dialog.c) and vitaQuake
 *     (menu.c, sys_psp2.c, net_adhoc_psp2.c), which ship and work;
 *   - the SDK header comments.
 *
 * ASSUMPTIONS ONLY HARDWARE CAN CONFIRM (each is marked HARDWARE: where it
 * is made, with what the code does if it turns out false):
 *
 *   H1  sceNetAdhocPdpRecv, blocking with a timeout in MICROSECONDS, returns
 *       0 and writes the datagram's size to *len (PSP semantics, the "big
 *       one" of ADHOC-NOTES 11.7).  vitaQuake reads the size from *len too,
 *       but only ever calls it non-blocking.
 *   H2  sceNetAdhocPdpSend to FF:FF:FF:FF:FF:FF reaches every console in the
 *       cell.  vitaQuake does exactly that for its server search -- but with
 *       a byte-swapped port, so its search may never have worked and it is
 *       not proof.  Therefore broadcast() ALSO unicasts to every MAC in
 *       sceNetAdhocctlGetPeerList (vadhoc_set_broadcast_mode).
 *   H3  PdpCreate's sport and PdpSend's dport are the same number in the
 *       same byte order.  We pass 0x4A4B to both, as the PSP build does.
 *   H4  The NetCheck dialog in the PSP_ADHOC modes leaves the console in a
 *       state where sceNetAdhocctlGetEtherAddr and sceNetAdhocPdpCreate
 *       work (VitaShell opens PTP sockets right after it, vitaQuake PDP).
 *   H5  Which dialog modes pair up.  VitaShell: sender CONN (timeout 0),
 *       receiver JOIN (60 s).  vitaQuake: CONN on both.  The PSP's lesson is
 *       host=CREATE, joiner=JOIN.  All three modes are exposed.
 *   H6  Whether the dialog honours groupName (rooms) and whether the ad hoc
 *       id must be the application's title id.
 *   H7  sceNetCtlAdhocGetState's values are SceNetCtlState's.
 *   H8  sceNetAdhocctlGetPeerList: *buflen in/out in bytes, entries packed
 *       as an array (the PSP shape); the list excludes this console.
 *   H9  What sceNetAdhocPdpSend costs on the emulation thread (stats
 *       tx_us/tx_max_us; vadhoc_set_tx_thread is the remedy).
 *   H10 RTT and loss of the Vita's ad hoc radio (netdrv's net_stats srtt_us):
 *       decides the ARQ profile.
 *   H11 Teardown: sceNetCtlAdhocDisconnect after PdpDelete, and that the
 *       console's Wi-Fi returns to its access point afterwards.
 */
#include "transport_vadhoc.h"
#include "nd_thread.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(VITA)
#include <psp2/common_dialog.h>
#include <psp2/net/net.h>
#include <psp2/net/netctl.h>
#include <psp2/netcheck_dialog.h>
#include <psp2/pspnet_adhoc.h>
#include <psp2/pspnet_adhocctl.h>
#include <psp2/sysmodule.h>
#elif defined(VADHOC_HOST_FAKE)
#include "tests/vadhoc_fake.h"         /* the same names, over UDP loopback */
#else
#error "transport_vadhoc.c is the PS Vita ad hoc transport: build with -DVITA"
#endif

/* sceNetCtlInit when it is already initialised (vitasdk's newlib defines
 * the same value the same way; the SDK headers have no name for it). */
#define VADHOC_NETCTL_NOT_TERMINATED 0x80412102u

/* sceNetInit pool when nobody initialised the stack before us (vitaQuake's
 * size; net_vita.c uses the same). */
#define VADHOC_NET_POOL     (1024 * 1024)
/* PDP socket buffer.  On a PSP the pending-RX queue is capped at this many
 * bytes (ADHOC-NOTES 11.10); 64 KiB holds ~45 bulk datagrams or >100 netdrv
 * frames while the RX thread is descheduled. */
#define VADHOC_PDP_BUFSIZE  0x10000
/* Blocking PdpRecv slice: bounds teardown (the thread re-checks its run flag
 * at least this often). */
#define VADHOC_RX_WAIT_US   250000
#define VADHOC_TX_WAIT_US   250000
/* One PDP datagram is at most SCE_NET_ADHOC_PDP_MFS = 1444 bytes.  Receiving
 * at that capacity (not ND_MAX_FRAME) means a foreign datagram is consumed
 * and dropped instead of wedging the socket: on a PSP a too-small buffer
 * returns NOT_ENOUGH_SPACE and LEAVES the datagram queued (11.7). */
#define VADHOC_RX_SCRATCH   1472
#define VADHOC_RX_SLOTS     256
#define VADHOC_TX_SLOTS     64
#define VADHOC_PEERS_MAX    8

enum
{
   ST_DOWN = 0,
   ST_MOD_NET,        /* SCE_SYSMODULE_NET loaded */
   ST_MOD_ADHOC,      /* SCE_SYSMODULE_PSPNET_ADHOC loaded */
   ST_NET,            /* sceNetInit done (by us or before us) */
   ST_NETCTL,         /* sceNetCtlInit done (ditto) */
   ST_ADHOC,          /* sceNetAdhocInit done */
   ST_CTL,            /* sceNetAdhocctlInit done: net_init complete */
   ST_OPEN            /* PDP socket + RX thread: transport open */
};

static const uint8_t BCAST_MAC[6] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };

static struct
{
   int      progress;
   int      mod_net_ours, mod_adhoc_ours, net_ours, netctl_ours;
   void    *net_pool;
   char     adhoc_id[SCE_NET_ADHOCCTL_ADHOCID_LEN];
   char     group[SCE_NET_ADHOCCTL_GROUPNAME_LEN];

   int      dlg_open;            /* Init succeeded, Term not called yet */
   int      dlg_state;           /* VADHOC_DLG_* (latched outcome) */

   int      pdp_id;
   uint8_t  mac[6];
   int      bcast_mode;          /* 0 both, 1 FF only, 2 peers only */

   nd_ring  rx, bulk, tx;
   nd_thread rx_th, tx_th;
   nd_sem   tx_sem;
   atomic_int rx_run, tx_run;
   int      tx_want, tx_on;
   atomic_int bulk_on;
   atomic_uint_fast32_t bulk_magic;

   /* debug shim: written by the emulation thread, read by the RX thread */
   atomic_int fault_lat_ms, fault_jit_ms, fault_loss_pct, fault_any;
   uint32_t fault_prng;          /* RX thread only */

   struct
   {
      nd_ctr tx_pkts, tx_would_block, tx_fail, tx_calls, tx_us, tx_max_us;
      nd_ctr tx_enq_us, tx_enq_max_us, tx_queued, tx_inline, tx_ring_full;
      nd_ctr bcast_calls, bcast_unicast;
      nd_ctr rx_pkts, rx_ring_drop, rx_oversize, rx_err;
      nd_ctr bulk_tx, bulk_txfail, bulk_rx, bulk_drop;
      nd_ctr fault_delayed, fault_dropped;
   } st;
   nd_ctr      last_sce;        /* either thread records a failure */
   const char *stage;
   uint8_t     scratch[VADHOC_RX_SCRATCH];     /* RX thread */
} V = { .stage = "down", .pdp_id = -1 };

/* The dialog keeps pointers into its parameter block and seals it with a
 * magic derived from the block's ADDRESS (_sceCommonDialogSetMagicNumber),
 * so the block is initialised in place, never copied, and lives here rather
 * than on a stack that is gone by the next frame. */
static SceNetCheckDialogParam  g_dlg_param;
static SceNetAdhocctlGroupName g_dlg_group;

/* ---- fault shim ------------------------------------------------------------ */

static uint32_t fault_rand(void)
{
   uint32_t x = V.fault_prng ? V.fault_prng : 0x9E3779B9u;
   x ^= x << 13; x ^= x >> 17; x ^= x << 5;
   V.fault_prng = x;
   return x;
}

void vadhoc_set_fault(int latency_ms, int jitter_ms, int loss_pct, uint32_t seed)
{
   int lat = latency_ms > 0 ? latency_ms : 0;
   int jit = jitter_ms  > 0 ? jitter_ms  : 0;
   int los = loss_pct   > 0 ? loss_pct   : 0;
   atomic_store_explicit(&V.fault_lat_ms, lat, memory_order_relaxed);
   atomic_store_explicit(&V.fault_jit_ms, jit, memory_order_relaxed);
   atomic_store_explicit(&V.fault_loss_pct, los, memory_order_relaxed);
   if (seed && V.progress != ST_OPEN)
      V.fault_prng = seed;       /* the RX thread owns it once it runs */
   atomic_store_explicit(&V.fault_any, lat | jit | los, memory_order_relaxed);
}

/* ---- RX thread ------------------------------------------------------------- */

static void rx_thread(void *arg)
{
   (void)arg;
   while (atomic_load_explicit(&V.rx_run, memory_order_acquire))
   {
      SceNetEtherAddr src;
      SceUShort16 sport = 0;
      int len = (int)sizeof(V.scratch);
      uint64_t t0 = nd_now_us();
      /* HARDWARE (H1): blocking (flag 0) with a microsecond timeout; success
       * is 0 with the size in len. */
      int rc = sceNetAdhocPdpRecv(V.pdp_id, &src, &sport, V.scratch, &len,
                                  VADHOC_RX_WAIT_US, 0);

      if (!atomic_load_explicit(&V.rx_run, memory_order_acquire))
         break;

      if (rc >= 0)
      {
         uint64_t due = 0;
         /* If H1 is wrong the other plausible shape is "returns the byte
          * count and leaves len alone": take the size from the return value
          * when len still reads as the capacity we passed in. */
         if (rc > 0 && rc < (int)sizeof(V.scratch) && len == (int)sizeof(V.scratch))
            len = rc;
         if (len <= 0 || len > (int)sizeof(V.scratch))
            continue;

         if (atomic_load_explicit(&V.fault_any, memory_order_relaxed))
         {
            int loss = atomic_load_explicit(&V.fault_loss_pct, memory_order_relaxed);
            int lat  = atomic_load_explicit(&V.fault_lat_ms, memory_order_relaxed);
            int jit  = atomic_load_explicit(&V.fault_jit_ms, memory_order_relaxed);
            if (loss && (int)(fault_rand() % 100) < loss)
            {
               nd_ctr_add(&V.st.fault_dropped, 1);
               continue;
            }
            if (lat | jit)
            {
               uint32_t d = (uint32_t)lat +
                  (jit ? fault_rand() % (uint32_t)(jit + 1) : 0);
               due = nd_now_us() + (uint64_t)d * 1000ull;
               nd_ctr_add(&V.st.fault_delayed, 1);
            }
         }

         if (len >= 4 && atomic_load_explicit(&V.bulk_on, memory_order_acquire))
         {
            uint32_t m;
            memcpy(&m, V.scratch, 4);
            if (m == (uint32_t)atomic_load_explicit(&V.bulk_magic,
                                                    memory_order_relaxed))
            {
               /* the bulk lane is not delayed (as on the PSP): its sender
                * paces itself and its datagrams carry no timing */
               if (len <= VADHOC_BULK_MAX &&
                   nd_ring_push(&V.bulk, src.data, V.scratch, (size_t)len))
                  nd_ctr_add(&V.st.bulk_rx, 1);
               else
                  nd_ctr_add(&V.st.bulk_drop, 1);
               continue;
            }
         }
         if (len > ND_MAX_FRAME)
         {
            nd_ctr_add(&V.st.rx_oversize, 1);   /* not ours: netdrv's wall
                                                   would drop it anyway */
            continue;
         }
         if (nd_ring_push_due(&V.rx, src.data, V.scratch, (size_t)len, due))
            nd_ctr_add(&V.st.rx_pkts, 1);
         else
            nd_ctr_add(&V.st.rx_ring_drop, 1);  /* full: ARQ retransmits */
      }
      else if ((unsigned)rc == (unsigned)SCE_ERROR_NET_ADHOC_TIMEOUT ||
               (unsigned)rc == (unsigned)SCE_ERROR_NET_ADHOC_WOULD_BLOCK)
      {
         /* An idle slice.  If it came back at once the call is not blocking
          * as assumed (H1): do not spin a core, poll at 1 ms instead. */
         if (nd_now_us() - t0 < 1000)
            nd_sleep_us(1000);
      }
      else
      {
         nd_ctr_add(&V.st.rx_err, 1);
         nd_ctr_set(&V.last_sce, (uint32_t)rc);
         nd_sleep_us(5000);                    /* no hard-error spin */
      }
   }
}

/* ---- send ------------------------------------------------------------------ */

static void stat_time(nd_ctr *sum, nd_ctr *max, uint64_t d)
{
   uint32_t v = d > 0xFFFFFFFFull ? 0xFFFFFFFFu : (uint32_t)d;
   nd_ctr_add(sum, v);
   nd_ctr_max(max, v);
}

/* The syscall itself, on whichever thread.  t0: the caller's clock read. */
static int pdp_send(const uint8_t mac[6], const void *buf, size_t len,
                    uint64_t t0, int is_inline)
{
   SceNetEtherAddr dst;
   uint64_t d;
   int rc;
   memcpy(dst.data, mac, 6);
   /* HARDWARE (H3): dport is the number PdpCreate was given.  Non-blocking:
    * a full socket buffer is a lost datagram, which the layers above repair. */
   rc = sceNetAdhocPdpSend(V.pdp_id, &dst, VADHOC_PDP_PORT, buf, (int)len, 0,
                           SCE_NET_ADHOC_F_NONBLOCK);
   d = nd_now_us() - t0;
   nd_ctr_add(&V.st.tx_calls, 1);
   stat_time(&V.st.tx_us, &V.st.tx_max_us, d);
   if (is_inline)
      stat_time(&V.st.tx_enq_us, &V.st.tx_enq_max_us, d);
   if (rc >= 0)                 /* 0 on a PSP; a byte count would be fine too */
   {
      nd_ctr_add(&V.st.tx_pkts, 1);
      return 0;
   }
   if ((unsigned)rc == (unsigned)SCE_ERROR_NET_ADHOC_WOULD_BLOCK)
   {
      nd_ctr_add(&V.st.tx_would_block, 1);
      return 0;
   }
   nd_ctr_add(&V.st.tx_fail, 1);
   nd_ctr_set(&V.last_sce, (uint32_t)rc);
   return -1;
}

static void tx_thread(void *arg)
{
   (void)arg;
   while (atomic_load_explicit(&V.tx_run, memory_order_acquire))
   {
      const nd_ring_slot *s;
      nd_sem_wait(&V.tx_sem, VADHOC_TX_WAIT_US);
      while ((s = nd_ring_peek(&V.tx)) != NULL)
      {
         pdp_send(s->mac, s->data, s->len, nd_now_us(), 0);
         nd_ring_drop(&V.tx);                  /* release after the send */
      }
   }
}

/* A netdrv frame out, from the emulation thread. */
static int send_frame(const uint8_t mac[6], const void *buf, size_t len)
{
   uint64_t t0 = nd_now_us();
   if (V.tx_on)
   {
      if (nd_ring_push(&V.tx, mac, buf, len))
      {
         nd_ctr_add(&V.st.tx_queued, 1);
         nd_sem_post(&V.tx_sem);
         stat_time(&V.st.tx_enq_us, &V.st.tx_enq_max_us, nd_now_us() - t0);
         return 0;
      }
      nd_ctr_add(&V.st.tx_ring_full, 1);       /* inline, never dropped */
   }
   nd_ctr_add(&V.st.tx_inline, 1);
   return pdp_send(mac, buf, len, t0, 1);
}

/* ---- nd_transport vtable --------------------------------------------------- */

static int vadhoc_send_to(void *ctx, const uint8_t mac[6],
                          const void *buf, size_t len)
{
   (void)ctx;
   if (V.progress != ST_OPEN)
      return -1;
   return send_frame(mac, buf, len);
}

/* Entries of the cell's peer list, MACs only.  Emulation thread. */
static int peer_macs(uint8_t out[][6], int max, char nick[][32])
{
   static SceNetAdhocctlPeerInfo list[VADHOC_PEERS_MAX];
   int bytes = (int)sizeof(list), n, i, rc;
   memset(list, 0, sizeof(list));
   /* HARDWARE (H8): *buflen is the capacity in and the bytes used out, and
    * the entries are an array (their `next` pointers are not followed). */
   rc = sceNetAdhocctlGetPeerList(&bytes, list);
   if (rc < 0)
   {
      nd_ctr_set(&V.last_sce, (uint32_t)rc);
      return -1;
   }
   n = bytes / (int)sizeof(list[0]);
   if (n > VADHOC_PEERS_MAX)
      n = VADHOC_PEERS_MAX;
   if (n > max)
      n = max;
   for (i = 0; i < n; i++)
   {
      memcpy(out[i], list[i].macAddr.data, 6);
      if (nick)
      {
         size_t k;
         memcpy(nick[i], list[i].nickname.data, 31);
         nick[i][31] = '\0';
         for (k = 0; nick[i][k]; k++)          /* it goes on a screen */
            if ((unsigned char)nick[i][k] < 0x20)
               nick[i][k] = '?';
      }
   }
   return n;
}

static int vadhoc_broadcast(void *ctx, const void *buf, size_t len)
{
   (void)ctx;
   if (V.progress != ST_OPEN)
      return -1;
   nd_ctr_add(&V.st.bcast_calls, 1);
   if (V.bcast_mode != 2)
      send_frame(BCAST_MAC, buf, len);
   if (V.bcast_mode != 1)
   {
      /* HARDWARE (H2): belt and braces.  netdrv broadcasts only its JOIN
       * (every 500 ms while joining) and the host's ROSTER (once a second),
       * and both are idempotent, so a console that hears the broadcast AND
       * the unicast copy just sees the same frame twice. */
      uint8_t macs[VADHOC_PEERS_MAX][6];
      int i, n = peer_macs(macs, VADHOC_PEERS_MAX, NULL);
      for (i = 0; i < n; i++)
      {
         if (!memcmp(macs[i], V.mac, 6) || !memcmp(macs[i], BCAST_MAC, 6))
            continue;
         send_frame(macs[i], buf, len);
         nd_ctr_add(&V.st.bcast_unicast, 1);
      }
   }
   return 0;
}

static int vadhoc_recv(void *ctx, uint8_t src_mac[6], void *buf, size_t cap)
{
   const nd_ring_slot *s;
   int len;
   (void)ctx;
   s = nd_ring_peek(&V.rx);
   if (!s)
      return 0;
   if (s->due_us && nd_now_us() < s->due_us)
      return 0;                                /* debug shim: not due yet */
   len = s->len;
   if ((size_t)len > cap)
      len = (int)cap;                          /* cap >= ND_MAX_FRAME */
   memcpy(src_mac, s->mac, 6);
   memcpy(buf, s->data, (size_t)len);
   nd_ring_drop(&V.rx);
   return len;
}

/* Two plain loads on the production path; the debug shim's due time is only
 * looked at (one clock read) when the harness switched it on. */
static int vadhoc_pending(void *ctx)
{
   const nd_ring_slot *s;
   (void)ctx;
   if (!nd_ring_nonempty(&V.rx))
      return 0;
   if (!atomic_load_explicit(&V.fault_any, memory_order_relaxed))
      return 1;
   s = nd_ring_peek(&V.rx);
   return s && (!s->due_us || nd_now_us() >= s->due_us);
}

static void vadhoc_local_addr(void *ctx, uint8_t mac[6])
{
   (void)ctx;
   memcpy(mac, V.mac, 6);
}

void vadhoc_transport_iface(nd_transport *out)
{
   memset(out, 0, sizeof(*out));               /* optional hooks read NULL */
   out->ctx        = NULL;                     /* singleton */
   out->send_to    = vadhoc_send_to;
   out->broadcast  = vadhoc_broadcast;
   out->recv       = vadhoc_recv;
   out->local_addr = vadhoc_local_addr;
   out->pending    = vadhoc_pending;
}

/* ---- bulk lane ------------------------------------------------------------- */

int vadhoc_bulk_open(unsigned slots, const char magic[4])
{
   uint32_t m;
   if (V.progress != ST_OPEN || !slots)
      return -1;
   if (!V.bulk.mem && nd_ring_init(&V.bulk, slots, VADHOC_BULK_MAX) != 0)
      return -1;
   memcpy(&m, magic, 4);
   atomic_store_explicit(&V.bulk_magic, m, memory_order_relaxed);
   nd_ring_flush(&V.bulk);
   atomic_store_explicit(&V.bulk_on, 1, memory_order_release);
   return 0;
}

void vadhoc_bulk_close(void)
{
   /* The ring stays (the RX thread may be mid-copy); close() frees it. */
   atomic_store_explicit(&V.bulk_on, 0, memory_order_release);
}

int vadhoc_bulk_send(const uint8_t mac[6], const void *buf, size_t len)
{
   if (V.progress != ST_OPEN || len > VADHOC_BULK_MAX)
      return -1;
   if (pdp_send(mac, buf, len, nd_now_us(), 1) != 0)
   {
      nd_ctr_add(&V.st.bulk_txfail, 1);
      return 0;                                /* lost: the sender resends */
   }
   nd_ctr_add(&V.st.bulk_tx, 1);
   return 0;
}

int vadhoc_bulk_recv(void *buf, size_t cap)
{
   const nd_ring_slot *s;
   int len;
   if (!V.bulk.mem || !(s = nd_ring_peek(&V.bulk)))
      return 0;
   len = s->len;
   if ((size_t)len > cap)
      len = (int)cap;
   memcpy(buf, s->data, (size_t)len);
   nd_ring_drop(&V.bulk);
   return len;
}

unsigned vadhoc_rx_depth(void)   { return V.rx.mem ? nd_ring_depth(&V.rx) : 0u; }
unsigned vadhoc_bulk_depth(void) { return V.bulk.mem ? nd_ring_depth(&V.bulk) : 0u; }

/* ---- (a) stack ------------------------------------------------------------- */

static int fail(int err, const char *stage, int sce)
{
   /* Unwind, but keep what failed readable for the caller's log. */
   vadhoc_net_term();
   V.stage = stage;
   nd_ctr_set(&V.last_sce, (uint32_t)sce);
   return err;
}

int vadhoc_net_init(const char *adhoc_id)
{
   SceNetAdhocctlAdhocId id;
   int rc;

   if (V.progress != ST_DOWN)
      return VADHOC_ERR_ALREADY;
   nd_ctr_set(&V.last_sce, 0);
   V.dlg_open = 0;
   V.dlg_state = VADHOC_DLG_IDLE;
   memset(&V.st, 0, sizeof(V.st));
   if (!adhoc_id || strlen(adhoc_id) != SCE_NET_ADHOCCTL_ADHOCID_LEN)
      adhoc_id = VADHOC_ADHOC_ID_DEFAULT;
   memcpy(V.adhoc_id, adhoc_id, SCE_NET_ADHOCCTL_ADHOCID_LEN);

   /* 1. modules.  Loading one that is loaded is an error on the Vita, and
    *    unloading one somebody else still uses pulls the stack from under
    *    them: only load what is not there, only unload what we loaded. */
   V.stage = "module_net";
   V.mod_net_ours = 0;
   if (sceSysmoduleIsLoaded(SCE_SYSMODULE_NET) != SCE_SYSMODULE_LOADED)
   {
      rc = sceSysmoduleLoadModule(SCE_SYSMODULE_NET);
      if (rc < 0)
         return fail(VADHOC_ERR_MODULES, "module_net", rc);
      V.mod_net_ours = 1;
   }
   V.progress = ST_MOD_NET;

   V.mod_adhoc_ours = 0;
   if (sceSysmoduleIsLoaded(SCE_SYSMODULE_PSPNET_ADHOC) != SCE_SYSMODULE_LOADED)
   {
      rc = sceSysmoduleLoadModule(SCE_SYSMODULE_PSPNET_ADHOC);
      if (rc < 0)
         return fail(VADHOC_ERR_MODULES, "module_adhoc", rc);
      V.mod_adhoc_ours = 1;
   }
   V.progress = ST_MOD_ADHOC;

   /* 2. the IP stack, unless it is up already (net_vita.c, or newlib's
    *    socket() which initialises it on first use): then sceNetInit says
    *    EBUSY and the pool we offered is not used. */
   V.net_ours = 0;
   V.net_pool = malloc(VADHOC_NET_POOL);
   if (!V.net_pool)
      return fail(VADHOC_ERR_MEMORY, "net_pool", 0);
   {
      SceNetInitParam p;
      p.memory = V.net_pool;
      p.size = VADHOC_NET_POOL;
      p.flags = 0;
      rc = sceNetInit(&p);
   }
   if (rc >= 0)
      V.net_ours = 1;
   else
   {
      free(V.net_pool);
      V.net_pool = NULL;
      if ((unsigned)rc != (unsigned)SCE_NET_ERROR_EBUSY)
         return fail(VADHOC_ERR_NET_INIT, "net_init", rc);
   }
   V.progress = ST_NET;

   V.netctl_ours = 0;
   rc = sceNetCtlInit();
   if (rc >= 0)
      V.netctl_ours = 1;
   else if ((unsigned)rc != VADHOC_NETCTL_NOT_TERMINATED)
      return fail(VADHOC_ERR_NETCTL_INIT, "netctl_init", rc);
   V.progress = ST_NETCTL;

   /* 3. the PSP-compatible ad hoc library, as VitaShell and vitaQuake
    *    initialise it: no arguments, then adhocctl with a RESERVED-type id
    *    of exactly nine characters. */
   rc = sceNetAdhocInit();
   if (rc < 0)
      return fail(VADHOC_ERR_ADHOC_INIT, "adhoc_init", rc);
   V.progress = ST_ADHOC;

   memset(&id, 0, sizeof(id));
   id.type = SCE_NET_ADHOCCTL_ADHOCTYPE_RESERVED;
   memcpy(id.data, V.adhoc_id, SCE_NET_ADHOCCTL_ADHOCID_LEN);
   rc = sceNetAdhocctlInit(&id);
   if (rc < 0)
      return fail(VADHOC_ERR_CTL_INIT, "ctl_init", rc);
   V.progress = ST_CTL;
   V.stage = "ready";
   return VADHOC_OK;
}

void vadhoc_net_term(void)
{
   if (V.progress == ST_DOWN)
      return;
   vadhoc_transport_close();
   if (V.dlg_open)
   {
      /* A dialog must not outlive the library it drives. */
      sceNetCheckDialogAbort();
      sceNetCheckDialogTerm();
      V.dlg_open = 0;
   }
   V.dlg_state = VADHOC_DLG_IDLE;
   if (V.progress >= ST_CTL)
      sceNetAdhocctlTerm();
   if (V.progress >= ST_ADHOC)
      sceNetAdhocTerm();
   if (V.progress >= ST_NETCTL && V.netctl_ours)
      sceNetCtlTerm();
   if (V.progress >= ST_NET && V.net_ours)
   {
      sceNetTerm();
      free(V.net_pool);
      V.net_pool = NULL;
   }
   if (V.progress >= ST_MOD_ADHOC && V.mod_adhoc_ours)
      sceSysmoduleUnloadModule(SCE_SYSMODULE_PSPNET_ADHOC);
   if (V.progress >= ST_MOD_NET && V.mod_net_ours)
      sceSysmoduleUnloadModule(SCE_SYSMODULE_NET);
   V.mod_net_ours = V.mod_adhoc_ours = V.net_ours = V.netctl_ours = 0;
   V.progress = ST_DOWN;
   V.stage = "down";
}

/* ---- (b) dialog ------------------------------------------------------------ */

void vadhoc_dialog_set_group(const char *group)
{
   memset(V.group, 0, sizeof(V.group));
   if (group)
      memcpy(V.group, group, strnlen(group, sizeof(V.group)));
}

int vadhoc_dialog_start(int mode, uint32_t timeout_us)
{
   int rc, sce_mode;

   if (V.progress < ST_CTL || V.progress == ST_OPEN)
      return VADHOC_ERR_STATE;
   if (V.dlg_open)
      return VADHOC_ERR_ALREADY;
   switch (mode)
   {
      case VADHOC_DIALOG_CREATE: sce_mode = SCE_NETCHECK_DIALOG_MODE_PSP_ADHOC_CREATE; break;
      case VADHOC_DIALOG_JOIN:   sce_mode = SCE_NETCHECK_DIALOG_MODE_PSP_ADHOC_JOIN;   break;
      case VADHOC_DIALOG_CONN:   sce_mode = SCE_NETCHECK_DIALOG_MODE_PSP_ADHOC_CONN;   break;
      default:                   return VADHOC_ERR_STATE;
   }
   sceNetCheckDialogParamInit(&g_dlg_param);   /* in place: see g_dlg_param */
   memcpy(g_dlg_group.data, V.group, SCE_NET_ADHOCCTL_GROUPNAME_LEN);
   g_dlg_param.groupName = &g_dlg_group;
   /* HARDWARE (H6): both references put their ad hoc id here as well. */
   memcpy(g_dlg_param.npCommunicationId.data, V.adhoc_id, 9);
   g_dlg_param.npCommunicationId.term = '\0';
   g_dlg_param.npCommunicationId.num = 0;
   g_dlg_param.mode = sce_mode;
   g_dlg_param.timeoutUs = timeout_us;

   V.stage = "dialog";
   rc = sceNetCheckDialogInit(&g_dlg_param);
   if (rc < 0)
   {
      nd_ctr_set(&V.last_sce, (uint32_t)rc);
      V.dlg_state = VADHOC_DLG_ERROR;
      return VADHOC_ERR_DIALOG;
   }
   V.dlg_open = 1;
   V.dlg_state = VADHOC_DLG_RUNNING;
   return VADHOC_OK;
}

int vadhoc_dialog_poll(void)
{
   SceNetCheckDialogResult res;
   int st, rc;

   if (!V.dlg_open || V.dlg_state != VADHOC_DLG_RUNNING)
      return V.dlg_state;                       /* idle, or already decided */
   st = (int)sceNetCheckDialogGetStatus();
   if (st == SCE_COMMON_DIALOG_STATUS_RUNNING)
      return VADHOC_DLG_RUNNING;
   if (st != SCE_COMMON_DIALOG_STATUS_FINISHED)
   {
      /* NONE while we believe it is open: it was torn down under us. */
      nd_ctr_set(&V.last_sce, (uint32_t)st);
      V.dlg_state = VADHOC_DLG_ERROR;
      return V.dlg_state;
   }
   memset(&res, 0, sizeof(res));
   rc = sceNetCheckDialogGetResult(&res);
   if (rc < 0)
   {
      nd_ctr_set(&V.last_sce, (uint32_t)rc);
      V.dlg_state = VADHOC_DLG_ERROR;
   }
   else if (res.result == SCE_COMMON_DIALOG_RESULT_OK)
   {
      V.dlg_state = VADHOC_DLG_OK;
      V.stage = "connected";
   }
   else if (res.result == SCE_COMMON_DIALOG_RESULT_USER_CANCELED ||
            res.result == SCE_COMMON_DIALOG_RESULT_ABORTED)
      V.dlg_state = VADHOC_DLG_CANCELLED;
   else
   {
      /* anything else is an SCE error code (no cell found, timed out, ...) */
      nd_ctr_set(&V.last_sce, (uint32_t)res.result);
      V.dlg_state = VADHOC_DLG_ERROR;
   }
   return V.dlg_state;
}

void vadhoc_dialog_abort(void)
{
   if (V.dlg_open && V.dlg_state == VADHOC_DLG_RUNNING)
      sceNetCheckDialogAbort();
}

void vadhoc_dialog_end(void)
{
   if (!V.dlg_open)
      return;
   sceNetCheckDialogTerm();
   V.dlg_open = 0;
   /* dlg_state stays: transport_open() checks that it ended OK */
}

int vadhoc_dialog_update(void *color_surface, void *depth_surface,
                         void *display_sync, unsigned width, unsigned height,
                         unsigned stride_px, unsigned gxm_color_format)
{
#ifdef VITA
   SceCommonDialogUpdateParam up;
   memset(&up, 0, sizeof(up));
   up.renderTarget.colorFormat      = (SceGxmColorFormat)gxm_color_format;
   up.renderTarget.surfaceType      = SCE_GXM_COLOR_SURFACE_LINEAR;
   up.renderTarget.width            = width;
   up.renderTarget.height           = height;
   up.renderTarget.strideInPixels   = stride_px;
   up.renderTarget.colorSurfaceData = color_surface;
   up.renderTarget.depthSurfaceData = depth_surface;
   up.displaySyncObject             = (SceGxmSyncObject *)display_sync;
   return sceCommonDialogUpdate(&up);
#else
   (void)color_surface; (void)depth_surface; (void)display_sync;
   (void)width; (void)height; (void)stride_px; (void)gxm_color_format;
   return 0;
#endif
}

/* ---- (c) transport --------------------------------------------------------- */

void vadhoc_set_tx_thread(int on)
{
   if (V.progress != ST_OPEN)
      V.tx_want = on ? 1 : 0;
}

void vadhoc_set_broadcast_mode(int mode)
{
   V.bcast_mode = (mode < 0 || mode > 2) ? 0 : mode;
}

int vadhoc_transport_open(void)
{
   SceNetEtherAddr me;
   int rc;

   if (V.progress == ST_OPEN)
      return VADHOC_ERR_ALREADY;
   if (V.progress != ST_CTL || V.dlg_open || V.dlg_state != VADHOC_DLG_OK)
      return VADHOC_ERR_STATE;                  /* the dialog must have ended OK */

   /* HARDWARE (H4): the MAC is valid once the dialog connected. */
   V.stage = "ether_addr";
   memset(&me, 0, sizeof(me));
   rc = sceNetAdhocctlGetEtherAddr(&me);
   if (rc < 0)
   {
      nd_ctr_set(&V.last_sce, (uint32_t)rc);
      return VADHOC_ERR_MAC;
   }
   memcpy(V.mac, me.data, 6);

   V.stage = "rings";
   if (nd_ring_init(&V.rx, VADHOC_RX_SLOTS, ND_MAX_FRAME) != 0)
      return VADHOC_ERR_MEMORY;
   atomic_store_explicit(&V.bulk_on, 0, memory_order_relaxed);

   V.stage = "pdp_create";
   rc = sceNetAdhocPdpCreate(&me, VADHOC_PDP_PORT, VADHOC_PDP_BUFSIZE, 0);
   if (rc < 0)
   {
      nd_ctr_set(&V.last_sce, (uint32_t)rc);
      nd_ring_free(&V.rx);
      return VADHOC_ERR_PDP;
   }
   V.pdp_id = rc;

   V.stage = "rx_thread";
   atomic_store_explicit(&V.rx_run, 1, memory_order_release);
   if (nd_thread_start(&V.rx_th, "gbadhoc_vadhoc_rx", rx_thread, NULL) != 0)
   {
      atomic_store_explicit(&V.rx_run, 0, memory_order_release);
      sceNetAdhocPdpDelete(V.pdp_id, 0);
      V.pdp_id = -1;
      nd_ring_free(&V.rx);
      return VADHOC_ERR_RX_THREAD;
   }

   /* The TX thread is an optimisation: if any part of it cannot be made the
    * transport sends inline, exactly as with the option off. */
   V.tx_on = 0;
   if (V.tx_want && nd_ring_init(&V.tx, VADHOC_TX_SLOTS, ND_MAX_FRAME) == 0)
   {
      if (nd_sem_init(&V.tx_sem, VADHOC_TX_SLOTS) == 0)
      {
         atomic_store_explicit(&V.tx_run, 1, memory_order_release);
         if (nd_thread_start(&V.tx_th, "gbadhoc_vadhoc_tx", tx_thread, NULL) == 0)
            V.tx_on = 1;
         else
         {
            atomic_store_explicit(&V.tx_run, 0, memory_order_release);
            nd_sem_destroy(&V.tx_sem);
         }
      }
      if (!V.tx_on)
         nd_ring_free(&V.tx);
   }

   V.progress = ST_OPEN;
   V.stage = "open";
   return VADHOC_OK;
}

void vadhoc_transport_close(void)
{
   if (V.progress != ST_OPEN)
      return;
   /* Stop accepting sends first: the vtable refuses once progress drops. */
   V.progress = ST_CTL;

   if (V.tx_on)
   {
      /* Let what netdrv queued last leave: its final frames are BYE, and a
       * peer that never hears one waits out an 8 s death timer. */
      int spin;
      for (spin = 0; spin < 100 && nd_ring_depth(&V.tx); spin++)
         nd_sleep_us(2000);
      atomic_store_explicit(&V.tx_run, 0, memory_order_release);
      nd_sem_post(&V.tx_sem);
      nd_thread_join(&V.tx_th);
      nd_sem_destroy(&V.tx_sem);
      nd_ring_free(&V.tx);
      V.tx_on = 0;
   }

   atomic_store_explicit(&V.rx_run, 0, memory_order_release);
   nd_thread_join(&V.rx_th);                    /* <= one recv slice */

   sceNetAdhocPdpDelete(V.pdp_id, 0);
   V.pdp_id = -1;
   /* HARDWARE (H11): leave the cell the dialog put us in.  The PSP order is
    * socket first, then disconnect. */
   sceNetCtlAdhocDisconnect();

   atomic_store_explicit(&V.bulk_on, 0, memory_order_relaxed);
   nd_ring_free(&V.rx);
   nd_ring_free(&V.bulk);
   V.dlg_state = VADHOC_DLG_IDLE;
   V.stage = "ready";
}

int vadhoc_transport_connected(void)
{
   int st = 0, rc;
   if (V.progress != ST_OPEN)
      return 0;
   /* HARDWARE (H7).  Only a definite "disconnected" counts: if the call
    * fails or speaks another dialect the session is left alone, and netdrv's
    * own 8 s keepalive is what notices a lost partner. */
   rc = sceNetCtlAdhocGetState(&st);
   return !(rc >= 0 && st == SCE_NETCTL_STATE_DISCONNECTED);
}

int vadhoc_peers(vadhoc_peer *out, int max)
{
   uint8_t macs[VADHOC_PEERS_MAX][6];
   char nicks[VADHOC_PEERS_MAX][32];
   int i, n;
   if (V.progress < ST_CTL)
      return -1;
   n = peer_macs(macs, max < VADHOC_PEERS_MAX ? max : VADHOC_PEERS_MAX, nicks);
   for (i = 0; i < n; i++)
   {
      memcpy(out[i].mac, macs[i], 6);
      memcpy(out[i].nick, nicks[i], sizeof(out[i].nick));
   }
   return n;
}

void vadhoc_local_mac(uint8_t mac[6])
{
   memcpy(mac, V.mac, 6);
}

/* ---- diagnostics ----------------------------------------------------------- */

void vadhoc_get_stats(vadhoc_stats *out)
{
   memset(out, 0, sizeof(*out));
#define G(f) out->f = nd_ctr_get(&V.st.f)
   G(tx_pkts); G(tx_would_block); G(tx_fail); G(tx_calls); G(tx_us);
   G(tx_max_us); G(tx_enq_us); G(tx_enq_max_us); G(tx_queued); G(tx_inline);
   G(tx_ring_full); G(bcast_calls); G(bcast_unicast); G(rx_pkts);
   G(rx_ring_drop); G(rx_oversize); G(rx_err); G(bulk_tx); G(bulk_txfail);
   G(bulk_rx); G(bulk_drop); G(fault_delayed); G(fault_dropped);
#undef G
}

uint32_t    vadhoc_last_sce_error(void) { return nd_ctr_get(&V.last_sce); }
const char *vadhoc_stage(void)          { return V.stage; }
