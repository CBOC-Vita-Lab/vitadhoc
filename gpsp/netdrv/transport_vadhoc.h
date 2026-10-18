/* transport_vadhoc.h -- PS Vita ad hoc backend for netdrv: two consoles and
 * no router, over the Vita's PSP-compatible ad hoc stack (ScePspnetAdhoc:
 * PDP datagrams addressed by MAC), carrying the same netdrv frames and the
 * same bulk lane as transport_adhoc does on a PSP.
 *
 * It mirrors transport_adhoc.h (one static instance, an RX thread feeding a
 * ring, the bulk lane, stats, the debug fault shim) with ONE structural
 * difference.  A PSP program connects with sceNetAdhocctlConnect() and may
 * block while it does.  The Vita has no such call for applications: the
 * connection is made by the system's NetCheck dialog, and a system dialog
 * only advances while the application keeps rendering frames.  So the
 * blocking bring-up is cut in four, and the frontend's main loop runs
 * between the pieces:
 *
 *   (a) vadhoc_net_init()          modules + stack; returns at once
 *   (b) vadhoc_dialog_start()      open the "connecting..." dialog
 *       vadhoc_dialog_poll()       ONCE PER RENDERED FRAME until it stops
 *                                  saying RUNNING (the renderer must feed
 *                                  the dialog: see vadhoc_dialog_update)
 *       vadhoc_dialog_end()        close it (always, whatever the outcome)
 *   (c) vadhoc_transport_open()    after OK: MAC, PDP socket, RX thread
 *       vadhoc_transport_iface()   -> netdrv_create / fe_np_start*
 *   (d) vadhoc_transport_close()   RX thread, socket, leave the radio cell
 *       vadhoc_net_term()          the rest, in reverse
 *
 * ROOMS.  The dialog owns who is in the radio cell; there is no scan list.
 * Inside the cell a "room" is netdrv's own handshake: the host calls
 * netdrv_host(), the joiner's JOIN is a PDP broadcast, and the protocol
 * string decides who answers.  vadhoc_dialog_set_group() optionally names
 * the cell (the PSP build's group ids fit: "GC07AMKE"), which keeps two
 * pairs of consoles in one room apart IF the firmware honours it (HARDWARE).
 *
 * WHAT ONLY HARDWARE CAN CONFIRM is marked "HARDWARE:" in transport_vadhoc.c
 * and collected at its top.  Everything here was compiled and linked against
 * vitasdk and run on a PC against a fake of the SCE calls
 * (netdrv/tests/vadhoc_fake.c) -- never on a console.
 */
#ifndef TRANSPORT_VADHOC_H
#define TRANSPORT_VADHOC_H

#include <stddef.h>
#include <stdint.h>

#include "netdrv.h"

