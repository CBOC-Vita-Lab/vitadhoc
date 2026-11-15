/* nat_turn.c -- see nat_turn.h.  W0 scaffold: package N writes the client.
 * Until then a started allocation is FAILED at once (LAN_RELAY_FAILED = 5). */
#include <string.h>

#include "nat_turn.h"

void nat_turn_start(nat_turn *c, nat_send_fn send, void *ctx, uint32_t server_ip_be, uint16_t server_port,
                    const char *user, const char *pass, const uint8_t seed[16], uint64_t now_ms)
{
   (void)send; (void)ctx; (void)server_ip_be; (void)server_port; (void)user; (void)pass;
   (void)seed; (void)now_ms;
   memset(c, 0, sizeof *c);
   c->state = 5;                      /* LAN_RELAY_FAILED */
}

int nat_turn_bind(nat_turn *c, uint32_t peer_ip_be, uint16_t peer_port, uint64_t now_ms)
{
   (void)c; (void)peer_ip_be; (void)peer_port; (void)now_ms;
   return -1;
}

void nat_turn_tick(nat_turn *c, uint64_t now_ms)
{
   (void)c; (void)now_ms;
}

int nat_turn_handle(nat_turn *c, const uint8_t *d, size_t n, const uint8_t **inner, size_t *inner_len)
{
   (void)c; (void)d; (void)n; (void)inner; (void)inner_len;
   return 0;
}

void nat_turn_release(nat_turn *c, uint64_t now_ms)
{
   (void)c; (void)now_ms;
}

int nat_turn_state(const nat_turn *c)
{
   return c->state;
}

int nat_turn_error(const nat_turn *c)
{
   return c->err;
}

int nat_turn_relayed(const nat_turn *c, uint32_t *ip_be, uint16_t *port)
{
   (void)c; (void)ip_be; (void)port;
   return 0;
}
