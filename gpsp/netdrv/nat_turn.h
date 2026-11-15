/* nat_turn.h -- the TURN (RFC 5766, UDP) client of the NAT layer: allocate,
 * permission, channel bind, relay, release.  Pure: no sockets, no threads,
 * no globals, C99; a callback sends and the caller passes the time.
 * Owner: package N.  (W0 scaffold: the bodies are placeholders; the values
 * of nat_turn_state() / nat_turn_error() are LAN_RELAY_* / LAN_RELAY_E_* of
 * transport_lan.h.) */
#ifndef NAT_TURN_H
#define NAT_TURN_H

#include <stddef.h>
#include <stdint.h>

#include "nat_stun.h"            /* nat_send_fn */

#ifdef __cplusplus
extern "C" {
#endif

/* Defined here so a caller can hold one without malloc.  N owns the fields
 * (and may grow the struct: callers only ever take its sizeof). */
typedef struct nat_turn
{
   int      state;              /* LAN_RELAY_* */
   int      err;                /* LAN_RELAY_E_* */
   nat_send_fn send;
   void    *ctx;
   uint32_t server_ip_be;
   uint16_t server_port;
   char     user[160], pass[160];
   uint8_t  seed[16];
   uint32_t relayed_ip_be;
   uint16_t relayed_port;
   uint64_t t0_ms;
} nat_turn;

void nat_turn_start(nat_turn *c, nat_send_fn send, void *ctx, uint32_t server_ip_be, uint16_t server_port,
                    const char *user, const char *pass, const uint8_t seed[16], uint64_t now_ms);
int  nat_turn_bind(nat_turn *c, uint32_t peer_ip_be, uint16_t peer_port, uint64_t now_ms);
void nat_turn_tick(nat_turn *c, uint64_t now_ms);
/* 0 not ours; 1 consumed; 2 a Data indication: *inner / *inner_len point into d */
int  nat_turn_handle(nat_turn *c, const uint8_t *d, size_t n, const uint8_t **inner, size_t *inner_len);
void nat_turn_release(nat_turn *c, uint64_t now_ms);
int  nat_turn_state(const nat_turn *c);                   /* LAN_RELAY_* */
int  nat_turn_error(const nat_turn *c);                   /* LAN_RELAY_E_* */
int  nat_turn_relayed(const nat_turn *c, uint32_t *ip_be, uint16_t *port);
#define NAT_TURN_CHANNEL 0x4000

#ifdef __cplusplus
}
#endif

#endif /* NAT_TURN_H */
