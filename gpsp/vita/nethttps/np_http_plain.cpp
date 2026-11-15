// np_http_plain.cpp -- plain http:// for the twin and the loopback tests (see np_http.h).  One short-lived thread per request: resolve,
// connect, send, read the answer (Content-Length, chunked or until the close), each step under a time limit, the whole under a total
// limit.  It reuses the client's own platform layer (np_https_sys: sockets, resolver, threads, clock), so a plain request and a TLS
// request behave alike for the caller.  Not built for the Vita.
#include "np_http.h"

#if !defined(NP_TARGET_VITA)

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "np_https_sys.h"

using namespace nph;

namespace {

enum { SS_FREE = 0, SS_RUN, SS_DONE, SS_ZOMBIE };   // ZOMBIE: released while its thread still runs; reaped later

struct Slot {
    int state;
    uint32_t gen;
    Thread* th;
    volatile int cancel;
    char host[256], path[NP_HTTPS_MAX_URL + 1], method[8], ctype[64], headers[NP_HTTPS_MAX_HEADER_BYTES + 4];
    uint16_t port;
    uint8_t* body;
    size_t body_len;
    uint32_t t_connect, t_io, t_total, max_response;
    int64_t t_start;
    volatile int stage;
    uint8_t* resp;
    size_t resp_len, resp_cap;
    NpHttpsResult res;
};

Slot g_slots[NP_HTTPS_SLOTS];
SpinLock g_lock;
uint32_t g_gen;
const uint8_t kEmpty[1] = {0};

NpHttpsHandle MakeHandle(uint32_t gen, int index) { return (gen << 8) | (uint32_t)(index + 1); }
Slot* SlotOf(NpHttpsHandle h) {   // lock held
    const int i = (int)(h & 0xFF) - 1;
    if (i < 0 || i >= NP_HTTPS_SLOTS) return nullptr;
    Slot* s = &g_slots[i];
    if (s->state == SS_FREE || s->state == SS_ZOMBIE || (s->gen & 0xFFFFFFu) != (h >> 8)) return nullptr;
    return s;
}

void FreeMem(Slot* s) {
    free(s->body);
    free(s->resp);
    s->body = nullptr;
    s->resp = nullptr;
    s->resp_len = s->resp_cap = 0;
}

void Reap() {   // lock NOT held
    for (int i = 0; i < NP_HTTPS_SLOTS; i++) {
        Thread* t = nullptr;
        g_lock.lock();
        Slot* s = &g_slots[i];
        if (s->state == SS_ZOMBIE || (s->state == SS_DONE && s->th)) { t = s->th; }
        g_lock.unlock();
        if (!t) continue;
        if (!ThreadJoin(t, 1)) continue;
        g_lock.lock();
        s = &g_slots[i];
        if (s->th == t) {
            s->th = nullptr;
            if (s->state == SS_ZOMBIE) { FreeMem(s); s->state = SS_FREE; }
        }
        g_lock.unlock();
    }
}

struct Ctx {
    Slot* s;
    int64_t total_end;
};

int Fail(Ctx& c, NpHttpsError e, const char* msg, int sys = 0) {
    Slot* s = c.s;
    s->res.error = e;
    s->res.sys_code = sys;
    snprintf(s->res.message, sizeof s->res.message, "%s", msg);
    return -1;
}

// Waits for the socket in slices so a cancel is noticed within ~50 ms; returns 1 ready, 0 time-out of `limit_end`, -1 failed / cancelled.
int WaitSock(Ctx& c, Sock* sk, bool wr, int64_t limit_end) {
    for (;;) {
        if (c.s->cancel) return -1;
        const int64_t now = MonoMs();
        const int64_t end = limit_end < c.total_end ? limit_end : c.total_end;
        if (now >= end) return 0;
        int64_t slice = end - now;
        if (slice > 50) slice = 50;
        const int r = SockWait(sk, wr, (uint32_t)slice);
        if (r != 0) return r;
    }
}

bool NameIs(const char* p, size_t n, const char* name) { return strlen(name) == n && strncasecmp(p, name, n) == 0; }

int Append(Ctx& c, const uint8_t* p, size_t n) {
    Slot* s = c.s;
    if (s->resp_len + n > s->max_response) return Fail(c, NP_HTTPS_E_TOO_LARGE, "the answer is too large");
    if (s->resp_len + n + 1 > s->resp_cap) {
        size_t cap = s->resp_cap ? s->resp_cap : 1024;
        while (cap < s->resp_len + n + 1) cap *= 2;
        uint8_t* b = (uint8_t*)realloc(s->resp, cap);
        if (!b) return Fail(c, NP_HTTPS_E_MEMORY, "out of memory");
        s->resp = b;
        s->resp_cap = cap;
    }
    memcpy(s->resp + s->resp_len, p, n);
    s->resp_len += n;
    s->resp[s->resp_len] = 0;
    return 0;
}

// The whole request, on the slot's thread.
void Run(Slot* s) {
    Ctx c = {s, 0};
    Sock sk;
    SockInit(&sk);
    s->t_start = MonoMs();
    c.total_end = s->t_start + s->t_total;
    s->res.error = NP_HTTPS_OK;
    s->stage = NP_HTTPS_S_DNS;
    uint32_t ip = 0;
    int sys = 0;
    const int64_t conn_end = s->t_start + s->t_connect;
    int rc = Resolve(s->host, &ip, s->t_connect, &s->cancel, &sys);
    if (rc != 0) {
        if (rc == NET_CANCELLED) Fail(c, NP_HTTPS_E_CANCELLED, "cancelled");
        else if (rc == NET_TIMEOUT) Fail(c, NP_HTTPS_E_TIMEOUT, "name lookup timed out", sys);
        else Fail(c, NP_HTTPS_E_DNS, "the name did not resolve", sys);
        goto done;
    }
    s->stage = NP_HTTPS_S_CONNECT;
    {
        int64_t left = conn_end - MonoMs();
        if (left < 1) left = 1;
        rc = SockConnect(&sk, ip, s->port, (uint32_t)left, &s->cancel, &sys);
        if (rc == NET_CANCELLED) { Fail(c, NP_HTTPS_E_CANCELLED, "cancelled"); goto done; }
        if (rc == NET_TIMEOUT) { Fail(c, NP_HTTPS_E_TIMEOUT, "connect timed out", sys); goto done; }
        if (rc != 0) { Fail(c, NP_HTTPS_E_CONNECT, "could not connect", sys); goto done; }
    }
    s->stage = NP_HTTPS_S_SEND;
    {
        char head[NP_HTTPS_MAX_URL + NP_HTTPS_MAX_HEADER_BYTES + 512];
        const bool has_body = s->body_len > 0 || strcmp(s->method, "POST") == 0;
        int n = snprintf(head, sizeof head, "%s %s HTTP/1.1\r\nHost: %s:%u\r\nUser-Agent: %s\r\nAccept: application/json, */*\r\n"
                                            "Accept-Encoding: identity\r\nConnection: close\r\n",
                         s->method, s->path, s->host, (unsigned)s->port, UserAgent());
        if (has_body && n > 0 && (size_t)n < sizeof head)
            n += snprintf(head + n, sizeof head - (size_t)n, "Content-Type: %s\r\nContent-Length: %u\r\n", s->ctype, (unsigned)s->body_len);
        if (n > 0 && (size_t)n < sizeof head) n += snprintf(head + n, sizeof head - (size_t)n, "%s\r\n", s->headers);
        if (n <= 0 || (size_t)n >= sizeof head) { Fail(c, NP_HTTPS_E_BAD_REQUEST, "request head too long"); goto done; }
        // the head and the body go out together (one small segment, as the TLS client's two writes would for a tiny request)
        size_t total = (size_t)n + s->body_len;
        uint8_t* out = (uint8_t*)malloc(total);
        if (!out) { Fail(c, NP_HTTPS_E_MEMORY, "out of memory"); goto done; }
        memcpy(out, head, (size_t)n);
        if (s->body_len) memcpy(out + n, s->body, s->body_len);
        size_t sent = 0;
        while (sent < total) {
            const int w = SockSend(&sk, out + sent, total - sent, &sys);
            if (w > 0) { sent += (size_t)w; continue; }
            if (w < 0) { free(out); Fail(c, NP_HTTPS_E_IO, "send failed", sys); goto done; }
            const int r = WaitSock(c, &sk, true, MonoMs() + s->t_io);
            if (r <= 0) { free(out); if (s->cancel) Fail(c, NP_HTTPS_E_CANCELLED, "cancelled"); else Fail(c, NP_HTTPS_E_TIMEOUT, "send timed out"); goto done; }
        }
        free(out);
    }
    s->stage = NP_HTTPS_S_RESPONSE;
    {
        // read until the blank line, then the body by its framing
        uint8_t buf[2048];
        char* hbuf = (char*)malloc(NP_HTTPS_MAX_URL + 4096);
        if (!hbuf) { Fail(c, NP_HTTPS_E_MEMORY, "out of memory"); goto done; }
        const size_t hcap = NP_HTTPS_MAX_URL + 4096;
        size_t hlen = 0;
        bool eof = false, have_head = false;
        long clen = -1;
        bool chunked = false;
        size_t body_start = 0;
        // chunk decoder state
        int cstate = 0;            // 0 size line, 1 data, 2 CRLF after data, 3 trailers/finished
        size_t cleft = 0;
        char cline[24];
        size_t cl = 0;
        bool cext = false;         // inside a chunk extension (after ';'): ignored to the end of the line
        size_t got_body = 0;
        bool body_done = false;
        auto Body = [&](const uint8_t* p, size_t n) -> int {   // framing of the body bytes
            if (!chunked) {
                size_t take = n;
                if (clen >= 0 && got_body + take > (size_t)clen) take = (size_t)clen - got_body;
                if (take && Append(c, p, take) != 0) return -1;
                got_body += take;
                if (clen >= 0 && got_body >= (size_t)clen) body_done = true;
                return 0;
            }
            size_t i = 0;
            while (i < n && !body_done) {
                if (cstate == 0) {
                    const char ch = (char)p[i++];
                    if (ch == '\n') {
                        cline[cl] = 0;
                        cl = 0;
                        cext = false;
                        char* e = nullptr;
                        const unsigned long v = strtoul(cline, &e, 16);
                        if (e == cline) return Fail(c, NP_HTTPS_E_PROTOCOL, "bad chunk size");
                        cleft = v;
                        cstate = v == 0 ? 3 : 1;
                        if (v == 0) body_done = true;
                    } else if (ch == ';') {
                        cext = true;
                    } else if (ch != '\r' && !cext) {
                        if (cl + 1 >= sizeof cline) return Fail(c, NP_HTTPS_E_PROTOCOL, "bad chunk size");
                        cline[cl++] = ch;
                    }
                } else if (cstate == 1) {
                    size_t take = n - i < cleft ? n - i : cleft;
                    if (Append(c, p + i, take) != 0) return -1;
                    i += take;
                    cleft -= take;
                    if (cleft == 0) cstate = 2;
                } else if (cstate == 2) {
                    if (p[i++] == '\n') cstate = 0;
                }
            }
            return 0;
        };
        for (;;) {
            if (have_head && body_done) break;
            if (have_head && eof) {
                if (chunked || (clen >= 0 && got_body < (size_t)clen)) { free(hbuf); Fail(c, NP_HTTPS_E_IO, "the connection closed early"); goto done; }
                break;
            }
            if (eof) { free(hbuf); Fail(c, NP_HTTPS_E_IO, "the connection closed before the answer"); goto done; }
            const int w = WaitSock(c, &sk, false, MonoMs() + s->t_io);
            if (w < 0) { free(hbuf); if (s->cancel) Fail(c, NP_HTTPS_E_CANCELLED, "cancelled"); else Fail(c, NP_HTTPS_E_IO, "receive failed"); goto done; }
            if (w == 0) { free(hbuf); Fail(c, NP_HTTPS_E_TIMEOUT, "the server did not answer in time"); goto done; }
            const int r = SockRecv(&sk, buf, sizeof buf, &sys);
            if (r == 0) continue;
            if (r == -2) { eof = true; continue; }
            if (r < 0) { free(hbuf); Fail(c, NP_HTTPS_E_IO, "receive failed", sys); goto done; }
            if (have_head) {
                if (Body(buf, (size_t)r) != 0) { free(hbuf); goto done; }
                continue;
            }
            if (hlen + (size_t)r > hcap - 1) { free(hbuf); Fail(c, NP_HTTPS_E_PROTOCOL, "answer head too long"); goto done; }
            memcpy(hbuf + hlen, buf, (size_t)r);
            hlen += (size_t)r;
            hbuf[hlen] = 0;
            char* end = strstr(hbuf, "\r\n\r\n");
            if (!end) continue;
            *end = 0;
            body_start = (size_t)(end - hbuf) + 4;
            // status line
            if (strncmp(hbuf, "HTTP/1.", 7) != 0 || (hbuf[7] != '0' && hbuf[7] != '1')) { free(hbuf); Fail(c, NP_HTTPS_E_PROTOCOL, "not HTTP/1.x"); goto done; }
            s->res.http_status = atoi(hbuf + 9);
            for (char* line = strstr(hbuf, "\r\n"); line;) {
                line += 2;
                char* nx = strstr(line, "\r\n");
                const size_t ll = nx ? (size_t)(nx - line) : strlen(line);
                const char* colon = (const char*)memchr(line, ':', ll);
                if (colon) {
                    const char* v = colon + 1;
                    while (v < line + ll && *v == ' ') v++;
                    if (NameIs(line, (size_t)(colon - line), "Content-Length")) clen = atol(v);
                    else if (NameIs(line, (size_t)(colon - line), "Transfer-Encoding") && strncasecmp(v, "chunked", 7) == 0) chunked = true;
                    else if (NameIs(line, (size_t)(colon - line), "Content-Encoding") && strncasecmp(v, "identity", 8) != 0) { free(hbuf); Fail(c, NP_HTTPS_E_PROTOCOL, "a compressed answer"); goto done; }
                }
                line = nx;
            }
            have_head = true;
            if (s->res.http_status >= 100 && s->res.http_status < 200) { free(hbuf); Fail(c, NP_HTTPS_E_PROTOCOL, "unexpected interim answer"); goto done; }
            if (s->res.http_status == 204 || s->res.http_status == 304) clen = 0;
            if (clen == 0) body_done = true;
            if (clen > 0 && (size_t)clen > s->max_response) { free(hbuf); Fail(c, NP_HTTPS_E_TOO_LARGE, "the answer is too large"); goto done; }
            // what followed the head in the same read
            if (hlen > body_start && Body((const uint8_t*)hbuf + body_start, hlen - body_start) != 0) { free(hbuf); goto done; }
        }
        free(hbuf);
        if (s->res.http_status >= 300 && s->res.http_status < 400) Fail(c, NP_HTTPS_E_REDIRECT, "a redirect (never followed)");
        else if (s->res.http_status < 200 || s->res.http_status >= 300) Fail(c, NP_HTTPS_E_HTTP_STATUS, "the server answered with an error status");
    }
done:
    SockClose(&sk);
    {
        const NpHttpsStage last = (NpHttpsStage)s->stage;
        s->res.stage = s->res.error == NP_HTTPS_OK ? NP_HTTPS_S_DONE : last;
    }
    s->stage = NP_HTTPS_S_DONE;
    s->res.elapsed_ms = (uint32_t)(MonoMs() - s->t_start);
    s->res.bytes_received = s->resp_len;
    if (s->res.error != NP_HTTPS_OK && s->res.error != NP_HTTPS_E_HTTP_STATUS) {   // as the TLS client: only a status answer keeps its body
        free(s->resp);
        s->resp = nullptr;
        s->resp_len = s->resp_cap = 0;
        s->res.bytes_received = 0;
    }
}

void ThreadMain(void* arg) {
    Slot* s = (Slot*)arg;
    Run(s);
    g_lock.lock();
    if (s->state == SS_RUN) s->state = SS_DONE;
    g_lock.unlock();
}

bool ParseUrl(const char* url, Slot* s) {
    if (!url || strlen(url) > NP_HTTPS_MAX_URL || strncasecmp(url, "http://", 7) != 0) return false;
    for (const char* p = url; *p; p++)
        if ((unsigned char)*p <= 0x20 || (unsigned char)*p >= 0x7F || *p == '#') return false;
    const char* a = url + 7;
    const char* end = a;
    while (*end && *end != '/' && *end != '?') end++;
    const char* colon = nullptr;
    for (const char* p = a; p < end; p++) {
        if (*p == '@' || *p == '[' || *p == ']') return false;
        if (*p == ':') colon = p;
    }
    const char* hostEnd = colon ? colon : end;
    const size_t hl = (size_t)(hostEnd - a);
    if (hl == 0 || hl > 253) return false;
    for (const char* p = a; p < hostEnd; p++)
        if (!(isalnum((unsigned char)*p) || *p == '-' || *p == '.')) return false;
    memcpy(s->host, a, hl);
    s->host[hl] = 0;
    s->port = 80;
    if (colon) {
        const char* q = colon + 1;
        if (q == end || end - q > 5) return false;
        uint32_t port = 0;
        for (; q < end; q++) {
            if (!isdigit((unsigned char)*q)) return false;
            port = port * 10 + (uint32_t)(*q - '0');
        }
        if (port == 0 || port > 65535) return false;
        s->port = (uint16_t)port;
    }
    if (*end == 0) strcpy(s->path, "/");
    else if (*end == '?') { s->path[0] = '/'; strcpy(s->path + 1, end); }
    else strcpy(s->path, end);
    return true;
}

}  // namespace

