# What snes.bin (and the Linux link twin) takes from gpsp/ to run a two-player link: the netdrv session driver, the
# UDP transport with its NAT layer, and the mirror session with the one-machine pair. Nothing from the Game Boy / GBA
# cores: netplay_stubs.c answers the two-Game-Boy calls inside fe_gblink.c with "no machine".
#
# LICENCE: these files live in gpsp/ (GPL-2.0-or-later, like the rest of the tree) and Snes9x is not GPL compatible
# (snes/README.md "Licence separation"). Linking them into snes.bin is the plan's design (ONLINE-2.0 section 4,
# package B3) and needs the owner's decision on the licence of exactly this list (see the B3 report). Keep the list
# short and in one place so that decision can be applied, or the files replaced, without touching anything else.
#
# GP is the gpsp directory relative to the including Makefile.
NETPLAY_GP_SRC := \
	$(GP)/frontend-common/fe_gblink.c $(GP)/frontend-common/fe_padpair.c $(GP)/frontend-common/fe_util.c \
	$(GP)/frontend-common/fe_evt.c \
	$(GP)/netdrv/netdrv.c $(GP)/netdrv/transport_lan.c \
	$(GP)/netdrv/nat_hash.c $(GP)/netdrv/nat_stun.c $(GP)/netdrv/nat_turn.c
NETPLAY_INC := -I$(GP)/frontend-common -I$(GP)/netdrv -I$(GP)/gbcore
# netdrv's sizing must be the app's (gpsp/vita/net.mk NET_VITA_CFLAGS): both consoles of a link run builds with
# the same window and backlog.
NETPLAY_ND_DEFS := -DND_WINDOW=64 -DND_TXQ_CAP=256 -DND_RTO_MAX_US=2500000
