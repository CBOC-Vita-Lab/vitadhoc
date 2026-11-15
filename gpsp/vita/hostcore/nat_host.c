/* nat_host.c -- netdrv's NAT layer (nat_hash / nat_stun / nat_turn) for the
 * Linux twin.  The three files are pure C with no Sony calls; the twin's
 * objects are built -DVITA like the console's, so this wrapper (as lan_host.c
 * does for transport_lan.c) compiles them the same way the Vita build does. */
#include "../../netdrv/nat_hash.c"
#include "../../netdrv/nat_stun.c"
#include "../../netdrv/nat_turn.c"
