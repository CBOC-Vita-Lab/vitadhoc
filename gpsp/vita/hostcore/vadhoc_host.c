/* vadhoc_host.c -- netdrv/transport_vadhoc.c for the Linux twin, against the
 * fake of the Sony ad hoc calls (netdrv/tests/vadhoc_fake.c: a radio cell
 * made of UDP loopback sockets, station NN = 127.0.0.1:base+NN).
 *
 * Two twin processes given different station numbers and each other as
 * peers (net_host.c reads them from <root>/nethost.ini) reach each other,
 * so the whole ad hoc path of link_vita.c runs: the dialog's state machine,
 * the PDP transport, the broadcast JOIN, teardown.  What it says about a
 * console is what the fake assumes of one -- see transport_vadhoc.c's list
 * of HARDWARE assumptions. */
#undef VITA
#define VADHOC_HOST_FAKE
#include "../../netdrv/transport_vadhoc.c"
