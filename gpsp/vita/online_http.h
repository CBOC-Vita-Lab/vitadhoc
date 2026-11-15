/* online_http.h -- the room client of online play (2.0): HTTPS calls to the
 * room service, one function per route, answers decoded into online_reply.
 * Owner: package H (online_http.cpp).  It stands on the project's own TLS
 * client in nethttps/ and never blocks the caller.  Plan: ONLINE-2.0-PLAN.md
 * section 2.3. */
#ifndef ONLINE_HTTP_H
#define ONLINE_HTTP_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum { ONLINE_E_NONE = 0, ONLINE_E_NET, ONLINE_E_NOT_FOUND, ONLINE_E_FULL, ONLINE_E_VERSION,
       ONLINE_E_UPDATE, ONLINE_E_BUSY, ONLINE_E_RATE, ONLINE_E_TOKEN, ONLINE_E_RELAY, ONLINE_E_BAD,
       ONLINE_E_SERVER, ONLINE_E_CLOCK, ONLINE_E_CERT };
/* NP_HTTPS_E_CLOCK -> CLOCK; NP_HTTPS_E_CERT -> CERT; any other transport failure -> NET.
 * status/code -> ONLINE_E_*: 404 NOT_FOUND; 409 room_full FULL; 409 version_mismatch VERSION;
 * 426 UPDATE; 503 over_budget/rooms_full/switched_off and 429 daily_cap BUSY; other 429 RATE; 401 TOKEN;
 * 403, 502, 503 relay_* RELAY; 400/413 BAD; other 5xx SERVER. */
enum { ONLINE_OP_NONE = 0, ONLINE_OP_CREATE, ONLINE_OP_LOOKUP, ONLINE_OP_JOIN, ONLINE_OP_POLL,
       ONLINE_OP_RELAY, ONLINE_OP_ENDPOINT, ONLINE_OP_CLOSE, ONLINE_OP_RESOLVE, ONLINE_OP_INFO };
enum { ONLINE_ROOM_OPEN = 0, ONLINE_ROOM_FULL, ONLINE_ROOM_JOINED, ONLINE_ROOM_RELAY, ONLINE_ROOM_CLOSED };
enum { ONLINE_MODE_PUNCH = 0, ONLINE_MODE_RELAY };

typedef struct online_desc
{
   int  kind;                 /* LAN_KIND_* */
   char console[5], game[9], rom[9], title[24], nick[16], ver[16];
   uint32_t compat;
   char pub[24], lan[24];     /* "ip:port" or "" */
   int  sym, relay_only;
} online_desc;

typedef struct online_reply
{
   int  op, http, err, retry_after;        /* err = ONLINE_E_* */
   char code[6], token[24], punch_key[17];
   int  state, mode, poll_ms, punch_ms, ttl_sec;
   int  have_peer;  char peer_nick[16], peer_pub[24], peer_lan[24];  int peer_sym;
   char relay_peer[24], reason[12];
   online_desc card;                       /* LOOKUP: kind, console, game, rom, title, nick, ver, compat */
   char host_ver[16];                      /* 409 version_mismatch */
   char turn_host[64];  uint16_t turn_port[3];  int turn_ports;   /* ordered 3478, 443, others */
   char turn_user[160], turn_cred[160];
   char resolved[16];                      /* RESOLVE: "a.b.c.d" */
   char min_ver[16], latest_ver[16];       /* UPDATE */
} online_reply;

void online_http_config(const char *base_url, const char *app_version);   /* copied; before any call */
/* Each returns 0 = sent, -1 = no free slot.  Up to THREE may be in flight (a poll, the relay credentials
 * and a resolve run beside each other during the punch); answers come back one per take, each with its op. */
/* rid: the request id, 16 lower-case hex digits chosen once per attempt and passed again on every retry of
 * it (NULL or "" = none).  The service answers a repeated create / join with the ORIGINAL answer. */
int  online_http_create(const online_desc *room, const char *rid);
int  online_http_lookup(const char *code);
int  online_http_join(const char *code, const online_desc *me, const char *rid);
int  online_http_poll(const char *code, const char *token, const char *new_pub /* NULL = unchanged */);
int  online_http_relay(const char *code, const char *token);
int  online_http_endpoint(const char *code, const char *token, const char *relay_addr);
int  online_http_close(const char *code, const char *token, const char *reason);
int  online_http_resolve(const char *host);
int  online_http_busy(int op);             /* 1 = a request of that op is in flight (ONLINE_OP_NONE: any) */
int  online_http_take(online_reply *out);  /* 1 = an answer (zeroed, then filled) */
void online_http_drop(void);               /* forget everything in flight: the answers are discarded */
void online_http_shutdown(void);           /* at exit: np_https_shutdown(2000) */
/* pure, for tests */
int  online_json_get(const char *body, const char *path, char *out, size_t cap);   /* "peer.pub"; 1 found */
int  online_json_int(const char *body, const char *path, int def);
size_t online_json_desc(char *out, size_t cap, const online_desc *d, const char *extra_members);

#ifdef __cplusplus
}
#endif

#endif /* ONLINE_HTTP_H */
