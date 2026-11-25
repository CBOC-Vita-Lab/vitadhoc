/* SPDX-License-Identifier: MIT OR GPL-2.0-or-later
 * Copyright (c) 2026 VitAdhoc contributors */
/* update_check.c -- see update_check.h. */
#include "update_check.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "fe_evt.h"
#include "nethttps/np_https.h"
#if !defined(NP_TARGET_VITA)
#include "nethttps/np_http.h"
#endif
#include "update_ver.h"

#define UPD_MAX_ANSWER (256u * 1024u)   /* a release with many assets is ~30 KiB */

static struct
{
   int started, daily;
   char path[600];                      /* update.txt */
   char url[300];
   char version[24];
   int (*online)(void);
   unsigned timeout_ms;
   time_t last;                         /* the last request's day (0 = never) */
   time_t last_before;                  /* ...and what it was before the one in flight */
   char latest[24];                     /* the newest newer version known    */
   char announced[24];                  /* the one the home screen has said (update.txt) */
   time_t retry_at;                     /* not online: do not ask the network before */
   NpHttpsHandle h;
   int plain, by_hand;                  /* the request in flight             */
   int state;
   int result_pending, result_kind, result_hand;
   int noticed;                         /* the home screen has said it        */
   int said_busy;
} U;

/* ---- update.txt: checked=<seconds>, latest=<version>, announced=<version> -- */

static void load(void)
{
   FILE *f;
   char line[80];
   U.last = 0;
   U.latest[0] = '\0';
   U.announced[0] = '\0';
   f = fopen(U.path, "r");
   if (!f)
      return;
   while (fgets(line, sizeof(line), f))
   {
      char *nl = strchr(line, '\n');
      if (nl)
         *nl = '\0';
      if (!strncmp(line, "checked=", 8))
         U.last = (time_t)atoll(line + 8);
      else if (!strncmp(line, "latest=", 7))
         snprintf(U.latest, sizeof(U.latest), "%.23s", line + 7);
      else if (!strncmp(line, "announced=", 10))
         snprintf(U.announced, sizeof(U.announced), "%.23s", line + 10);
   }
   fclose(f);
   /* what is remembered counts only while it is newer than this app (the
    * player may have installed it since) */
   if (U.latest[0] && !upd_ver_newer(U.latest, U.version))
      U.latest[0] = '\0';
}

static void save(void)
{
   FILE *f;
   char tmp[620];
   if (!U.path[0])
      return;
   snprintf(tmp, sizeof(tmp), "%s.tmp", U.path);
   f = fopen(tmp, "w");
   if (!f)
      return;
   fprintf(f, "checked=%lld\nlatest=%s\n", (long long)U.last, U.latest);
   if (U.announced[0])
      fprintf(f, "announced=%s\n", U.announced);
   if (fclose(f) != 0 || rename(tmp, U.path) != 0)
      remove(tmp);
}

/* ---- the setting and the state ------------------------------------------- */

void upd_start(const char *root, const char *url, const char *version,
               int (*online)(void), unsigned timeout_ms, int daily)
{
   memset(&U, 0, sizeof(U));
   snprintf(U.url, sizeof(U.url), "%s", url && *url ? url : UPD_URL_DEFAULT);
   snprintf(U.version, sizeof(U.version), "%s", version ? version : "");
   if (root && *root)
      snprintf(U.path, sizeof(U.path), "%s/update.txt", root);
   U.online = online;
   U.timeout_ms = timeout_ms ? timeout_ms : 10000;
   U.daily = daily ? 1 : 0;
   U.started = 1;
   load();
   U.state = U.latest[0] ? UPD_NEWER : UPD_UNKNOWN;
}

void upd_set_daily(int daily) { U.daily = daily ? 1 : 0; }
int  upd_daily(void) { return U.daily; }
int  upd_state(void) { return U.state; }
int  upd_busy(void) { return U.h != 0; }
const char *upd_latest(void) { return U.latest; }

void upd_notice_text(const char *shown, char *out, size_t cap)
{
   snprintf(out, cap, "VitAdhoc %s is out: get it with the CBOC Vita Installer "
            "or from GitHub", shown);
}

int upd_take_notice(char *text, size_t cap, int by_hand)
{
   if (!U.started || !U.latest[0] || U.h)
      return 0;
   if (!by_hand && (U.noticed || !U.daily || !strcmp(U.announced, U.latest)))
      return 0;
   U.noticed = 1;
   /* once per new version, not once a run: the version said is kept */
   if (strcmp(U.announced, U.latest) != 0)
   {
      snprintf(U.announced, sizeof(U.announced), "%s", U.latest);
      save();
   }
   upd_notice_text(U.latest, text, cap);
   return 1;
}

int upd_take_result(int *kind, int *by_hand)
{
   if (!U.result_pending)
      return 0;
   U.result_pending = 0;
   if (kind)
      *kind = U.result_kind;
   if (by_hand)
      *by_hand = U.result_hand;
   return 1;
}

/* ---- a request ---------------------------------------------------------- */

