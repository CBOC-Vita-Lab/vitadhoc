/* SPDX-License-Identifier: MIT OR GPL-2.0-or-later
 * Copyright (c) 2026 VitAdhoc contributors */
/* update_check.h -- is a newer VitAdhoc out?  (Settings > About > Check for
 * updates.)
 *
 * It asks GitHub's public releases API for the latest release of
 * CBOC-Vita-Lab/vitadhoc:
 *
 *    https://api.github.com/repos/CBOC-Vita-Lab/vitadhoc/releases/latest
 *
 * with the app's own HTTPS client (nethttps/, Mbed TLS, its own root
 * certificates: nethttps/np_https_roots.h lists Sectigo's, which GitHub's
 * certificates chain to), compares the release's tag with this app's version
 * (update_ver.h: numbers, newest wins; drafts and pre-releases are not
 * releases) and, when the tag is newer, says so once -- a small notice on
 * the home screen: "VitAdhoc 2.1 is out: get it with the CBOC Vita Installer
 * or from GitHub".  It never downloads or installs anything.
 *
 * WHEN.  Settings > About > Check for updates: Daily (the default) or Off.
 * Daily: at most one request a day (the time of the last one is in
 * update.txt in the data folder), made in the background while the home
 * screen has been idle for a couple of seconds and the network may be used
 * (Wi-Fi is up and the radio is not in ad hoc mode, no link session: the
 * same rule as the box art's and RetroAchievements' fetching).  Off: no
 * request at all -- except the one the player asks for with X on the row.
 *
 * WHAT IS SENT.  One GET with a User-Agent of "VitAdhoc/<version>" and the
 * client's standard headers.  No account, no console or device id, no
 * cookie, nothing about the player's games.  A failure of any kind (no
 * network, a certificate, a timeout, an answer that is not a release) is
 * logged and otherwise silent: the next try is tomorrow's.
 *
 * Portable C over the HTTPS client; on the twin, "http://" URLs go to the
 * plain-HTTP stand-in (vita.ini update_url = http://127.0.0.1:PORT/path), and
 * with no update_url the twin asks nobody. */
#ifndef GBADHOC_UPDATE_CHECK_H
#define GBADHOC_UPDATE_CHECK_H

#include <stddef.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

#define UPD_URL_DEFAULT \
   "https://api.github.com/repos/CBOC-Vita-Lab/vitadhoc/releases/latest"
#define UPD_DAY_S      86400
#define UPD_IDLE_FRAMES 120      /* the home screen untouched for ~2 s       */

/* Once, at start-up.  `root` is the data folder (update.txt is in it);
 * `url` NULL = GitHub's; `version` this app's ("2.0.0"); `online` says
 * whether the network may be used now (NULL = always: the twin);
 * `timeout_ms` 0 = 10 s for a request.  `daily`: the setting. */
void upd_start(const char *root, const char *url, const char *version,
               int (*online)(void), unsigned timeout_ms, int daily);
void upd_set_daily(int daily);
int  upd_daily(void);

/* Once a frame from the home screen, with the frames it has been idle for
 * and the time now (the console's clock, seconds): starts the day's check
 * when it is due, collects the answer.  UI thread. */
void upd_poll(int idle, time_t now);

/* X on the row: check now, whatever the setting says and the day.  0 =
 * started; -1 = one is already under way, or the network may not be used
 * now. */
int  upd_check_now(time_t now);

enum { UPD_UNKNOWN = 0,     /* no answer yet (never asked, or under way)     */
       UPD_CURRENT,         /* this version is the newest there is           */
       UPD_NEWER,           /* a newer one is out: upd_latest()              */
       UPD_FAILED };        /* the last try got no usable answer             */
int  upd_state(void);
int  upd_busy(void);
/* The newer version's number ("2.1") while upd_state() == UPD_NEWER; the
 * newest remembered from an earlier day counts too. */
const char *upd_latest(void);

/* 1 = the answer of a check the player asked for (or one made in the
 * background) has just come: say it.  Once each.  *kind: UPD_*; the
 * UI thread only. */
int  upd_take_result(int *kind, int *by_hand);
/* 1 once per run when a newer version is known and the home screen has not
 * said so yet: *text is the notice.  Not with the setting Off.  `by_hand`: the
 * answer to X on the row, which is always told (and counts as said). */
int  upd_take_notice(char *text, size_t cap, int by_hand);

/* The words of the notice for version `shown`. */
void upd_notice_text(const char *shown, char *out, size_t cap);

/* At exit: an unfinished request is cancelled. */
void upd_stop(void);

#ifdef __cplusplus
}
#endif

#endif /* GBADHOC_UPDATE_CHECK_H */
