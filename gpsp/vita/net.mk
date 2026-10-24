# net.mk -- the network layer of the Vita build, for gpsp/vita/Makefile to
# include:
#
#     include net.mk
#     OBJS    += $(NET_VITA_OBJS)
#     CFLAGS  += $(NET_VITA_CFLAGS)        # for EVERY file, see below
#     LIBS    += $(NET_VITA_LIBS)
#
# Paths are relative to gpsp/vita/.

NET_VITA_SRCS := ../netdrv/netdrv.c \
                 ../netdrv/transport_lan.c \
                 ../netdrv/transport_vadhoc.c \
                 ../netdrv/upnp_igd.c \
                 net_vita.c
NET_VITA_OBJS := $(NET_VITA_SRCS:.c=.o)

# netdrv's sizing is compile-time and lives in netdrv.h, which the frontend
# (fe_host.c, netpacket_host.c) includes too: these flags must reach every
# translation unit of the build, not only the files above, or two objects
# disagree on sizeof(netdrv) and on ND_MAX_FRAME without a link error.
#
#  ND_MAX_PAYLOAD   left at netdrv.h's 560: the Vita has the memory, a frame
#                   (576 B) is far inside any MTU, and the link session's
#                   ordered-channel chunks are 4x the PSP's.
#  ND_WINDOW 64     in-flight frames.  32 carries ~1000 payloads/s at 30 ms
#                   RTT but only ~100/s at 300 ms; the wireless-adapter mode
#                   needs ~120/s.  64 keeps 2x headroom on a slow internet
#                   path.  It sizes the reorder buffer, so both consoles must
#                   agree: lan_compat_hash() folds it into the room's hash.
#  ND_TXQ_CAP 256   backlog behind the window (>= 4x the window, as desktop).
#  ND_RTO_MAX_US    the RTO CEILING, for all three kinds of link.  It only
#                   bounds the estimator and the backoff from above, so one
#                   generous value (the PSP's 2.5 s) serves LAN, internet and
#                   ad hoc; the floor and the first-retransmission cap are
#                   chosen per session at run time: net_vita_arq_profile()
#                   -> fe_np_set_arq_timers().
#  ND_RTO_FIRST_MAX_US must stay UNDEFINED here: its default (= the ceiling)
#                   is what "0 = no cap" means for the ad hoc profile.
#
# One netdrv instance is then 909 KiB (netdrv_sizeof() of an ARMv7 build;
# 638 KiB with netdrv.h's defaults), allocated once per session.  netdrv's
# own unit suite and netdrv/tests/test_lan.c both pass at this sizing.
NET_VITA_CFLAGS := -I../netdrv \
                   -DND_WINDOW=64 -DND_TXQ_CAP=256 -DND_RTO_MAX_US=2500000

# Stub libraries, in an order that links (each after what uses it).
#  ScePspnetAdhoc   sceNetAdhoc*, sceNetAdhocctl*        (transport_vadhoc)
#  SceCommonDialog  sceNetCheckDialog*, sceCommonDialogUpdate
#  SceNetCtl        sceNetCtl*                            (all three)
#  SceNet           sceNetInit/Term (+ newlib's sockets)
#  SceSysmodule     sceSysmoduleLoadModule
#  SceLibKernel / SceKernelThreadMgr: threads, LwMutex, semaphores, the
#                   process clock -- vitasdk's default link already has them.
NET_VITA_LIBS := -lScePspnetAdhoc_stub -lSceCommonDialog_stub \
                 -lSceNetCtl_stub -lSceNet_stub -lSceSysmodule_stub
