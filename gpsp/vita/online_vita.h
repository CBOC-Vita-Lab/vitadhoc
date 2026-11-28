/* online_vita.h -- one online connection (2.0): the state machine that takes
 * a room from "ask the service" through STUN, the punch or the relay, to a
 * transport that has its peer.  Owner: package M1 (online_vita.c).  Plan:
 * ONLINE-2.0-PLAN.md section 2.4, where online_step() is specified stage by
 * stage. */
#ifndef ONLINE_VITA_H
#define ONLINE_VITA_H

#include <stdint.h>

#include "transport_lan.h"
#include "online_http.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ONLINE_APP_VERSION "2.0.1"
#define ONLINE_URL_DEFAULT "https://vitadhoc.psvita.dev"
#define ONLINE_STUN_DEFAULT "stun.cloudflare.com:3478,stun.cloudflare.com:53"

enum { ON_IDLE = 0, ON_RESOLVE, ON_STUN, ON_CREATE, ON_WAITING,   /* host: the code is up        */
       ON_LOOKUP, ON_JOIN,                                        /* joiner                      */
       ON_PUNCH, ON_RELAY_CREDS, ON_RELAY_RESOLVE, ON_RELAY_ALLOC, ON_RELAY_POST, ON_RELAY_WAIT,
       ON_MEASURE, ON_CONNECTED, ON_FAILED };
enum { ONLINE_Q_NONE = 0, ONLINE_Q_GREAT, ONLINE_Q_GOOD, ONLINE_Q_SLOW };   /* < 80 ms, < 120 ms, else */
enum { ONLINE_FAIL_NONE = 0, ONLINE_FAIL_CODE, ONLINE_FAIL_UPDATE, ONLINE_FAIL_VERSION,
       ONLINE_FAIL_FULL, ONLINE_FAIL_UNREACHABLE, ONLINE_FAIL_BUSY, ONLINE_FAIL_SERVICE,
       ONLINE_FAIL_CLOSED, ONLINE_FAIL_EXPIRED, ONLINE_FAIL_CLOCK,
       ONLINE_FAIL_SLOW /* a 429 that clears within 2 minutes: "wait a minute and try again" */,
       ONLINE_FAIL_CONSOLE /* the service refuses this console's id (an older service) */ };
/* ONLINE_FAIL_VERSION: the view's host_ver says which side is older (1.6) */

typedef struct online_cfg
{
   const char *url, *stun;            /* NULL = the defaults; stun "off" = no STUN (pub = own address) */
   int  post_lan;                     /* 1 (default): the LAN address is offered for same-router pairs */
   int  force_relay, no_relay;        /* tests */
   unsigned punch_ms;                 /* 0 = the service's */
   uint64_t (*now_us)(void);
   void (*entropy)(uint8_t out[16]);
} online_cfg;

typedef struct online_view
{
   int  stage, is_host, relayed, quality, rtt_ms, fail;
   char code[6], peer_nick[16], host_ver[16];
   unsigned waited_s, ttl_s;          /* host: since the code went up / until it expires */
   uint32_t peer_ip_be; uint16_t peer_port;   /* ON_CONNECTED: the address netdrv and the bulk lane use */
} online_view;

void online_setup(const online_cfg *cfg);                   /* once, at boot (copied) */
int  online_host(lan_transport *t, const online_desc *room);   /* 0 = begun; pub/lan/sym are filled here */
int  online_join(lan_transport *t, const char *code, const online_desc *me);
int  online_step(void);                                     /* once per frame; returns the stage */
void online_get(online_view *out);
/* SNES (2.0): the program that plays the game is another one (snes.bin) and the sockets do not survive the change.  At
 * ON_CONNECTED, what the next program needs to make the same link again on the SAME local port: the punch key and the
 * peer's addresses (it punches again), or, for a relayed link, the allocation itself, which is let go WITHOUT being
 * released (lan_transport_relay_handoff) and adopted there.  Call it once, before online_linked() / online_stop().
 * 0 = filled; -1 = not connected, or a relayed link that is not READY. */
typedef struct online_handoff
{
   int      relayed;
   char     key[17];                      /* the punch key, 16 hex digits */
   uint32_t peer_ip_be;  uint16_t peer_port;   /* punched: where the peer answered; relayed: its relayed address */
   char     peer_lan[24];                 /* "ip:port" of the peer's own network address, or "" */
   uint16_t local_port;                   /* this transport's bound port: the next program binds it too */
   lan_relay_params relay;                /* relayed only */
   char     peer_nick[16];
} online_handoff;
int  online_handoff_get(online_handoff *out);
void online_linked(void);        /* the session formed: host closes the room ("linked"); relay stays up */
void online_stop(void);          /* cancel or teardown: close the room, stop punch and relay */
int  online_active(void);
/* The room card, for the UI's code pad; needs no transport. */
int  online_card_begin(const char *code);                   /* 0 = asked */
int  online_card_poll(online_reply *card);                  /* 0 not yet, 1 here (card->err says how) */
void online_card_cancel(void);

#ifdef __cplusplus
}
#endif

#endif /* ONLINE_VITA_H */
