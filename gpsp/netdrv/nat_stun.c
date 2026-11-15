/* nat_stun.c -- see nat_stun.h.  W0 scaffold: package N writes the client.
 * Until then nothing parses as STUN and a started exchange reports FAILED
 * (LAN_STUN_FAILED = 3 in transport_lan.h). */
#include <string.h>

#include "nat_stun.h"

size_t nat_stun_build_request(uint8_t out[20], const uint8_t txid[12])
{
   (void)txid;
   memset(out, 0, 20);
   return 0;
}

int nat_stun_parse_response(const uint8_t *d, size_t n, uint8_t txid[12],
                            uint32_t *ip_be, uint16_t *port)
{
   (void)d; (void)n; (void)txid; (void)ip_be; (void)port;
   return 0;
}

void nat_stun_start(nat_stun *s, nat_send_fn send, void *ctx, const uint32_t *ip_be,
                    const uint16_t *port, int n, const uint8_t txids[2][12], uint64_t now_ms)
{
   (void)send; (void)ctx; (void)ip_be; (void)port; (void)n; (void)txids; (void)now_ms;
   memset(s, 0, sizeof *s);
   s->state = 3;                      /* LAN_STUN_FAILED */
}

void nat_stun_tick(nat_stun *s, uint64_t now_ms)
{
   (void)s; (void)now_ms;
}

int nat_stun_handle(nat_stun *s, uint32_t from_ip_be, uint16_t from_port,
                    const uint8_t *d, size_t n)
{
   (void)s; (void)from_ip_be; (void)from_port; (void)d; (void)n;
   return 0;
}

int nat_stun_state(const nat_stun *s)
{
   return s->state;
}

int nat_stun_result(const nat_stun *s, uint32_t *ip_be, uint16_t *port, int *sym)
{
   (void)s; (void)ip_be; (void)port; (void)sym;
   return 0;
}
