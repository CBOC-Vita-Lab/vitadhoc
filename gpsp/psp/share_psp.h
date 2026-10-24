/* share_psp.h -- "Share game data" on the PSP: the radio, the stick and
 * this build's identity, as frontend-common/fe_share.h's callbacks want
 * them (docs/SAVE-SHARE.md).  The screens are psp/ui_share.inc. */
#ifndef SHARE_PSP_H
#define SHARE_PSP_H

#include <stddef.h>
#include <stdint.h>

#include "fe_share.h"

/* The radio.  A share room is an ad-hoc group like any other (its name is
 * room_make(ROOM_SHARE, ...)), with the transport's bulk lane and nothing
 * else on it: no netdrv, no link session.  `host` creates the group, a
 * guest joins the scanned one and never creates it.  Blocks for the
 * bring-up (seconds).  0 = up, else an ADHOC_ERR_* code. */
int  share_net_up(int host, const char *group);
void share_net_down(void);
int  share_net_is_up(void);
/* 1 while adhocctl still reports the group (0 = the radio dropped it). */
int  share_net_connected(void);
void share_net_send(void *user, const uint8_t mac[6], const void *buf,
                    size_t len);
/* One datagram into buf (cap >= FE_SHARE_DGRAM_MAX); 0 = none. */
int  share_net_recv(void *buf, size_t cap);
unsigned share_now_ms(void);

#ifdef VITA
/* The Vita's radio (vita/share_vita.c): Wi-Fi rooms on transport_lan.  Its
 * results use the PSP transport's names, which the screens test:
 *   ADHOC_OK            up
 *   ADHOC_ERR_WLAN_OFF  not connected to Wi-Fi
 *   ADHOC_ERR_NO_GROUP  (join) no room of that name answered
 *   ADHOC_ERR_NP_START  the room could not open (no memory, the port, the
 *                       radio held by a link session) */
#ifndef ADHOC_OK
enum { ADHOC_OK = 0, ADHOC_ERR_WLAN_OFF = -1, ADHOC_ERR_NP_START = -14,
       ADHOC_ERR_NO_GROUP = -15 };
#endif
/* This console's address as the bulk lane names it (zeros while down). */
void share_net_local_mac(uint8_t mac[6]);
#endif

/* Who this console is: the core's build string, each core's state format,
 * the radio's MAC (valid once the radio is up).  Harness `share_build =
 * <text>` replaces the build string, to stage a mismatch with one EBOOT. */
void share_ident(fe_share_ident *out);
void share_build_override(const char *build);

/* sceIo behind fe_share_fs. */
extern const fe_share_fs share_fs;

/* A file under construction: fe_share_sink over one sceIo handle.  The
 * name is always the final name + SHARE_TMP_SUFFIX, never the final. */
#define SHARE_TMP_SUFFIX ".gsp"
typedef struct { int fd; unsigned written, fail_after, len; } share_file;
int  share_file_create(share_file *f, const char *path);
void share_file_close(share_file *f);
int  share_file_write(void *user, uint32_t off, const void *buf,
                      uint32_t len);
int  share_file_read(void *user, uint32_t off, void *buf, uint32_t len);
/* Harness `share_fail_write_after = N`: every temp file refuses to grow
 * past N bytes, as a full Memory Stick does. */
void share_fail_write_after(unsigned bytes);

#endif /* SHARE_PSP_H */
