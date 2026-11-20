# What snes.bin (and the Linux link twin) takes from gpsp/ to run a two-player link: ONLY the project's own UDP transport and
# its NAT layer (written for VitAdhoc by its owner; MIT OR GPL-2.0-or-later, SPDX headers in the files). The session itself is
# playerkit/src/pk_link.cpp and pk_link_net.cpp (MIT, written from scratch for the players).
#
# NOT linked, ever (the main app's Game Boy link, third-party GPL-2.0 from the GBAdhoc project): netdrv.c, fe_gblink.c,
# fe_evt.c, fe_util.c (and gb_link.c). The Makefile checks the finished program's object list and symbols for them
# (NETPLAY_FORBIDDEN below, `make check-licence`), and fails the build if one is there.
#
# transport_lan.c includes no netdrv header in a player build (PK_PLAYER): its small types come from playerkit/src/pk_lan_compat.h
# and the roster-frame rewrite, which a player never reaches, is compiled out.
#
# GP is the gpsp directory relative to the including Makefile.
NETPLAY_GP_SRC := \
	$(GP)/netdrv/transport_lan.c $(GP)/netdrv/nat_hash.c $(GP)/netdrv/nat_stun.c $(GP)/netdrv/nat_turn.c
# -DPK_PLAYER: transport_lan.[ch] take their small types from playerkit/src/pk_lan_compat.h (MIT, ours) instead of the app driver's
# headers; lazy (=) because PK is set after this file is included by some Makefiles
NETPLAY_INC = -I$(GP)/netdrv -I$(PK)/src -DPK_PLAYER
# the transport's sizing (it only sizes its netdrv-frame ring; the players use its datagram lane)
NETPLAY_ND_DEFS := -DND_WINDOW=64 -DND_TXQ_CAP=256 -DND_RTO_MAX_US=2500000
NETPLAY_FORBIDDEN := netdrv.o fe_gblink.o fe_evt.o fe_util.o gb_link.o
# and none of the app driver's headers may be read by any compile of a player (checked in the -MMD dependency files)
NETPLAY_FORBIDDEN_HDR := netdrv.h netdrv_wire.h nd_thread.h gb_link.h transport_adhoc.h transport_udp.h transport_vadhoc.h transport_inet.h
