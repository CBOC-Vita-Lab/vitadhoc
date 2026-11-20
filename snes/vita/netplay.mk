# What snes.bin (and the Linux link twin) takes from gpsp/ to run a two-player link: ONLY the project's own UDP transport and
# its NAT layer (written for VitAdhoc by its owner; MIT OR GPL-2.0-or-later, SPDX headers in the files). The session itself is
# playerkit/src/pk_link.cpp and pk_link_net.cpp (MIT, written from scratch for the players).
#
# NOT linked, ever (the main app's Game Boy link, third-party GPL-2.0 from the GBAdhoc project): netdrv.c, fe_gblink.c,
# fe_evt.c, fe_util.c (and gb_link.c). The Makefile checks the finished program's object list and symbols for them
# (NETPLAY_FORBIDDEN below, `make check-licence`), and fails the build if one is there.
#
# transport_lan.c calls one function of the session driver, nd_crc16, on a path a player never reaches (a netdrv roster
# frame); playerkit/src/pk_link_net_shim.c provides that name with a plain CRC-16 of our own.
#
# GP is the gpsp directory relative to the including Makefile.
NETPLAY_GP_SRC := \
	$(GP)/netdrv/transport_lan.c $(GP)/netdrv/nat_hash.c $(GP)/netdrv/nat_stun.c $(GP)/netdrv/nat_turn.c
NETPLAY_INC := -I$(GP)/netdrv
# the transport's sizing (it only sizes its netdrv-frame ring; the players use its datagram lane)
NETPLAY_ND_DEFS := -DND_WINDOW=64 -DND_TXQ_CAP=256 -DND_RTO_MAX_US=2500000
NETPLAY_FORBIDDEN := netdrv.o fe_gblink.o fe_evt.o fe_util.o gb_link.o
