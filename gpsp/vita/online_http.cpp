// online_http.cpp -- the room client of online play (2.0); see online_http.h and ONLINE-2.0-PLAN.md 1.2, 1.3, 1.5, 2.2, 2.3.
//
// One function per route of the room service.  Every call returns at once (it only queues a request on the vendored Mbed TLS client,
// nethttps/np_https.*, which has its own worker thread, or, on the twin only, on the plain-HTTP stand-in nethttps/np_http_plain.cpp);
// online_http_take() collects one decoded answer per call.  Nothing here blocks the frame thread.
//
// Why a flat JSON reader and no JSON library: the service only ever stores and returns strings of printable ASCII without a double
// quote or a backslash (PROTOCOL 1.1), so a reader that walks objects and arrays and refuses any body with a backslash is exact.
//
// Never logged: a token, a relay credential, an address.  The client's own log lines carry the service's host name and the outcome of
// each request only; they reach fe_evt from take() (the frame thread: fe_evt has one producer), never from the worker threads.
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "online_http.h"

#include "fe_evt.h"
#include "nethttps/np_https.h"
#include "nethttps/np_https_sys.h"
#if !defined(NP_TARGET_VITA)
#include "nethttps/np_http.h"
#endif

namespace {

// ---- settings ---------------------------------------------------------------------------------------------------------------------

const char kDefaultUrl[] = "https://vitadhoc.psvita.dev";
const uint32_t kConnectMs = 6000, kIoMs = 6000, kTotalMs = 8000;   // 8 s for the whole request (plan H)
const uint32_t kMaxAnswer = 4096;                                  // a longer answer is ONLINE_E_BAD
const int kSlots = 3;
const uint32_t kResolveMs = 4000;

char g_base[200];
char g_ver[16];
bool g_configured;

// ---- the log queue: worker threads push, take() hands to fe_evt -------------------------------------------------------------------
struct LogRing {
    char line[24][200];
    int head, tail;   // push at head, pop at tail
    nph::SpinLock lock;
};
LogRing g_log;

void LogPush(const char* text) {
    g_log.lock.lock();
    const int next = (g_log.head + 1) % 24;
    if (next != g_log.tail) {   // full: the newest line is dropped (the log is a help, never a cost)
        snprintf(g_log.line[g_log.head], sizeof g_log.line[0], "%s", text);
        g_log.head = next;
    }
    g_log.lock.unlock();
}

void LogDrain() {   // frame thread only
    for (;;) {
        char buf[200];
        g_log.lock.lock();
        if (g_log.tail == g_log.head) { g_log.lock.unlock(); return; }
        memcpy(buf, g_log.line[g_log.tail], sizeof buf);
        g_log.tail = (g_log.tail + 1) % 24;
        g_log.lock.unlock();
        fe_evt("online_tls %s", buf);
    }
}

// ---- slots --------------------------------------------------------------------------------------------------------------------------

enum { K_NONE = 0, K_TLS, K_PLAIN, K_LOCAL, K_RESOLVE };

struct ResolveJob {
    char host[64];
    volatile int cancel, done;
    int refs;                 // 2 initially (caller Slot/Zombie + ResolveMain worker); last ref deletes
    uint32_t ip;
    int rc;
};

void ReleaseResolveJob(ResolveJob* j) {
    if (j && __atomic_sub_fetch(&j->refs, 1, __ATOMIC_ACQ_REL) == 0)
        delete j;
}

struct Slot {
    int kind;
    int op;
    NpHttpsHandle h;
    online_reply local;       // K_LOCAL: the answer, already made
    ResolveJob* job;          // K_RESOLVE
    nph::Thread* th;
};
Slot g_slot[kSlots];

struct Zombie { ResolveJob* job; nph::Thread* th; };   // a resolve whose caller dropped it: reaped once its thread ends
const int kZombies = 16;
Zombie g_zombie[kZombies];

void ReapZombies() {
    for (int i = 0; i < kZombies; i++) {
        Zombie& z = g_zombie[i];
        if (!z.job) continue;
        if (!__atomic_load_n(&z.job->done, __ATOMIC_ACQUIRE)) continue;
        if (!nph::ThreadJoin(z.th, 50)) continue;
        ReleaseResolveJob(z.job);
        z.job = nullptr;
        z.th = nullptr;
    }
}

void ResolveMain(void* arg) {
    ResolveJob* j = (ResolveJob*)arg;
    int sys = 0;
    j->rc = nph::Resolve(j->host, &j->ip, kResolveMs, &j->cancel, &sys);
    __atomic_store_n(&j->done, 1, __ATOMIC_RELEASE);
    ReleaseResolveJob(j);
}

// ---- the flat JSON reader --------------------------------------------------------------------------------------------------------

const char* Ws(const char* p) {
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
    return p;
}

// After the string that starts at p (a quote); null if it is not terminated or holds anything but printable ASCII.  The service
// only ever sends 0x20..0x7E inside a string (PROTOCOL 1.1); a control character, DEL or a byte with the top bit set means the
// answer is not the service's, and it is refused whole instead of handing such bytes to the screen, the log or an address parser.
const char* SkipString(const char* p) {
    if (*p != '"') return nullptr;
    for (p++; *p; p++) {
        if (*p == '"') return p + 1;
        if ((unsigned char)*p < 0x20 || (unsigned char)*p > 0x7E) return nullptr;
    }
    return nullptr;
}

const char* SkipValue(const char* p, int depth) {
    p = Ws(p);
    if (depth > 8) return nullptr;
    if (*p == '"') return SkipString(p);
    if (*p == '{' || *p == '[') {
        const char close = *p == '{' ? '}' : ']';
        p = Ws(p + 1);
        if (*p == close) return p + 1;
        for (;;) {
            if (close == '}') {
                p = SkipString(Ws(p));
                if (!p) return nullptr;
                p = Ws(p);
                if (*p != ':') return nullptr;
                p++;
            }
            p = SkipValue(p, depth + 1);
            if (!p) return nullptr;
            p = Ws(p);
            if (*p == ',') { p = Ws(p + 1); continue; }
            if (*p == close) return p + 1;
            return nullptr;
        }
    }
    const char* q = p;
    while (isalnum((unsigned char)*q) || *q == '-' || *q == '+' || *q == '.') q++;
    if (q == p) return nullptr;
    return q;
}

// Finds the value at `path` ("peer.pub", "servers.0.username"): *v = its first character, *end = after it.
bool Find(const char* body, const char* path, const char** v, const char** end) {
    if (!body || !path || !*path || strchr(body, '\\')) return false;
    const char* p = Ws(body);
    const char* c = path;
    while (*c) {
        const char* dot = strchr(c, '.');
        const size_t cl = dot ? (size_t)(dot - c) : strlen(c);
        if (cl == 0) return false;
        if (*p == '{') {
            p = Ws(p + 1);
            bool found = false;
            while (*p && *p != '}') {
                const char* ke = SkipString(p);
                if (!ke) return false;
                const bool match = (size_t)(ke - p) == cl + 2 && memcmp(p + 1, c, cl) == 0;
                p = Ws(ke);
                if (*p != ':') return false;
                p = Ws(p + 1);
                if (match) { found = true; break; }
                p = SkipValue(p, 1);
                if (!p) return false;
                p = Ws(p);
                if (*p == ',') p = Ws(p + 1);
                else if (*p != '}') return false;
            }
            if (!found) return false;
        } else if (*p == '[') {
            size_t idx = 0;
            for (size_t i = 0; i < cl; i++) {
                if (!isdigit((unsigned char)c[i]) || idx > 1000) return false;
                idx = idx * 10 + (size_t)(c[i] - '0');
            }
            p = Ws(p + 1);
            for (size_t i = 0; i < idx; i++) {
                if (*p == ']') return false;
                p = SkipValue(p, 1);
                if (!p) return false;
                p = Ws(p);
                if (*p != ',') return false;
                p = Ws(p + 1);
            }
            if (*p == ']') return false;
        } else {
            return false;
        }
        c = dot ? dot + 1 : c + cl;
    }
    const char* e = SkipValue(p, 1);
    if (!e) return false;
    *v = p;
    *end = e;
    return true;
}

// ---- small helpers ------------------------------------------------------------------------------------------------------------------

void CopyStr(char* dst, size_t cap, const char* src) {
    if (!cap) return;
    snprintf(dst, cap, "%s", src ? src : "");
}

// Printable ASCII only and no quote or backslash: what the service would sanitise anyway (PROTOCOL 1.1).  Returns the new length.
size_t Clean(char* out, size_t cap, const char* s) {
    size_t n = 0;
    for (; s && *s && n + 1 < cap; s++)
        out[n++] = ((unsigned char)*s < 0x20 || (unsigned char)*s > 0x7E || *s == '"' || *s == '\\') ? '?' : *s;
    if (cap) out[n] = 0;
    return n;
}

// Appends `"key":"value"` (or `"key":123`) at out[*n]; false when it does not fit.
bool PutStr(char* out, size_t cap, size_t* n, const char* key, const char* val, bool comma) {
    char tmp[96];
    Clean(tmp, sizeof tmp, val);
    const int w = snprintf(out + *n, cap - *n, "%s\"%s\":\"%s\"", comma ? "," : "", key, tmp);
    if (w < 0 || (size_t)w >= cap - *n) return false;
    *n += (size_t)w;
    return true;
}

bool PutInt(char* out, size_t cap, size_t* n, const char* key, int val) {
    const int w = snprintf(out + *n, cap - *n, ",\"%s\":%d", key, val);
    if (w < 0 || (size_t)w >= cap - *n) return false;
    *n += (size_t)w;
    return true;
}

int Clamp(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

int ParseRoomState(const char* s) {
    if (!strcmp(s, "open")) return ONLINE_ROOM_OPEN;
    if (!strcmp(s, "full")) return ONLINE_ROOM_FULL;
    if (!strcmp(s, "joined")) return ONLINE_ROOM_JOINED;
    if (!strcmp(s, "relay")) return ONLINE_ROOM_RELAY;
    if (!strcmp(s, "closed")) return ONLINE_ROOM_CLOSED;
    return -1;
}

uint32_t HexVal(const char* s) {
    uint32_t v = 0;
    for (; *s; s++) {
        const int c = tolower((unsigned char)*s);
        if (!isxdigit(c)) return 0;
        v = (v << 4) | (uint32_t)(c <= '9' ? c - '0' : c - 'a' + 10);
    }
    return v;
}

// ---- mapping the answer ---------------------------------------------------------------------------------------------------------

// `reason` is the error object's extra member of a 429 (rate_limited: slow_down, daily_cap, ...): daily_cap is the day-long one.
int MapStatus(int status, const char* code, const char* reason) {
    switch (status) {
    case 404: return ONLINE_E_NOT_FOUND;
    case 409:
        if (!strcmp(code, "room_full")) return ONLINE_E_FULL;
        if (!strcmp(code, "version_mismatch")) return ONLINE_E_VERSION;
        return ONLINE_E_BAD;
    case 426: return ONLINE_E_UPDATE;
    case 401: return ONLINE_E_TOKEN;
    case 403: return !strcmp(code, "relay_not_allowed") ? ONLINE_E_RELAY_WAIT : ONLINE_E_RELAY;
    case 502: return ONLINE_E_RELAY;
    case 429: return !strcmp(reason, "daily_cap") || !strcmp(code, "daily_cap") ? ONLINE_E_BUSY : ONLINE_E_RATE;
    case 503:
        if (!strcmp(code, "over_budget") || !strcmp(code, "rooms_full") || !strcmp(code, "switched_off")) return ONLINE_E_BUSY;
        if (!strncmp(code, "relay_", 6)) return ONLINE_E_RELAY;
        return ONLINE_E_SERVER;
    case 400: case 413: return ONLINE_E_BAD;
    default: break;
    }
    if (status >= 500 && status < 600) return ONLINE_E_SERVER;
    return ONLINE_E_BAD;   // any other status the service never sends: not a transport failure, not usable
}

bool Str(const char* body, const char* path, char* out, size_t cap) { return online_json_get(body, path, out, cap) != 0; }

// "turn:host:port?transport=udp" -> host, port; false for any other url.
bool ParseTurnUrl(const char* url, char* host, size_t hcap, int* port) {
    if (strncmp(url, "turn:", 5) != 0) return false;
    const char* q = strchr(url, '?');
    if (!q || !strstr(q, "transport=udp")) return false;
    const char* h = url + 5;
    const char* colon = nullptr;
    for (const char* p = h; p < q; p++)
        if (*p == ':') colon = p;
    const char* he = colon ? colon : q;
    if (he == h || (size_t)(he - h) >= hcap) return false;
    for (const char* p = h; p < he; p++)
        if (!(isalnum((unsigned char)*p) || *p == '-' || *p == '.')) return false;
    int pt = 3478;
    if (colon) {
        pt = 0;
        for (const char* p = colon + 1; p < q; p++) {
            if (!isdigit((unsigned char)*p) || pt > 6553) return false;
            pt = pt * 10 + (*p - '0');
        }
    }
    if (pt < 1 || pt > 65535) return false;
    memcpy(host, h, (size_t)(he - h));
    host[he - h] = 0;
    *port = pt;
    return true;
}

// The first server entry with a username: its UDP turn urls, ports ordered 3478, 443, then the others in the order listed (at most 3).
bool ParseServers(const char* body, online_reply* r) {
    for (int i = 0; i < 8; i++) {
        char path[48], user[200];
        snprintf(path, sizeof path, "servers.%d.username", i);
        if (!Str(body, path, user, sizeof user) || !user[0]) continue;
        if (strlen(user) >= sizeof r->turn_user) return false;
        char cred[200];
        snprintf(path, sizeof path, "servers.%d.credential", i);
        if (!Str(body, path, cred, sizeof cred) || !cred[0] || strlen(cred) >= sizeof r->turn_cred) return false;
        int ports[8], np = 0;
        char host[64] = "";
        for (int j = 0; j < 8; j++) {
            char url[200], h[64];
            int pt;
            snprintf(path, sizeof path, "servers.%d.urls.%d", i, j);
            if (!Str(body, path, url, sizeof url)) break;
            if (!ParseTurnUrl(url, h, sizeof h, &pt)) continue;
            if (!host[0]) strcpy(host, h);
            else if (strcmp(host, h) != 0) continue;
            bool dup = false;
            for (int k = 0; k < np; k++) dup = dup || ports[k] == pt;
            if (!dup && np < 8) ports[np++] = pt;
        }
        if (!host[0] || np == 0) return false;
        CopyStr(r->turn_host, sizeof r->turn_host, host);
        CopyStr(r->turn_user, sizeof r->turn_user, user);
        CopyStr(r->turn_cred, sizeof r->turn_cred, cred);
        r->turn_ports = 0;
        const int order[2] = {3478, 443};
        for (int o = 0; o < 2; o++)
            for (int k = 0; k < np; k++)
                if (ports[k] == order[o] && r->turn_ports < 3) r->turn_port[r->turn_ports++] = (uint16_t)order[o];
        for (int k = 0; k < np; k++)
            if (ports[k] != 3478 && ports[k] != 443 && r->turn_ports < 3) r->turn_port[r->turn_ports++] = (uint16_t)ports[k];
        return true;
    }
    return false;
}

void ParsePeer(const char* body, online_reply* r) {
    char nick[32];
    r->have_peer = Str(body, "peer.nick", nick, sizeof nick) && nick[0];
    if (!r->have_peer) return;
    CopyStr(r->peer_nick, sizeof r->peer_nick, nick);
    Str(body, "peer.pub", r->peer_pub, sizeof r->peer_pub);
    Str(body, "peer.lan", r->peer_lan, sizeof r->peer_lan);
    r->peer_sym = online_json_int(body, "peer.sym", 0);
}

// A 2xx answer.  Returns false when a field the call cannot do without is missing (the answer is then ONLINE_E_BAD).
bool ParseOk(int op, const char* body, online_reply* r) {
    char tmp[64];
    switch (op) {
    case ONLINE_OP_CREATE:
        if (!Str(body, "code", r->code, sizeof r->code) || !Str(body, "token", r->token, sizeof r->token) || !r->code[0] || !r->token[0]) return false;
        Str(body, "punchKey", r->punch_key, sizeof r->punch_key);
        r->state = ONLINE_ROOM_OPEN;
        r->ttl_sec = online_json_int(body, "ttlSec", 0);
        r->poll_ms = Clamp(online_json_int(body, "pollMs", 2000), 1000, 15000);
        return true;
    case ONLINE_OP_LOOKUP: {
        online_desc& c = r->card;
        if (!Str(body, "state", tmp, sizeof tmp)) return false;
        r->state = ParseRoomState(tmp);
        if (r->state != ONLINE_ROOM_OPEN && r->state != ONLINE_ROOM_FULL) return false;
        if (!Str(body, "game", c.game, sizeof c.game) || !c.game[0]) return false;
        Str(body, "code", r->code, sizeof r->code);
        c.kind = online_json_int(body, "kind", 0);
        Str(body, "console", c.console, sizeof c.console);
        Str(body, "rom", c.rom, sizeof c.rom);
        Str(body, "title", c.title, sizeof c.title);
        Str(body, "nick", c.nick, sizeof c.nick);
        Str(body, "ver", c.ver, sizeof c.ver);
        if (Str(body, "compat", tmp, sizeof tmp)) c.compat = HexVal(tmp);
        return true;
    }
    case ONLINE_OP_JOIN:
        if (!Str(body, "token", r->token, sizeof r->token) || !r->token[0]) return false;
        Str(body, "punchKey", r->punch_key, sizeof r->punch_key);
        r->state = ONLINE_ROOM_JOINED;
        r->mode = Str(body, "mode", tmp, sizeof tmp) && !strcmp(tmp, "relay") ? ONLINE_MODE_RELAY : ONLINE_MODE_PUNCH;
        ParsePeer(body, r);
        r->punch_ms = online_json_int(body, "punchMs", 0);
        r->poll_ms = Clamp(online_json_int(body, "pollMs", 1500), 1000, 15000);
        return true;
    case ONLINE_OP_POLL:
        if (!Str(body, "state", tmp, sizeof tmp)) return false;
        r->state = ParseRoomState(tmp);
        if (r->state < 0 || r->state == ONLINE_ROOM_FULL) return false;
        r->mode = Str(body, "mode", tmp, sizeof tmp) && !strcmp(tmp, "relay") ? ONLINE_MODE_RELAY : ONLINE_MODE_PUNCH;
        ParsePeer(body, r);
        Str(body, "relayPeer", r->relay_peer, sizeof r->relay_peer);
        Str(body, "reason", r->reason, sizeof r->reason);
        r->punch_ms = online_json_int(body, "punchMs", 0);
        // the service's pollMs; when it says none, the table of 1.3 by state (open: its first 30 s step)
        r->poll_ms = Clamp(online_json_int(body, "pollMs", r->state == ONLINE_ROOM_OPEN ? 2000 : 1500), 1000, 15000);
        return true;
    case ONLINE_OP_RELAY:
        r->ttl_sec = online_json_int(body, "ttlSec", 0);
        return ParseServers(body, r);
    case ONLINE_OP_ENDPOINT:
        Str(body, "relayPeer", r->relay_peer, sizeof r->relay_peer);
        return true;
    case ONLINE_OP_INFO:
        Str(body, "min", r->min_ver, sizeof r->min_ver);
        Str(body, "latest", r->latest_ver, sizeof r->latest_ver);
        r->ttl_sec = online_json_int(body, "roomTtlSec", 0);
        return true;
    default:   // CLOSE: `{}`; anything that parses is fine
        return true;
    }
}

void FillReply(int op, const NpHttpsResult& res, online_reply* r) {
    r->op = op;
    r->http = res.http_status;
    if (res.error == NP_HTTPS_OK) {
        if (res.body_len > kMaxAnswer) { r->err = ONLINE_E_BAD; return; }
        // a 200 the call cannot use: for the credentials that means no relay server to be had
        r->err = ParseOk(op, (const char*)res.body, r) ? ONLINE_E_NONE : (op == ONLINE_OP_RELAY ? ONLINE_E_RELAY : ONLINE_E_BAD);
        return;
    }
    if (res.error == NP_HTTPS_E_HTTP_STATUS || res.error == NP_HTTPS_E_REDIRECT) {
        const char* body = (const char*)res.body;
        char code[32] = "", reason[32] = "";
        Str(body, "error.code", code, sizeof code);
        Str(body, "error.reason", reason, sizeof reason);
        r->err = res.error == NP_HTTPS_E_REDIRECT ? ONLINE_E_BAD : MapStatus(res.http_status, code, reason);
        if (r->err == ONLINE_E_BAD && res.http_status == 400) {
            char field[16] = "";
            Str(body, "error.field", field, sizeof field);
            if (!strcmp(field, "console") && !strcmp(code, "bad_request")) r->err = ONLINE_E_CONSOLE;     // an older service (one without a console id the app sends) says which field it refuses
        }
        r->retry_after = online_json_int(body, "error.retryAfterSec", 0);
        if (r->err == ONLINE_E_UPDATE) {
            Str(body, "error.min", r->min_ver, sizeof r->min_ver);
            Str(body, "error.latest", r->latest_ver, sizeof r->latest_ver);
        } else if (r->err == ONLINE_E_VERSION) {
            Str(body, "error.hostVer", r->host_ver, sizeof r->host_ver);
        }
        return;
    }
    if (res.error == NP_HTTPS_E_CLOCK) r->err = ONLINE_E_CLOCK;
    else if (res.error == NP_HTTPS_E_CERT) r->err = ONLINE_E_CERT;
    else if (res.error == NP_HTTPS_E_TOO_LARGE) r->err = ONLINE_E_BAD;   // an answer past 4096 bytes
    else r->err = ONLINE_E_NET;
}

// ---- sending ----------------------------------------------------------------------------------------------------------------------

int FreeSlot() {
    for (int i = 0; i < kSlots; i++)
        if (g_slot[i].kind == K_NONE) return i;
    return -1;
}

// An answer that needs no network (a bad code, a refused start): it waits in its slot for take().
int Local(int op, int err) {
    const int i = FreeSlot();
    if (i < 0) return -1;
    Slot& s = g_slot[i];
    memset(&s, 0, sizeof s);
    s.kind = K_LOCAL;
    s.op = op;
    s.local.op = op;
    s.local.err = err;
    return 0;
}

bool CodeOk(const char* code) {
    if (!code) return false;
    size_t n = 0;
    for (; code[n]; n++)
        if (!isalnum((unsigned char)code[n])) return false;
    return n >= 1 && n <= 8;
}

int StartReq(int op, const char* method, const char* path, const char* body) {
    if (!g_configured) return -1;
    const int i = FreeSlot();
    if (i < 0) return -1;
    char url[420];
    const int w = snprintf(url, sizeof url, "%s%s?v=%s", g_base, path, g_ver);
    if (w < 0 || (size_t)w >= sizeof url) return Local(op, ONLINE_E_BAD);
    NpHttpsRequest rq;
    memset(&rq, 0, sizeof rq);
    rq.url = url;
    rq.method = method;
    rq.body = body;
    rq.body_len = body ? strlen(body) : 0;
    rq.connect_timeout_ms = kConnectMs;
    rq.io_timeout_ms = kIoMs;
    rq.total_timeout_ms = kTotalMs;
    rq.max_response = kMaxAnswer;
    rq.keep_alive = 1;   // the polls of one wait reuse a TLS connection for 10 s: fewer handshakes, the same request count
    NpHttpsError err = NP_HTTPS_OK;
    int kind = K_TLS;
    NpHttpsHandle h;
#if !defined(NP_TARGET_VITA)
    if (strncmp(url, "http://", 7) == 0) { kind = K_PLAIN; h = np_http_start(&rq, &err); }
    else
#endif
        h = np_https_start(&rq, &err);
    if (!h) {
        if (err == NP_HTTPS_E_BUSY) return -1;
        return Local(op, ONLINE_E_NET);   // no TLS in this build, a refused url: the call is answered, it just never reaches a server
    }
    Slot& s = g_slot[i];
    memset(&s, 0, sizeof s);
    s.kind = kind;
    s.op = op;
    s.h = h;
    return 0;
}

int Poll(const Slot& s, NpHttpsResult* res) {
#if !defined(NP_TARGET_VITA)
    if (s.kind == K_PLAIN) return np_http_poll(s.h, res);
#endif
    return np_https_poll(s.h, res);
}

void Release(const Slot& s) {
#if !defined(NP_TARGET_VITA)
    if (s.kind == K_PLAIN) { np_http_release(s.h); return; }
#endif
    np_https_release(s.h);
}

// /^\d{1,4}(\.\d{1,4}){0,3}$/ : the service's own rule for `v` and `ver`.
bool VersionOk(const char* v) {
    if (!v) return false;
    int groups = 0;
    while (true) {
        int d = 0;
        while (isdigit((unsigned char)*v)) { v++; d++; }
        if (d < 1 || d > 4 || ++groups > 4) return false;
        if (*v == 0) return true;
        if (*v++ != '.') return false;
    }
}

int TokenOk(const char* t) { return t && *t; }

}  // namespace

// =================================================================================================================================
extern "C" {

void online_http_config(const char *base_url, const char *app_version)
{
   CopyStr(g_base, sizeof g_base, base_url && *base_url ? base_url : kDefaultUrl);
   for (size_t n = strlen(g_base); n && g_base[n - 1] == '/'; ) g_base[--n] = 0;
   /* The service refuses anything but digits and dots in `v` (400 bad_version): a build label such as "2.0-dev" must never go out,
    * so an unusable text becomes the release version (ONLINE_APP_VERSION of online_vita.h, the caller's normal argument). */
   CopyStr(g_ver, sizeof g_ver, VersionOk(app_version) ? app_version : "2.0.1");
   char ua[96];
   snprintf(ua, sizeof ua, "VitAdhoc/%s (PlayStation Vita)", g_ver);
   np_https_set_user_agent(ua);   // the same on the twin: the service refuses an empty one
   np_https_set_log(LogPush);
   g_configured = g_ver[0] != 0;
}

// The request id of create and join: 16 lower-case hex digits, or none.  Builds the extra member of the body.
static bool RidMember(const char *rid, char *out, size_t cap)
{
   out[0] = 0;
   if (!rid || !*rid) return true;
   if (strlen(rid) != 16) return false;
   for (int i = 0; i < 16; i++)
      if (!((rid[i] >= '0' && rid[i] <= '9') || (rid[i] >= 'a' && rid[i] <= 'f'))) return false;
   snprintf(out, cap, "\"rid\":\"%s\"", rid);
   return true;
}

int online_http_create(const online_desc *room, const char *rid)
{
   char body[768], extra[32];
   if (!room) return -1;
   if (!RidMember(rid, extra, sizeof extra)) return Local(ONLINE_OP_CREATE, ONLINE_E_BAD);
   if (!online_json_desc(body, sizeof body, room, extra)) return Local(ONLINE_OP_CREATE, ONLINE_E_BAD);
   return StartReq(ONLINE_OP_CREATE, "POST", "/v1/rooms", body);
}

int online_http_lookup(const char *code)
{
   char path[64];
   if (!CodeOk(code)) return Local(ONLINE_OP_LOOKUP, ONLINE_E_NOT_FOUND);   // the service answers 404 to such a code as well
   snprintf(path, sizeof path, "/v1/rooms/%s", code);
   return StartReq(ONLINE_OP_LOOKUP, "GET", path, nullptr);
}

int online_http_join(const char *code, const online_desc *me, const char *rid)
{
   char path[64], body[768], extra[32];
   if (!me) return -1;
   if (!CodeOk(code)) return Local(ONLINE_OP_JOIN, ONLINE_E_NOT_FOUND);
   if (!RidMember(rid, extra, sizeof extra)) return Local(ONLINE_OP_JOIN, ONLINE_E_BAD);
   if (!online_json_desc(body, sizeof body, me, extra)) return Local(ONLINE_OP_JOIN, ONLINE_E_BAD);
   snprintf(path, sizeof path, "/v1/rooms/%s/join", code);
   return StartReq(ONLINE_OP_JOIN, "POST", path, body);
}

int online_http_poll(const char *code, const char *token, const char *new_pub)
{
   char path[64], body[160], tk[40], pub[40];
   if (!TokenOk(token)) return -1;
   if (!CodeOk(code)) return Local(ONLINE_OP_POLL, ONLINE_E_NOT_FOUND);
   Clean(tk, sizeof tk, token);
   if (new_pub && *new_pub) {
      Clean(pub, sizeof pub, new_pub);
      snprintf(body, sizeof body, "{\"token\":\"%s\",\"pub\":\"%s\"}", tk, pub);
   }
   else
      snprintf(body, sizeof body, "{\"token\":\"%s\"}", tk);
   snprintf(path, sizeof path, "/v1/rooms/%s/poll", code);
   return StartReq(ONLINE_OP_POLL, "POST", path, body);
}

int online_http_relay(const char *code, const char *token)
{
   char path[64], body[96], tk[40];
   if (!TokenOk(token)) return -1;
   if (!CodeOk(code)) return Local(ONLINE_OP_RELAY, ONLINE_E_NOT_FOUND);
   Clean(tk, sizeof tk, token);
   snprintf(body, sizeof body, "{\"token\":\"%s\"}", tk);
   snprintf(path, sizeof path, "/v1/rooms/%s/relay", code);
   return StartReq(ONLINE_OP_RELAY, "POST", path, body);
}

int online_http_endpoint(const char *code, const char *token, const char *relay_addr)
{
   char path[64], body[160], tk[40], ra[40];
   if (!TokenOk(token) || !relay_addr) return -1;
   if (!CodeOk(code)) return Local(ONLINE_OP_ENDPOINT, ONLINE_E_NOT_FOUND);
   Clean(tk, sizeof tk, token);
   Clean(ra, sizeof ra, relay_addr);
   snprintf(body, sizeof body, "{\"token\":\"%s\",\"relay\":\"%s\"}", tk, ra);
   snprintf(path, sizeof path, "/v1/rooms/%s/endpoint", code);
   return StartReq(ONLINE_OP_ENDPOINT, "POST", path, body);
}

int online_http_close(const char *code, const char *token, const char *reason)
{
   char path[64], body[128], tk[40];
   if (!TokenOk(token)) return -1;
   if (!CodeOk(code)) return Local(ONLINE_OP_CLOSE, ONLINE_E_NOT_FOUND);
   if (!reason || (strcmp(reason, "linked") && strcmp(reason, "cancel") && strcmp(reason, "failed")))
      reason = "cancel";
   Clean(tk, sizeof tk, token);
   snprintf(body, sizeof body, "{\"token\":\"%s\",\"reason\":\"%s\"}", tk, reason);
   snprintf(path, sizeof path, "/v1/rooms/%s/close", code);
   return StartReq(ONLINE_OP_CLOSE, "POST", path, body);
}

int online_http_resolve(const char *host)
{
   unsigned a, b, c, d;
   char tail;
   int i;
   if (!g_configured) return -1;
   i = FreeSlot();
   if (i < 0) return -1;
   ReapZombies();
   if (!host || !*host || strlen(host) >= sizeof ((ResolveJob *)0)->host)
      return Local(ONLINE_OP_RESOLVE, ONLINE_E_BAD);
   for (const char *p = host; *p; p++)
      if (!(isalnum((unsigned char)*p) || *p == '-' || *p == '.'))
         return Local(ONLINE_OP_RESOLVE, ONLINE_E_BAD);
   if (sscanf(host, "%u.%u.%u.%u%c", &a, &b, &c, &d, &tail) == 4 && a < 256 && b < 256 && c < 256 && d < 256)
   {   /* an IPv4 literal needs no DNS */
      if (Local(ONLINE_OP_RESOLVE, ONLINE_E_NONE) != 0)
         return -1;
      snprintf(g_slot[i].local.resolved, sizeof g_slot[i].local.resolved, "%u.%u.%u.%u", a, b, c, d);
      return 0;
   }
   ResolveJob *job = new ResolveJob();
   memset(job, 0, sizeof *job);
   strcpy(job->host, host);
   __atomic_store_n(&job->refs, 2, __ATOMIC_RELEASE);
   nph::Thread *th = nph::ThreadStart(ResolveMain, job);
   if (!th)
   {
      delete job;
      return Local(ONLINE_OP_RESOLVE, ONLINE_E_NET);
   }
   Slot &s = g_slot[i];
   memset(&s, 0, sizeof s);
   s.kind = K_RESOLVE;
   s.op = ONLINE_OP_RESOLVE;
   s.job = job;
   s.th = th;
   return 0;
}

int online_http_busy(int op)
{
   for (int i = 0; i < kSlots; i++)
      if (g_slot[i].kind != K_NONE && (op == ONLINE_OP_NONE || g_slot[i].op == op))
         return 1;
   return 0;
}

int online_http_take(online_reply *out)
{
   if (!out)
      return 0;
   LogDrain();
   ReapZombies();
   for (int i = 0; i < kSlots; i++)
   {
      Slot &s = g_slot[i];
      if (s.kind == K_NONE)
         continue;
      if (s.kind == K_LOCAL)
      {
         *out = s.local;
         memset(&s, 0, sizeof s);
         return 1;
      }
      if (s.kind == K_RESOLVE)
      {
         if (!__atomic_load_n(&s.job->done, __ATOMIC_ACQUIRE))
            continue;
         memset(out, 0, sizeof *out);
         out->op = ONLINE_OP_RESOLVE;
         if (s.job->rc == 0)
            snprintf(out->resolved, sizeof out->resolved, "%u.%u.%u.%u", (s.job->ip >> 24) & 255, (s.job->ip >> 16) & 255,
                     (s.job->ip >> 8) & 255, s.job->ip & 255);
         else
            out->err = ONLINE_E_NET;
         nph::ThreadJoin(s.th, 200);
         ReleaseResolveJob(s.job);
         memset(&s, 0, sizeof s);
         return 1;
      }
      NpHttpsResult res;
      const int st = Poll(s, &res);
      if (st == 0)
         continue;
      memset(out, 0, sizeof *out);
      out->op = s.op;
      if (st < 0)
         out->err = ONLINE_E_NET;   // the client lost the handle (it was shut down under us)
      else
      {
         FillReply(s.op, res, out);
         char line[120];
         snprintf(line, sizeof line, "[room] op=%d http=%d client=%s -> err=%d %ums", s.op, res.http_status,
                  np_https_error_name(res.error), out->err, (unsigned)res.elapsed_ms);
         LogPush(line);
         Release(s);
      }
      memset(&s, 0, sizeof s);
      LogDrain();
      return 1;
   }
   return 0;
}

void online_http_drop(void)
{
   ReapZombies();   // make room first
   for (int i = 0; i < kSlots; i++)
   {
      Slot &s = g_slot[i];
      if (s.kind == K_TLS || s.kind == K_PLAIN)
         Release(s);   // cancels a running request, frees an answer
      else if (s.kind == K_RESOLVE)
      {
         // Never wait here: this runs on the frame thread.  The resolve thread is told to stop and handed to the reaper, which
         // drops the main thread's ref once joined; if the table is full or ThreadJoin times out at shutdown, the worker drops the
         // last ref when nph::Resolve exits so ResolveJob is neither leaked nor freed under a live worker.
         __atomic_store_n(&s.job->cancel, 1, __ATOMIC_RELEASE);
         if (!__atomic_load_n(&s.job->done, __ATOMIC_ACQUIRE))
            nph::ResolveAbortFor(&s.job->cancel);
         bool queued = false;
         for (int z = 0; z < kZombies; z++)
            if (!g_zombie[z].job) { g_zombie[z].job = s.job; g_zombie[z].th = s.th; queued = true; break; }
         if (!queued)
            ReleaseResolveJob(s.job);
      }
      memset(&s, 0, sizeof s);
   }
   ReapZombies();
}

void online_http_shutdown(void)
{
   online_http_drop();
   np_https_shutdown(2000);
#if !defined(NP_TARGET_VITA)
   np_http_shutdown(2000);
#endif
   for (int z = 0; z < kZombies; z++)
      if (g_zombie[z].job)
      {
         nph::ThreadJoin(g_zombie[z].th, kResolveMs + 500);
         ReleaseResolveJob(g_zombie[z].job);
         g_zombie[z].job = nullptr;
         g_zombie[z].th = nullptr;
      }
   LogDrain();
}

/* ---- pure helpers ------------------------------------------------------------------------------------------------------------ */

int online_json_get(const char *body, const char *path, char *out, size_t cap)
{
   const char *v, *e;
   if (out && cap)
      out[0] = 0;
   if (!out || !cap || !Find(body, path, &v, &e))
      return 0;
   if (*v == '"')
   {
      v++;
      e--;   /* the closing quote */
   }
   if ((size_t)(e - v) >= cap)
      return 0;   /* never hand out a truncated token */
   memcpy(out, v, (size_t)(e - v));
   out[e - v] = 0;
   return 1;
}

int online_json_int(const char *body, const char *path, int def)
{
   char tmp[24];
   char *end;
   long v;
   if (!online_json_get(body, path, tmp, sizeof tmp))
      return def;
   if (!strcmp(tmp, "true"))
      return 1;
   if (!strcmp(tmp, "false"))
      return 0;
   v = strtol(tmp, &end, 10);
   if (end == tmp || *end)
      return def;
   return (int)v;
}

size_t online_json_desc(char *out, size_t cap, const online_desc *d, const char *extra_members)
{
   size_t n = 0;
   char hex[16];
   int w;
   if (!out || !cap)
      return 0;
   out[0] = 0;
   if (!d)
      return 0;
   w = snprintf(out, cap, "{\"kind\":%d", d->kind);
   if (w < 0 || (size_t)w >= cap)
      return 0;
   n = (size_t)w;
   snprintf(hex, sizeof hex, "%08x", (unsigned)d->compat);
   if (!PutStr(out, cap, &n, "console", d->console, true) || !PutStr(out, cap, &n, "game", d->game, true) ||
       !PutStr(out, cap, &n, "rom", d->rom, true) || !PutStr(out, cap, &n, "title", d->title, true) ||
       !PutStr(out, cap, &n, "nick", d->nick, true) || !PutStr(out, cap, &n, "ver", VersionOk(d->ver) ? d->ver : g_ver, true) ||
       !PutStr(out, cap, &n, "compat", hex, true) || !PutStr(out, cap, &n, "pub", d->pub, true) ||
       !PutStr(out, cap, &n, "lan", d->lan, true) || !PutInt(out, cap, &n, "sym", d->sym ? 1 : 0) ||
       !PutInt(out, cap, &n, "relayOnly", d->relay_only ? 1 : 0))
   {
      out[0] = 0;
      return 0;
   }
   if (extra_members && *extra_members)
   {
      w = snprintf(out + n, cap - n, ",%s", extra_members);
      if (w < 0 || (size_t)w >= cap - n)
      {
         out[0] = 0;
         return 0;
      }
      n += (size_t)w;
   }
   if (n + 2 > cap)
   {
      out[0] = 0;
      return 0;
   }
   out[n++] = '}';
   out[n] = 0;
   return n;
}

}   // extern "C"
