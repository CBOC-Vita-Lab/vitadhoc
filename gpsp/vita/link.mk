# link.mk -- the link layer of the Vita app, for app.mk to include (both the
# VPK and the Linux twin).  Paths are relative to gpsp/vita/.
#
#   link_vita.c    THE NETWORK (app_vita.h part A, ui_host_hooks.h sections
#                  3-4): Wi-Fi rooms, a direct address, ad hoc; the session.
#                  Plain C against net_vita.h, the transports and fe_host:
#                  the same file on the console and on the twin.
#
# What it stands on:
#   the console    net_vita.c + the transports + UPnP: net.mk's list, already
#                  in the VPK (app.mk NET_SRCS).
#   the twin       hostcore/net_host.c    net_vita.h on Linux: the portable
#                                         half of net_vita.c + a stand-in
#                                         Wi-Fi the tests control
#                  hostcore/lan_host.c    transport_lan.c, its POSIX build
#                  hostcore/igd_host.c    upnp_igd.c, likewise
#                  hostcore/vadhoc_host.c transport_vadhoc.c over the fake
#                  hostcore/vadhoc_fake_host.c   ...the fake Sony ad hoc
#                                         stack (netdrv/tests), a radio cell
#                                         of loopback sockets
#                  (wrappers, because the twin's objects are built -DVITA
#                  and those files' -DVITA branches are Sony headers)
#
# The weak stubs these replace are in stubs_vita.c and ui_hooks_default.c;
# nothing is removed from any list.  Tests: hostcore/link_tests.py.
#
# local_vita.c (part B, two players on one console) is another workstream's:
# it appends itself here.

APP_SRCS      += link_vita.c share_vita.c mgift_vita.c
APP_SRCS_HOST += hostcore/net_host.c hostcore/lan_host.c hostcore/igd_host.c \
                 hostcore/vadhoc_host.c hostcore/vadhoc_fake_host.c
