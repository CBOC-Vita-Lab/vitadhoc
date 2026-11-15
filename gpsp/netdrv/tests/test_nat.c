/* test_nat.c -- the NAT layer's unit tests (package N's file).
 *
 * W0 scaffold: one test that checks the stubs say "not available", so
 * `make test-nat` builds and passes from the first commit.  N replaces this
 * file with the real tests (pure modules, then the transport against the
 * NAT simulator in tests/natsim/). */
#include <stdio.h>
#include <string.h>

#include "../transport_lan.h"
#include "../nat_hash.h"
#include "../nat_stun.h"
#include "../nat_turn.h"

static int fails;
#define CHECK(c, msg) do { if (!(c)) { printf("FAIL: %s\n", msg); fails++; } } while (0)

int main(void)
{
   lan_nat_status st;
   uint8_t key[8] = {0};
   memset(&st, 0xAA, sizeof st);
   lan_transport_nat_status(NULL, &st);
   CHECK(st.stun == LAN_STUN_IDLE && st.punch == LAN_PUNCH_OFF && st.relay == LAN_RELAY_OFF,
         "the stub status is all zero");
   CHECK(lan_transport_punch_arm(NULL, key) < 0, "the stubs say not available");
   CHECK(NAT_TURN_CHANNEL == 0x4000, "channel base");
   printf(fails ? "nat: %d FAILED\n" : "nat: scaffold ok\n", fails);
   return fails != 0;
}