extern "C" {

NpHttpsHandle np_http_start(const NpHttpsRequest* rq, NpHttpsError* err) {
    NpHttpsError dummy;
    if (!err) err = &dummy;
    *err = NP_HTTPS_OK;
    if (!rq || !rq->url) { *err = NP_HTTPS_E_BAD_REQUEST; return 0; }
    Reap();
    Slot* t = (Slot*)calloc(1, sizeof(Slot));
    if (!t) { *err = NP_HTTPS_E_MEMORY; return 0; }
    const char* method = rq->method ? rq->method : (rq->body_len ? "POST" : "GET");
    bool ok = ParseUrl(rq->url, t);
    if (strcmp(method, "GET") == 0) ok = ok && rq->body_len == 0;
    else if (strcmp(method, "POST") != 0) ok = false;
    if (rq->body_len > NP_HTTPS_MAX_REQUEST_BODY || (rq->body_len && !rq->body)) ok = false;
    const char* ctype = rq->content_type ? rq->content_type : "application/json";
    if (ok && strlen(ctype) >= sizeof t->ctype) ok = false;
    if (ok && rq->headers) {   // the plain client takes the same header lines (the room client sends none)
        size_t hl = 0;
        int count = 0;
        for (const char* const* h = rq->headers; *h && ok; h++) {
            const size_t l = strlen(*h);
            if (++count > NP_HTTPS_MAX_HEADERS || strpbrk(*h, "\r\n") || hl + l + 2 > NP_HTTPS_MAX_HEADER_BYTES) { ok = false; break; }
            memcpy(t->headers + hl, *h, l);
            hl += l;
            t->headers[hl++] = '\r';
            t->headers[hl++] = '\n';
        }
    }
    if (!ok) { free(t); *err = NP_HTTPS_E_BAD_REQUEST; return 0; }
    snprintf(t->method, sizeof t->method, "%s", method);
    strcpy(t->ctype, ctype);
    uint8_t* body = nullptr;
    if (rq->body_len) {
        body = (uint8_t*)malloc(rq->body_len);
        if (!body) { free(t); *err = NP_HTTPS_E_MEMORY; return 0; }
        memcpy(body, rq->body, rq->body_len);
    }
    t->t_connect = rq->connect_timeout_ms ? rq->connect_timeout_ms : 10000;
    t->t_io = rq->io_timeout_ms ? rq->io_timeout_ms : 10000;
    t->t_total = rq->total_timeout_ms ? rq->total_timeout_ms : 30000;
    if (t->t_total > 120000) t->t_total = 120000;
    if (t->t_connect > t->t_total) t->t_connect = t->t_total;
    t->max_response = rq->max_response ? rq->max_response : NP_HTTPS_DEFAULT_MAX_RESPONSE;
    if (t->max_response > NP_HTTPS_MAX_RESPONSE_LIMIT) t->max_response = NP_HTTPS_MAX_RESPONSE_LIMIT;

    g_lock.lock();
    int index = -1;
    for (int i = 0; i < NP_HTTPS_SLOTS; i++)
        if (g_slots[i].state == SS_FREE) { index = i; break; }
    if (index < 0) { g_lock.unlock(); free(body); free(t); *err = NP_HTTPS_E_BUSY; return 0; }
    Slot* s = &g_slots[index];
    uint32_t gen = ++g_gen & 0xFFFFFFu;
    if (gen == 0) gen = ++g_gen & 0xFFFFFFu;
    *s = *t;
    s->gen = gen;
    s->body = body;
    s->body_len = rq->body_len;
    s->state = SS_RUN;
    s->stage = NP_HTTPS_S_QUEUED;
    g_lock.unlock();
    free(t);
    s->th = ThreadStart(ThreadMain, s);
    if (!s->th) {
        g_lock.lock();
        FreeMem(s);
        s->state = SS_FREE;
        g_lock.unlock();
        *err = NP_HTTPS_E_MEMORY;
        return 0;
    }
    return MakeHandle(gen, index);
}

int np_http_poll(NpHttpsHandle h, NpHttpsResult* out) {
    Reap();
    g_lock.lock();
    Slot* s = SlotOf(h);
    if (!s) { g_lock.unlock(); return -1; }
    if (s->state != SS_DONE) {
        if (out) {
            memset(out, 0, sizeof *out);
            out->stage = (NpHttpsStage)s->stage;
            out->body = kEmpty;
            out->elapsed_ms = s->t_start ? (uint32_t)(MonoMs() - s->t_start) : 0;
        }
        g_lock.unlock();
        return 0;
    }
    if (out) {
        *out = s->res;
        out->body = s->resp ? s->resp : kEmpty;
        out->body_len = s->resp ? s->resp_len : 0;
    }
    g_lock.unlock();
    return 1;
}

void np_http_cancel(NpHttpsHandle h) {
    g_lock.lock();
    Slot* s = SlotOf(h);
    if (s && s->state == SS_RUN) s->cancel = 1;
    g_lock.unlock();
}

void np_http_release(NpHttpsHandle h) {
    g_lock.lock();
    Slot* s = SlotOf(h);
    if (s) {
        if (s->state == SS_RUN) { s->cancel = 1; s->state = SS_ZOMBIE; }   // its thread ends within ~50 ms; Reap frees the slot
        else { FreeMem(s); s->state = s->th ? SS_ZOMBIE : SS_FREE; }   // (a finished thread not yet joined: Reap frees the slot)
    }
    g_lock.unlock();
    Reap();
}

void np_http_shutdown(uint32_t timeout_ms) {
    g_lock.lock();
    for (int i = 0; i < NP_HTTPS_SLOTS; i++)
        if (g_slots[i].state == SS_RUN) g_slots[i].cancel = 1;
    g_lock.unlock();
    const int64_t until = MonoMs() + timeout_ms;
    for (;;) {
        Reap();
        bool any = false;
        g_lock.lock();
        for (int i = 0; i < NP_HTTPS_SLOTS; i++) {
            Slot* s = &g_slots[i];
            if (s->state == SS_RUN || s->th) any = true;
            else if (s->state != SS_FREE) { FreeMem(s); s->state = SS_FREE; }
        }
        g_lock.unlock();
        if (!any || MonoMs() >= until) break;
        SleepMs(5);
    }
    g_lock.lock();
    for (int i = 0; i < NP_HTTPS_SLOTS; i++)
        if (g_slots[i].state == SS_DONE && !g_slots[i].th) { FreeMem(&g_slots[i]); g_slots[i].state = SS_FREE; }
    g_lock.unlock();
}

}  // extern "C"

#endif  // !NP_TARGET_VITA