static int begin(time_t now, int by_hand)
{
   NpHttpsRequest rq;
   NpHttpsError err = NP_HTTPS_OK;

   if (U.h || !U.started)
      return -1;
   if (U.online && !U.online())
      return -1;
   /* The User-Agent is the HTTPS client's own, the one every request of the
    * app has ("VitAdhoc/<version> (PlayStation Vita)", set once by the online
    * play client): it is not changed here, where a room request may be
    * under way and would go out with another name from then on. */
   memset(&rq, 0, sizeof(rq));
   rq.url = U.url;
   rq.method = "GET";
   rq.connect_timeout_ms = U.timeout_ms;
   rq.io_timeout_ms = U.timeout_ms;
   rq.total_timeout_ms = U.timeout_ms * 2;
   rq.max_response = UPD_MAX_ANSWER;
   U.plain = 0;
#if !defined(NP_TARGET_VITA)
   if (!strncmp(U.url, "http://", 7))
   {
      U.plain = 1;
      U.h = np_http_start(&rq, &err);
   }
   else
#endif
      U.h = np_https_start(&rq, &err);
   /* a try counts from here, answered or not: at most one a day */
   U.last_before = U.last;
   U.last = now;
   save();
   if (!U.h)
   {
      fe_evt("update_check start_failed err=%s by_hand=%d",
             np_https_error_name(err), by_hand);
      U.state = UPD_FAILED;
      U.result_pending = 1;
      U.result_kind = UPD_FAILED;
      U.result_hand = by_hand;
      return -1;
   }
   U.by_hand = by_hand;
   fe_evt("update_check start by_hand=%d", by_hand);
   return 0;
}

static void finish(int kind, const char *shown)
{
   if (kind == UPD_NEWER)
      snprintf(U.latest, sizeof(U.latest), "%s", shown);
   else if (kind == UPD_CURRENT)
      U.latest[0] = '\0';
   /* a failed try does not forget a newer version learnt before */
   U.state = kind == UPD_FAILED && U.latest[0] ? UPD_NEWER : kind;
   save();
   U.result_pending = 1;
   U.result_kind = kind;
   U.result_hand = U.by_hand;
   if (kind == UPD_NEWER)
      U.noticed = 0;                 /* the home screen may say it now */
}

static void collect(void)
{
   NpHttpsResult r;
   int rc;
   char shown[24];

   memset(&r, 0, sizeof(r));
#if !defined(NP_TARGET_VITA)
   rc = U.plain ? np_http_poll(U.h, &r) : np_https_poll(U.h, &r);
#else
   rc = np_https_poll(U.h, &r);
#endif
   if (rc == 0)
      return;
   if (rc < 0)
   {
      U.h = 0;
      finish(UPD_FAILED, NULL);
      return;
   }
   if (r.error != NP_HTTPS_OK)
   {
      fe_evt("update_check failed err=%s stage=%s http=%d ms=%u",
             np_https_error_name(r.error), np_https_stage_name(r.stage),
             r.http_status, r.elapsed_ms);
      finish(UPD_FAILED, NULL);
   }
   else if (r.http_status != 200)
   {
      fe_evt("update_check failed http=%d", r.http_status);
      finish(UPD_FAILED, NULL);
   }
   else if (!upd_release_parse((const char *)r.body, r.body_len, shown,
                               sizeof(shown)))
   {
      /* a draft, a pre-release, not JSON, a tag that is no version */
      fe_evt("update_check unusable_answer bytes=%u", (unsigned)r.body_len);
      finish(UPD_FAILED, NULL);
   }
   else if (upd_ver_newer(shown, U.version))
   {
      fe_evt("update_check newer latest=%s mine=%s", shown, U.version);
      finish(UPD_NEWER, shown);
   }
   else
   {
      fe_evt("update_check current latest=%s mine=%s", shown, U.version);
      finish(UPD_CURRENT, NULL);
   }
#if !defined(NP_TARGET_VITA)
   if (U.plain)
      np_http_release(U.h);
   else
#endif
      np_https_release(U.h);
   U.h = 0;
}

void upd_poll(int idle, time_t now)
{
   if (!U.started)
      return;
   if (U.h)
   {
      collect();
      return;
   }
   if (!U.daily || idle < UPD_IDLE_FRAMES)
      return;
   /* due: never asked, or a day ago -- or the clock went back */
   if (U.last && now >= U.last && now - U.last < UPD_DAY_S)
      return;
   /* Wi-Fi off: ask the system again in a few seconds, not every idle frame */
   if (U.retry_at && now < U.retry_at && now + UPD_RETRY_S >= U.retry_at)
      return;
   if (U.online && !U.online())
   {
      U.retry_at = now + UPD_RETRY_S;
      return;
   }
   U.retry_at = 0;
   (void)begin(now, 0);
}

int upd_check_now(time_t now)
{
   if (!U.started || U.h)
      return -1;
   return begin(now, 1);
}

void upd_stop(void)
{
   if (!U.started)
      return;
   if (U.h)
   {
      /* dropped, not answered: it is not the day's try (the next idle home
       * screen asks again) */
      fe_evt("update_check dropped by_hand=%d", U.by_hand);
      U.last = U.last_before;
      save();
#if !defined(NP_TARGET_VITA)
      if (U.plain)
      {
         np_http_cancel(U.h);
         np_http_release(U.h);
      }
      else
#endif
      {
         np_https_cancel(U.h);
         np_https_release(U.h);
      }
      U.h = 0;
   }
}