#ifdef __cplusplus
extern "C" {
#endif

/* PDP port: the PSP build's (plan 4.3; == 19019, the UDP default). */
#define VADHOC_PDP_PORT   0x4A4B
/* Ad hoc id: exactly 9 characters, no terminator.  Consoles only see each
 * other in the dialog if theirs are equal, so it is one constant for every
 * GBAdhoc build.  The two references that work on hardware (VitaShell,
 * vitaQuake) both pass their 9-character TITLE ID here and as the dialog's
 * NP communication id.  HARDWARE: whether the dialog insists on that. */
#define VADHOC_ADHOC_ID_DEFAULT "GBAD00001"
/* Largest bulk-lane datagram: the same 1400 as transport_lan, so a session
 * that picked a bulk size runs on any transport; a PDP datagram may be up
 * to 1444 bytes (SCE_NET_ADHOC_PDP_MFS). */
#define VADHOC_BULK_MAX   1400

enum
{
   VADHOC_OK                = 0,
   VADHOC_ERR_MODULES       = -1,   /* sceSysmoduleLoadModule failed */
   VADHOC_ERR_NET_INIT      = -2,   /* sceNetInit failed */
   VADHOC_ERR_NETCTL_INIT   = -3,   /* sceNetCtlInit failed */
   VADHOC_ERR_ADHOC_INIT    = -4,   /* sceNetAdhocInit failed */
   VADHOC_ERR_CTL_INIT      = -5,   /* sceNetAdhocctlInit failed */
   VADHOC_ERR_DIALOG        = -6,   /* sceNetCheckDialogInit failed */
   VADHOC_ERR_STATE         = -7,   /* called out of order */
   VADHOC_ERR_MAC           = -8,   /* sceNetAdhocctlGetEtherAddr failed */
   VADHOC_ERR_PDP           = -9,   /* sceNetAdhocPdpCreate failed */
   VADHOC_ERR_MEMORY        = -10,
   VADHOC_ERR_RX_THREAD     = -11,
   VADHOC_ERR_ALREADY       = -12   /* init/open while already up */
};

/* ---- (a) stack ------------------------------------------------------------
 * Loads SCE_SYSMODULE_NET and SCE_SYSMODULE_PSPNET_ADHOC, then sceNetInit
 * (with a pool of its own, unless the stack is already up: vita/net_vita.c
 * or newlib's socket() may have started it, and that is fine), sceNetCtlInit
 * (same tolerance), sceNetAdhocInit, sceNetAdhocctlInit.  adhoc_id NULL =
 * VADHOC_ADHOC_ID_DEFAULT.  term() undoes exactly what init() did itself. */
int  vadhoc_net_init(const char *adhoc_id);
void vadhoc_net_term(void);

/* ---- (b) the system dialog ------------------------------------------------ */

enum
{
   VADHOC_DIALOG_CONN   = 0,   /* connect: join the cell or start it
                                  (SCE_NETCHECK_DIALOG_MODE_PSP_ADHOC_CONN) */
   VADHOC_DIALOG_CREATE = 1,   /* start a cell: the host (..._CREATE) */
   VADHOC_DIALOG_JOIN   = 2    /* join one, never start one (..._JOIN) */
};

enum
{
   VADHOC_DLG_IDLE      = 0,   /* no dialog was started */
   VADHOC_DLG_RUNNING   = 1,
   VADHOC_DLG_OK        = 2,   /* connected: vadhoc_transport_open() may run */
   VADHOC_DLG_CANCELLED = 3,   /* the player backed out, or it was aborted */
   VADHOC_DLG_ERROR     = 4    /* failed/timed out: vadhoc_last_sce_error() */
};

/* The cell's name, at most 8 characters; NULL or "" = unnamed, which is what
 * both hardware references pass.  Call before vadhoc_dialog_start(). */
void vadhoc_dialog_set_group(const char *group);

/* Open the dialog.  timeout_us: how long a JOIN looks for a cell (0 = the
 * firmware's own default; VitaShell passes 60 s for JOIN and 0 for CONN).
 * Needs vadhoc_net_init() and, once at program start, the application's
 * sceCommonDialogSetConfigParam().  VADHOC_OK or VADHOC_ERR_*. */
int  vadhoc_dialog_start(int mode, uint32_t timeout_us);
/* Its state, VADHOC_DLG_*.  Call once per rendered frame while RUNNING; the
 * outcome is latched until vadhoc_dialog_end(). */
int  vadhoc_dialog_poll(void);
/* Ask a RUNNING dialog to close (the application is quitting).  Keep
 * polling: it ends CANCELLED a few frames later. */
void vadhoc_dialog_abort(void);
/* sceNetCheckDialogTerm().  Call exactly once after poll() left RUNNING. */
void vadhoc_dialog_end(void);

/* THE RENDERER'S PART.  A common dialog draws itself into the application's
 * frame, so while one is RUNNING the renderer must call sceCommonDialogUpdate
 * once per frame:
 *
 *   - AFTER the frame's own drawing is finished (after sceGxmEndScene: the
 *     call fails with SCE_COMMON_DIALOG_ERROR_WITHIN_SCENE inside a scene)
 *     and BEFORE that frame is queued for display (sceGxmDisplayQueueAddEntry
 *     / the buffer flip);
 *   - on the thread that renders;
 *   - describing the BACK buffer it has just drawn: its colour surface
 *     memory, the depth/stencil surface memory, the buffer's sync object,
 *     width 960, height 544, stride in pixels (960 for a display buffer
 *     allocated at exactly that width), SCE_GXM_COLOR_SURFACE_LINEAR and the
 *     display's colour format (SCE_GXM_COLOR_FORMAT_A8B8G8R8 in vita2d).
 *
 * This is vita2d's vita2d_common_dialog_update(), which VitaShell and
 * vitaQuake call between vita2d_end_drawing() and vita2d_swap_buffers().
 * The function below is that call for a renderer that is not vita2d; it
 * returns sceCommonDialogUpdate's result (0, or an SCE error).  It needs a
 * GXM renderer: with a bare sceDisplaySetFrameBuf loop the dialog cannot
 * draw (SCE_COMMON_DIALOG_ERROR_GXM_IS_UNINITIALIZED).  gxm_color_format is
 * a SceGxmColorFormat, display_sync a SceGxmSyncObject*. */
int  vadhoc_dialog_update(void *color_surface, void *depth_surface,
                          void *display_sync, unsigned width, unsigned height,
                          unsigned stride_px, unsigned gxm_color_format);

/* ---- (c) transport --------------------------------------------------------- */

/* Own MAC (sceNetAdhocctlGetEtherAddr), the PDP socket on VADHOC_PDP_PORT,
 * the rings, the RX thread.  Only after the dialog ended OK. */
int  vadhoc_transport_open(void);
/* RX/TX threads joined, socket deleted, sceNetCtlAdhocDisconnect().  The
 * stack stays initialised: another dialog can connect again. */
void vadhoc_transport_close(void);
/* Fill the vtable (zeroed first).  Valid only while open. */
void vadhoc_transport_iface(nd_transport *out);
/* 1 while the transport is open and the console is still in the cell
 * (sceNetCtlAdhocGetState).  A syscall: poll it about once a second, not
 * per frame. */
int  vadhoc_transport_connected(void);

typedef struct vadhoc_peer
{
   uint8_t mac[6];
   char    nick[32];             /* the other console's user name */
} vadhoc_peer;

/* Who else is in the cell (sceNetAdhocctlGetPeerList): for a "connected to
 * <name>" line.  Returns the count (<= max), or <0 on error. */
int  vadhoc_peers(vadhoc_peer *out, int max);
/* This console's MAC, valid while open. */
void vadhoc_local_mac(uint8_t mac[6]);

/* ---- bulk lane: transport_adhoc's, same semantics -------------------------- */
int  vadhoc_bulk_open(unsigned slots, const char magic[4]);
void vadhoc_bulk_close(void);
int  vadhoc_bulk_send(const uint8_t mac[6], const void *buf, size_t len);
int  vadhoc_bulk_recv(void *buf, size_t cap);
unsigned vadhoc_rx_depth(void);
unsigned vadhoc_bulk_depth(void);

/* ---- diagnostics ----------------------------------------------------------- */

typedef struct vadhoc_stats
{
   uint32_t tx_pkts;         /* PdpSend successes */
   uint32_t tx_would_block;  /* nonblocking send refused: dropped, ARQ's job */
   uint32_t tx_fail;         /* other PdpSend errors */
   uint32_t tx_calls;        /* PdpSend calls, wherever they ran */
   uint32_t tx_us, tx_max_us;       /* ... and what they cost */
   uint32_t tx_enq_us, tx_enq_max_us;   /* what the EMULATION thread paid */
   uint32_t tx_queued, tx_inline, tx_ring_full;
   uint32_t bcast_calls;     /* netdrv broadcast() calls */
   uint32_t bcast_unicast;   /* ... the per-peer copies sent beside FF:..:FF */
   uint32_t rx_pkts;         /* datagrams queued for the pump */
   uint32_t rx_ring_drop;    /* ring full (ARQ recovers) */
   uint32_t rx_oversize;     /* > ND_MAX_FRAME and not bulk */
   uint32_t rx_err;          /* unexpected PdpRecv errors */
   uint32_t bulk_tx, bulk_txfail, bulk_rx, bulk_drop;
   uint32_t fault_delayed, fault_dropped;
} vadhoc_stats;

void        vadhoc_get_stats(vadhoc_stats *out);
uint32_t    vadhoc_last_sce_error(void);   /* the SCE code of the last failure */
const char *vadhoc_stage(void);            /* where the bring-up is / failed */

/* ---- debug fault shim: adhoc_transport_set_fault()'s contract --------------
 * Applied on the RECEIVE side (PdpSend is fire-and-forget): each console
 * adds latency_ms + U[0,jitter_ms] to what it receives and drops loss_pct
 * percent; the ring stays FIFO (radio-like, no reorder).  All zero = off,
 * one integer test per datagram. */
void vadhoc_set_fault(int latency_ms, int jitter_ms, int loss_pct, uint32_t seed);

/* ---- how netdrv's broadcast() is sent ---------------------------------------
 * 0 = to FF:FF:FF:FF:FF:FF AND a unicast copy to every console in the peer
 *     list (DEFAULT: works whichever of the two the firmware honours; the
 *     duplicates are netdrv's idempotent JOIN and ROSTER, a few per second);
 * 1 = broadcast only (the PSP build's behaviour);
 * 2 = per-peer copies only.
 * HARDWARE: run a session in mode 1 and in mode 2 once; whichever fails to
 * connect tells which mechanism the firmware lacks. */
void vadhoc_set_broadcast_mode(int mode);

/* ---- TX offload (transport_adhoc's ADR-0021, as an option) -----------------
 * 0 = sceNetAdhocPdpSend runs inline on the emulation thread (DEFAULT);
 * 1 = a TX thread makes the call, the emulation thread pays a ring copy and
 * a semaphore signal.  HARDWARE: nobody knows yet what PdpSend costs on a
 * Vita; vadhoc_stats.tx_us / tx_max_us say, and this is the remedy if it is
 * milliseconds as on a PSP-1000.  Call before vadhoc_transport_open(). */
void vadhoc_set_tx_thread(int on);

#ifdef __cplusplus
}
#endif

#endif /* TRANSPORT_VADHOC_H */
