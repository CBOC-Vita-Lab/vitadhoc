/* ra_http.h -- one web request at a time, on a thread of its own.
 *
 * RetroAchievements' client (ra_vita.c) asks its server things: sign in,
 * which achievements a game has, this one was earned.  The answers take
 * from a tenth of a second to tens of them, and a frame has 16.7 ms, so the
 * request runs on plat_net_start()'s thread and the game thread collects
 * the answer when it is there:
 *
 *     ra_http_submit(url, post, type)    0 = taken (nothing else in flight)
 *     ... frames ...
 *     ra_http_take(&res)                 1 = here is the answer, once
 *
 * http:// is spoken here over the C library's sockets (the twin's tests talk
 * to a server on the loopback); https:// is the Vita's own HTTP and TLS
 * library (SceHttp/SceSsl), given the root certificates of ra_roots.h beside
 * the console's store.  The twin has no TLS: an https:// request fails.
 *
 * An answer's status is the HTTP status, or RA_HTTP_RETRY when the server
 * was not reached (no Wi-Fi, the radio in ad hoc mode, a timeout, TLS) --
 * rcheevos' RC_API_SERVER_RESPONSE_RETRYABLE_CLIENT_ERROR, which it retries
 * later.  `note` says what went wrong, for the log (the thread does not
 * write the log itself: fe_evt has one producer). */
#ifndef RA_HTTP_H
#define RA_HTTP_H

#include <stddef.h>
#include <stdint.h>

#define RA_HTTP_RETRY      (-2)
#define RA_HTTP_MAX_BODY   (4u * 1024u * 1024u)

typedef struct ra_http_res
{
   int    status;
   char  *body;          /* malloc'd, NUL-terminated; the taker frees it */
   size_t len;
   char   note[96];
} ra_http_res;

/* The User-Agent (before the first request) and what says the network may
 * be used now (NULL = always; the twin). */
void ra_http_config(const char *user_agent, int (*online)(void));
/* 0 = taken; -1 = a request is in flight (or the thread cannot start). */
int  ra_http_submit(const char *url, const char *post, const char *type);
/* The other job the thread takes: the MD5 of a whole file (a game's
 * RetroAchievements hash -- reading 32 MiB from the card is seconds, not a
 * frame's work).  Same slot, same contract; the answer's status is 0 and
 * `note` the 32 hex digits, or -1 when the file could not be read. */
int  ra_http_hash_file(const char *path);
/* 1 = an answer, moved into *res; 0 = none (yet). */
int  ra_http_take(ra_http_res *res);
/* 1 = nothing in flight and nothing waiting to be taken. */
int  ra_http_idle(void);
/* At exit: the thread is told to stop and given timeout_us to do so. */
void ra_http_stop(uint32_t timeout_us);

/* Pure: split http://host[:port]/path.  0 = done. */
int  ra_http_split_url(const char *url, char *host, size_t hcap, int *port,
                       const char **path);

#endif
