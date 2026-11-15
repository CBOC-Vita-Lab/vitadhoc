/* nat_stun.h -- the STUN binding client of the NAT layer (RFC 5389, the one
 * request this project needs).  Pure: no sockets, no threads, no globals,
 * C99; a callback sends and the caller passes the time.  Owner: package N.
 *
 * The state values returned by nat_stun_state() are LAN_STUN_* of
 * transport_lan.h (this file does not include it: the NAT_STUN_* names below
 * are the same numbers, checked by a static assert in transport_lan.c).
 *
 * Retransmission: a request goes out at once and again 250, 500, 1000, 1500
 * and 2000 ms after the one before; the wait after the fifth is the last
 * (5.25 s in all) and then the server is given up on.  The state is OK as
 * soon as every server has answered or been given up on with at least one
 * answer, or 1 s after the FIRST answer (a second server that is slow or
 * filtered must not hold the first result back); the second server keeps
 * being asked on its own schedule and `sym` is filled in when it answers. */
#ifndef NAT_STUN_H
#define NAT_STUN_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum { NAT_STUN_IDLE = 0, NAT_STUN_RUNNING, NAT_STUN_OK, NAT_STUN_FAILED };

typedef int (*nat_send_fn)(void *ctx, uint32_t ip_be, uint16_t port, const void *buf, size_t len);

/* Defined here so a caller can hold one without malloc.  N owns the fields
 * (and may grow the struct: callers only ever take its sizeof). */
typedef struct nat_stun
{
   int      state;                  /* NAT_STUN_* == LAN_STUN_* */
   nat_send_fn send;
   void    *ctx;
   int      n;
   uint32_t ip_be[2];
   uint16_t port[2];
   uint8_t  txid[2][12];
   uint64_t next_ms[2];             /* when to send again / give up */
   int      sends[2];
   int      got[2], gave_up[2];
   uint32_t map_ip_be[2];
   uint16_t map_port[2];
   int      first;                  /* index of the first answer, -1 = none */
   uint64_t first_ms;
   uint64_t clock_ms;               /* the last time start / tick were given */
   int      sym;                    /* two answers that differ */
} nat_stun;

/* 20-byte Binding request into out; returns 20. */
size_t nat_stun_build_request(uint8_t out[20], const uint8_t txid[12]);
/* 1 = a success answer (txid, mapped address filled: XOR-MAPPED-ADDRESS, else
 * MAPPED-ADDRESS, IPv4 only; ip_be is the four bytes as they lie in memory),
 * 2 = an error answer (txid filled), 0 = not a STUN answer we can use. */
int    nat_stun_parse_response(const uint8_t *d, size_t n, uint8_t txid[12],
                               uint32_t *ip_be, uint16_t *port);
/* n = 1 or 2 servers; the transaction ids come from the caller (they must
 * be unpredictable: a guessable id lets anyone forge the answer). */
void   nat_stun_start(nat_stun *s, nat_send_fn send, void *ctx, const uint32_t *ip_be,
                      const uint16_t *port, int n, const uint8_t txids[2][12], uint64_t now_ms);
void   nat_stun_tick(nat_stun *s, uint64_t now_ms);
/* A datagram from from_ip:from_port.  1 = it answered one of our requests
 * (right transaction id from the right server): consumed. */
int    nat_stun_handle(nat_stun *s, uint32_t from_ip_be, uint16_t from_port,
                       const uint8_t *d, size_t n);
int    nat_stun_state(const nat_stun *s);                 /* LAN_STUN_* */
/* 1 once there is an answer: *ip_be / *port = the first one, *sym = two
 * answers differ.  0 = nothing yet. */
int    nat_stun_result(const nat_stun *s, uint32_t *ip_be, uint16_t *port, int *sym);

#ifdef __cplusplus
}
#endif

#endif /* NAT_STUN_H */
