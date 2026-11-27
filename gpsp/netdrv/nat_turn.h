/* SPDX-License-Identifier: MIT OR GPL-2.0-or-later
 * Written for VitAdhoc by its owner; offered under either licence so the separate players (snes.bin, md.bin) can link it. */
/* nat_turn.h -- the TURN (RFC 8656, UDP) client of the NAT layer, reduced to
 * ONE peer: allocate, permission, channel bind, refresh, release.  Pure: no
 * sockets, no threads, no globals, C99, no heap; a callback sends and the
 * caller passes the time.  Owner: package N.
 *
 * The values of nat_turn_state() / nat_turn_error() are LAN_RELAY_* /
 * LAN_RELAY_E_* of transport_lan.h (this file does not include it: the
 * NAT_RELAY_* names below are the same numbers, checked by a static assert
 * in transport_lan.c).
 *
 * ChannelData is not a STUN message and needs no state, so the wrap /
 * unwrap helpers at the end are plain functions the transport calls on its
 * data path.  The client talks to the single server it was started with;
 * the caller (the transport) only gives it datagrams from that address. */
#ifndef NAT_TURN_H
#define NAT_TURN_H

#include <stddef.h>
#include <stdint.h>

#include "nat_stun.h"            /* nat_send_fn */

#ifdef __cplusplus
extern "C" {
#endif

enum { NAT_RELAY_OFF = 0, NAT_RELAY_ALLOCATING, NAT_RELAY_ALLOCATED, NAT_RELAY_BINDING,
       NAT_RELAY_READY, NAT_RELAY_FAILED };
enum { NAT_RELAY_E_NONE = 0, NAT_RELAY_E_TIMEOUT, NAT_RELAY_E_AUTH, NAT_RELAY_E_QUOTA, NAT_RELAY_E_LOST };

#define NAT_TURN_CHANNEL 0x4000
#define NAT_TURN_TXNS    4
#define NAT_TURN_CRED_TTL_MS  3600000u  /* 1-hour TURN REST credential lifetime */
#define NAT_TURN_CRED_WARN_MS 3300000u  /* warn at 55 minutes (5 min before expiry) */

/* One request in flight (kept for its retransmissions). */
typedef struct nat_turn_txn
{
   int      active;
   int      kind;               /* allocate, refresh, permission, bind */
   int      sends, authed, auth_tries;
   uint32_t nonce_gen;          /* c->nonce_gen when this request was signed */
   uint8_t  txid[12];
   size_t   len;
   uint8_t  msg[640];
   uint64_t next_ms;
} nat_turn_txn;

/* Defined here so a caller can hold one without malloc.  N owns the fields
 * (and may grow the struct: callers only ever take its sizeof). */
typedef struct nat_turn
{
   int      state;              /* NAT_RELAY_* == LAN_RELAY_* */
   int      err;                /* NAT_RELAY_E_* == LAN_RELAY_E_* */
   nat_send_fn send;
   void    *ctx;
   uint32_t server_ip_be;
   uint16_t server_port;
   char     user[160], pass[160];
   uint32_t xs[4];              /* transaction-id generator (xorshift128), seeded by the caller */
   uint32_t relayed_ip_be;
   uint16_t relayed_port;
   uint64_t t0_ms;
   uint64_t clock_ms;           /* the last time start / bind / tick / release were given: answers are
                                   handled at that time (nat_turn_handle takes none) */
   uint64_t cred_mint_ms;       /* monotonic ms when credentials were minted / started */
   uint64_t cred_expires_ms;    /* monotonic ms when credentials expire (0 = untracked) */
   /* long-term credential */
   char     realm[128], nonce[192];
   int      have_nonce;
   uint32_t nonce_gen;
   uint8_t  key[16];
   /* allocation */
   uint32_t lifetime_s;
   uint64_t refresh_at, expires_at, alloc_retry_at;
   int      refresh_fails, realloc_tries;
   int      adopting;           /* an allocation made by an earlier program is being taken over (nat_turn_adopt) */
   /* the one peer */
   int      have_peer;
   uint32_t peer_ip_be;
   uint16_t peer_port;
   int      perm_ok, bind_ok, perm_busy, bind_busy;
   uint64_t perm_at, bind_at, retry_at;
   nat_turn_txn txn[NAT_TURN_TXNS];
} nat_turn;

/* Start allocating on server_ip:server_port (UDP) with the long-term
 * credential user / pass (copied).  seed: 16 bytes of real randomness for
 * the transaction ids. */
void nat_turn_start(nat_turn *c, nat_send_fn send, void *ctx, uint32_t server_ip_be, uint16_t server_port,
                    const char *user, const char *pass, const uint8_t seed[16], uint64_t now_ms);
/* Take over an allocation that already exists (2.0: the app made it, then snes.bin took the same local port;
 * the server keeps it, keyed by our 5-tuple).  NO Allocate is sent: a Refresh asks the server (401 first, then
 * authenticated) and, when it answers, the state is ALLOCATED with relayed_ip:relayed_port as given.  An answer
 * of 481 (no such allocation: the router changed the mapping, or it expired) or a refused credential is FAILED
 * (LAN_RELAY_E_LOST / _AUTH), a silent server TIMEOUT.  Then nat_turn_bind() as usual re-sends the permission and
 * ChannelBind (the same channel, NAT_TURN_CHANNEL), which refreshes the binding. */
void nat_turn_adopt(nat_turn *c, nat_send_fn send, void *ctx, uint32_t server_ip_be, uint16_t server_port,
                    const char *user, const char *pass, uint32_t relayed_ip_be, uint16_t relayed_port,
                    const uint8_t seed[16], uint64_t now_ms);
/* After ALLOCATED: permission for the peer's IP and ChannelBind 0x4000 to
 * peer_ip:peer_port (its RELAYED address); READY follows.  0 = started,
 * -1 = not allocated yet / another peer is already bound. */
int  nat_turn_bind(nat_turn *c, uint32_t peer_ip_be, uint16_t peer_port, uint64_t now_ms);
void nat_turn_tick(nat_turn *c, uint64_t now_ms);
/* A datagram from the server.  0 not ours; 1 consumed; 2 a Data indication
 * from the bound peer: *inner / *inner_len point into d. */
int  nat_turn_handle(nat_turn *c, const uint8_t *d, size_t n, const uint8_t **inner, size_t *inner_len);
/* Refresh with lifetime 0, once, best effort; the state goes to OFF. */
void nat_turn_release(nat_turn *c, uint64_t now_ms);
int  nat_turn_state(const nat_turn *c);                   /* LAN_RELAY_* */
int  nat_turn_error(const nat_turn *c);                   /* LAN_RELAY_E_* */
int  nat_turn_relayed(const nat_turn *c, uint32_t *ip_be, uint16_t *port);
/* Override the tracked credential TTL from the host mint time (default 1 hour from nat_turn_start). */
void nat_turn_set_cred_ttl(nat_turn *c, uint64_t ttl_ms, uint64_t now_ms);
/* Non-zero when credentials are within warn_before_ms (0 -> 5 min) of expiry; writes remaining ms to *rem_ms. */
int  nat_turn_cred_expiring(const nat_turn *c, uint64_t now_ms, uint64_t warn_before_ms, uint64_t *rem_ms);

/* ChannelData (RFC 8656 12.4), UDP form: 40 00 | length BE16 | payload, no
 * padding.  wrap returns the framed length (0 if cap is too small); unwrap
 * returns the channel number and points *payload into d (0 = not
 * ChannelData / truncated). */
size_t   nat_turn_channel_wrap(uint8_t *out, size_t cap, uint16_t channel, const void *payload, size_t len);
uint16_t nat_turn_channel_unwrap(const uint8_t *d, size_t n, const uint8_t **payload, size_t *len);

#ifdef __cplusplus
}
#endif

#endif /* NAT_TURN_H */
