/* nat_stun.h -- the STUN binding client of the NAT layer (RFC 5389, the one
 * request this project needs).  Pure: no sockets, no threads, no globals,
 * C99; a callback sends and the caller passes the time.  Owner: package N.
 * (W0 scaffold: the bodies are placeholders; the state values returned by
 * nat_stun_state() are LAN_STUN_* of transport_lan.h.) */
#ifndef NAT_STUN_H
#define NAT_STUN_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef int (*nat_send_fn)(void *ctx, uint32_t ip_be, uint16_t port, const void *buf, size_t len);

/* Defined here so a caller can hold one without malloc.  N owns the fields. */
typedef struct nat_stun
{
   int      state;                  /* LAN_STUN_* */
   nat_send_fn send;
   void    *ctx;
   uint32_t ip_be[2];
   uint16_t port[2];
   int      n;
   uint8_t  txid[2][12];
   uint64_t t0_ms, last_ms;
   int      tries;
   uint32_t map_ip_be[2];
   uint16_t map_port[2];
   int      got[2];
   int      sym;
} nat_stun;

size_t nat_stun_build_request(uint8_t out[20], const uint8_t txid[12]);
int    nat_stun_parse_response(const uint8_t *d, size_t n, uint8_t txid[12],
                               uint32_t *ip_be, uint16_t *port);  /* 1 ok, 2 error answer, 0 not STUN */
void   nat_stun_start(nat_stun *s, nat_send_fn send, void *ctx, const uint32_t *ip_be,
                      const uint16_t *port, int n, const uint8_t txids[2][12], uint64_t now_ms);
void   nat_stun_tick(nat_stun *s, uint64_t now_ms);
int    nat_stun_handle(nat_stun *s, uint32_t from_ip_be, uint16_t from_port,
                       const uint8_t *d, size_t n);       /* 1 = consumed */
int    nat_stun_state(const nat_stun *s);                 /* LAN_STUN_* */
int    nat_stun_result(const nat_stun *s, uint32_t *ip_be, uint16_t *port, int *sym);

#ifdef __cplusplus
}
#endif

#endif /* NAT_STUN_H */
