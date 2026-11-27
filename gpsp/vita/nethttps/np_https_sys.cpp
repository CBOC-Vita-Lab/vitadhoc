// np_https_sys.cpp: sockets, name resolution, threads, clocks and random bytes for np_https, on sceNet (the Vita) and on POSIX
// (the host: unit tests and the desktop harness). See np_https_sys.h. Also the three hooks mbedtls asks its host for
// (mbedtls_hardware_poll, mbedtls_ms_time, np_https_wallclock, mbedtls_platform_gmtime_r): see dependencies/mbedtls/np_user_config.h.
#include "np_https_sys.h"
#include "np_log.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(NP_TARGET_VITA)
#include "../net_vita.h"   // vitadhoc: the app brings sceNet up (net_vita_init) and knows the Wi-Fi state
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/rng.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/net/net.h>
#include <psp2/net/netctl.h>
#include <psp2/rtc.h>
#include <psp2/sysmodule.h>
#else
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <pthread.h>
#include <sched.h>
#include <sys/random.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>
#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0   // macOS: SO_NOSIGPIPE on the socket instead (SockOpen)
#endif
#endif

#if defined(NP_TARGET_VITA)
enum NpEnumSizeCheck { NP_ESC_0 = 0 };
static_assert(sizeof(NpEnumSizeCheck) == 4, "nethttps requires -fno-short-enums");
#endif

namespace nph {

int64_t (*g_wall_override)(void) = nullptr;

static char g_userAgent[96] = "VitAdhoc";
const char* UserAgent() { return g_userAgent; }
void SetUserAgent(const char* ua) {
    // printable ASCII only (it goes into a header line); anything else and an empty text keep the old value
    if (!ua || !*ua || strlen(ua) >= sizeof g_userAgent) return;
    for (const char* p = ua; *p; p++)
        if ((unsigned char)*p < 0x20 || (unsigned char)*p > 0x7E) return;
    strcpy(g_userAgent, ua);
}

void Log(const char* format, ...) {
    char line[256];
    va_list ap;
    va_start(ap, format);
    vsnprintf(line, sizeof line, format, ap);
    va_end(ap);
    NpLogf("net-https", "%s\n", line);   // Galaxy: also the Vita log.txt (np_log.h)
}

// Days from 1970-01-01 to a civil date (proleptic Gregorian; Howard Hinnant's algorithm), and its inverse.
static int64_t DaysFromCivil(int64_t y, unsigned m, unsigned d) {
    y -= m <= 2;
    const int64_t era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = (unsigned)(y - era * 400);
    const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + (int64_t)doe - 719468;
}

void SpinLock::lock() {
    while (__atomic_exchange_n(&v, 1, __ATOMIC_ACQUIRE)) {
#if defined(NP_TARGET_VITA)
        sceKernelDelayThread(50);
#else
        sched_yield();
#endif
    }
}
void SpinLock::unlock() { __atomic_store_n(&v, 0, __ATOMIC_RELEASE); }

// =================================================================================================================================
#if defined(NP_TARGET_VITA)
// =================================================================================================================================

int64_t MonoMs() { return (int64_t)(sceKernelGetProcessTimeWide() / 1000u); }
void SleepMs(uint32_t ms) { sceKernelDelayThread(ms * 1000u); }

int64_t WallSeconds() {
    if (g_wall_override) return g_wall_override();
    SceDateTime dt;
    if (sceRtcGetCurrentClock(&dt, 0) < 0) return 0;     // UTC
    if (dt.year < 1970 || dt.month < 1 || dt.month > 12 || dt.day < 1 || dt.day > 31) return 0;
    return DaysFromCivil(dt.year, dt.month, dt.day) * 86400 + (int64_t)dt.hour * 3600 + (int64_t)dt.minute * 60 + (int64_t)dt.second;
}

struct Event { SceUID sema; };
Event* EventCreate() {
    Event* e = (Event*)malloc(sizeof *e);
    if (!e) return nullptr;
    e->sema = sceKernelCreateSema("np_https", 0, 0, 1, nullptr);
    if (e->sema < 0) { free(e); return nullptr; }
    return e;
}
void EventDestroy(Event* e) {
    if (!e) return;
    sceKernelDeleteSema(e->sema);
    free(e);
}
void EventSignal(Event* e) { sceKernelSignalSema(e->sema, 1); }
bool EventWait(Event* e, uint32_t timeout_ms) {
    SceUInt timeout = timeout_ms * 1000u;
    return sceKernelWaitSema(e->sema, 1, &timeout) >= 0;
}

struct Thread { SceUID id; void (*entry)(void*); void* arg; };
static int ThreadTrampoline(SceSize, void* argp) {
    Thread* t = *(Thread**)argp;
    t->entry(t->arg);
    return 0;
}
Thread* ThreadStart(void (*entry)(void*), void* arg) {
    Thread* t = (Thread*)malloc(sizeof *t);
    if (!t) return nullptr;
    t->entry = entry;
    t->arg = arg;
    // Core 2 (user core 2), a little below the game's priority: the same place as the radio's receive thread. A deep stack: the handshake
    // (RSA / ECDSA verification) runs here.
    t->id = sceKernelCreateThread("np_https", ThreadTrampoline, 0x10000100 + 20, 160 * 1024, 0, SCE_KERNEL_CPU_MASK_USER_2, nullptr);
    if (t->id < 0) { free(t); return nullptr; }
    if (sceKernelStartThread(t->id, sizeof t, &t) < 0) { sceKernelDeleteThread(t->id); free(t); return nullptr; }
    return t;
}
bool ThreadJoin(Thread* t, uint32_t timeout_ms) {
    SceUInt timeout = timeout_ms * 1000u;
    int status = 0;
    if (sceKernelWaitThreadEnd(t->id, &status, &timeout) < 0) return false;
    sceKernelDeleteThread(t->id);
    free(t);
    return true;
}

// vitadhoc: the app owns sceNet (net_vita.c: one pool, brought up once, taken down at exit after every release).  We only ask it to be
// up (net_vita_init is idempotent and tolerates a pool somebody else made) and what the Wi-Fi says.  The lock covers the rare first call
// from a worker thread racing the frame thread's own first call.
static bool g_netUp;
static SpinLock g_netLock;
static bool EnsureNet() {
    if (__atomic_load_n(&g_netUp, __ATOMIC_ACQUIRE)) return true;
    g_netLock.lock();
    bool ok = __atomic_load_n(&g_netUp, __ATOMIC_RELAXED);
    if (!ok) {
        ok = net_vita_init() >= 0;
        __atomic_store_n(&g_netUp, ok, __ATOMIC_RELEASE);
    }
    g_netLock.unlock();
    return ok;
}

bool NetworkUp() {
    if (!EnsureNet()) return false;
    net_vita_wifi w;
    return net_vita_wifi_status(&w) != 0;
}

// One entry per Resolve in progress (the worker's and any caller's resolve thread run side by side), keyed by the caller's cancel flag.  An
// abort finds its own resolver by that key and cannot hit one that has been destroyed: the entry is removed under the same lock before the
// resolver is.  A full table only means that resolve cannot be aborted early (its time-out still ends it).
struct ResolverEnt { const volatile int* cancel; int rid; };
static ResolverEnt g_resolvers[8];
static SpinLock g_resolverLock;

static void AbortResolvers(const volatile int* only) {
    g_resolverLock.lock();
    for (int i = 0; i < 8; i++)
        if (g_resolvers[i].cancel && (!only || g_resolvers[i].cancel == only)) sceNetResolverAbort(g_resolvers[i].rid, 0);
    g_resolverLock.unlock();
}
void ResolveAbort() { AbortResolvers(nullptr); }
void ResolveAbortFor(const volatile int* cancel) { if (cancel) AbortResolvers(cancel); }

int Resolve(const char* host, uint32_t* ip, uint32_t timeout_ms, const volatile int* cancel, int* sys) {
    if (!EnsureNet()) { *sys = -1; return NET_FAIL; }
    if (*cancel) return NET_CANCELLED;
    const int rid = sceNetResolverCreate("np_https", nullptr, 0);
    if (rid < 0) { *sys = rid; return NET_FAIL; }
    int slot = -1;
    g_resolverLock.lock();
    for (int i = 0; i < 8; i++)
        if (!g_resolvers[i].cancel) { g_resolvers[i].cancel = cancel; g_resolvers[i].rid = rid; slot = i; break; }
    g_resolverLock.unlock();
    // a cancel that came before the entry existed found nothing to abort: look again now that it can
    SceNetInAddr addr;
    memset(&addr, 0, sizeof addr);
    int rc;
    if (*cancel) rc = -1;
    else rc = sceNetResolverStartNtoa(rid, host, &addr, (int)(timeout_ms * 1000u), 1, 0);   // microseconds; one retry of the name servers
    if (slot >= 0) {
        g_resolverLock.lock();
        g_resolvers[slot].cancel = nullptr;
        g_resolverLock.unlock();
    }
    sceNetResolverDestroy(rid);
    if (*cancel) return NET_CANCELLED;
    if (rc < 0) {
        *sys = rc;
        return (uint32_t)rc == (uint32_t)SCE_NET_ERROR_RESOLVER_ETIMEDOUT ? NET_TIMEOUT : NET_FAIL;
    }
    *ip = __builtin_bswap32(addr.s_addr);
    return 0;
}

void SockInit(Sock* s) { s->fd = -1; s->ep = -1; }

static int EpollWait(Sock* s, uint32_t events, uint32_t ms) {
    SceNetEpollEvent out[1];
    memset(out, 0, sizeof out);
    const int n = sceNetEpollWait(s->ep, out, 1, (int)(ms * 1000u));   // microseconds
    if (n < 0) return -1;
    if (n == 0) return 0;
    if (out[0].events & (SCE_NET_EPOLLERR | SCE_NET_EPOLLHUP)) return 1;   // let the next send/recv report it
    return (out[0].events & events) ? 1 : 0;
}

int SockWait(Sock* s, bool for_write, uint32_t ms) {
    // Level-triggered on both directions would report "writable" at once for a read wait: ask for exactly one by re-arming.
    if (s->ep >= 0) {
        SceNetEpollEvent ev;
        memset(&ev, 0, sizeof ev);
        ev.events = (for_write ? SCE_NET_EPOLLOUT : SCE_NET_EPOLLIN) | SCE_NET_EPOLLERR | SCE_NET_EPOLLHUP;
        ev.data.fd = s->fd;
        sceNetEpollControl(s->ep, SCE_NET_EPOLL_CTL_MOD, s->fd, &ev);
    } else {
        // first use: create + add with the wanted direction
        s->ep = sceNetEpollCreate("np_https", 0);
        if (s->ep < 0) { s->ep = -1; return -1; }
        SceNetEpollEvent ev;
        memset(&ev, 0, sizeof ev);
        ev.events = (for_write ? SCE_NET_EPOLLOUT : SCE_NET_EPOLLIN) | SCE_NET_EPOLLERR | SCE_NET_EPOLLHUP;
        ev.data.fd = s->fd;
        if (sceNetEpollControl(s->ep, SCE_NET_EPOLL_CTL_ADD, s->fd, &ev) < 0) {
            sceNetEpollDestroy(s->ep);
            s->ep = -1;
            return -1;
        }
    }
    return EpollWait(s, for_write ? SCE_NET_EPOLLOUT : SCE_NET_EPOLLIN, ms);
}

int SockConnect(Sock* s, uint32_t ip, uint16_t port, uint32_t timeout_ms, const volatile int* cancel, int* sys) {
    if (!EnsureNet()) { *sys = -1; return NET_FAIL; }
    s->fd = sceNetSocket("np_https", SCE_NET_AF_INET, SCE_NET_SOCK_STREAM, 0);
    if (s->fd < 0) { *sys = s->fd; s->fd = -1; return NET_FAIL; }
    int one = 1;
    // Galaxy: checked and logged; a socket that cannot be non-blocking is not used (the request thread would block on it)
    if (const int rc = sceNetSetsockopt(s->fd, SCE_NET_SOL_SOCKET, SCE_NET_SO_NBIO, &one, sizeof one); rc < 0) {
        nph::Log("SO_NBIO refused 0x%08x", (unsigned)rc);
        sceNetSocketClose(s->fd);
        s->fd = -1;
        *sys = rc;
        return NET_FAIL;
    }
    if (const int rc = sceNetSetsockopt(s->fd, SCE_NET_IPPROTO_TCP, SCE_NET_TCP_NODELAY, &one, sizeof one); rc < 0)
        nph::Log("TCP_NODELAY refused 0x%08x", (unsigned)rc);
    SceNetSockaddrIn sa;
    memset(&sa, 0, sizeof sa);
    sa.sin_len = sizeof sa;
    sa.sin_family = SCE_NET_AF_INET;
    sa.sin_port = sceNetHtons(port);
    sa.sin_addr.s_addr = __builtin_bswap32(ip);
    int rc = sceNetConnect(s->fd, (SceNetSockaddr*)&sa, sizeof sa);
    if (rc >= 0) return 0;
    if ((uint32_t)rc != (uint32_t)SCE_NET_ERROR_EINPROGRESS && (uint32_t)rc != (uint32_t)SCE_NET_ERROR_EAGAIN) { *sys = rc; return NET_FAIL; }
    int64_t left = timeout_ms;
    while (left > 0) {
        if (*cancel) return NET_CANCELLED;
        const uint32_t slice = left > 50 ? 50 : (uint32_t)left;
        const int w = SockWait(s, true, slice);
        if (w < 0) { *sys = -2; return NET_FAIL; }
        if (w > 0) {
            int err = 0;
            unsigned len = sizeof err;
            if (sceNetGetsockopt(s->fd, SCE_NET_SOL_SOCKET, SCE_NET_SO_ERROR, &err, &len) < 0) { *sys = -3; return NET_FAIL; }
            if (err != 0) { *sys = err; return (uint32_t)err == (uint32_t)SCE_NET_ERROR_ETIMEDOUT ? NET_TIMEOUT : NET_FAIL; }
            return 0;
        }
        left -= slice;
    }
    return NET_TIMEOUT;
}

int SockSend(Sock* s, const void* buf, size_t len, int* sys) {
    const int n = sceNetSend(s->fd, buf, (unsigned)len, SCE_NET_MSG_DONTWAIT);
    if (n >= 0) return n == 0 ? -1 : n;
    if ((uint32_t)n == (uint32_t)SCE_NET_ERROR_EAGAIN) return 0;
    *sys = n;
    return -1;
}
int SockRecv(Sock* s, void* buf, size_t len, int* sys) {
    const int n = sceNetRecv(s->fd, buf, (unsigned)len, SCE_NET_MSG_DONTWAIT);
    if (n > 0) return n;
    if (n == 0) return -2;
    if ((uint32_t)n == (uint32_t)SCE_NET_ERROR_EAGAIN) return 0;
    *sys = n;
    return -1;
}
void SockClose(Sock* s) {
    if (s->ep >= 0) { sceNetEpollDestroy(s->ep); s->ep = -1; }
    if (s->fd >= 0) { sceNetSocketClose(s->fd); s->fd = -1; }
}

static int FillRandom(unsigned char* out, size_t len) {
#ifdef __APPLE__
    while (len > 0) {                               // getentropy: at most 256 bytes a call
        const size_t n = len < 256 ? len : 256;
        if (getentropy(out, n) != 0) return -1;
        out += n;
        len -= n;
    }
    return 0;
#endif
    while (len > 0) {
        const size_t n = len > 64 ? 64 : len;   // sceKernelGetRandomNumber: 64 bytes at most
        if (sceKernelGetRandomNumber(out, (SceSize)n) < 0) return -1;
        out += n;
        len -= n;
    }
    return 0;
}

// =================================================================================================================================
#else  // host (POSIX)
// =================================================================================================================================

int64_t MonoMs() {
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}
void SleepMs(uint32_t ms) {
    timespec ts;
    ts.tv_sec = ms / 1000;
    ts.tv_nsec = (long)(ms % 1000) * 1000000L;
    nanosleep(&ts, nullptr);
}
int64_t WallSeconds() {
    if (g_wall_override) return g_wall_override();
    timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (int64_t)ts.tv_sec;
}

struct Event {
    pthread_mutex_t m;
    pthread_cond_t c;
    int set;
};
Event* EventCreate() {
    Event* e = (Event*)calloc(1, sizeof *e);
    if (!e) return nullptr;
    pthread_mutex_init(&e->m, nullptr);
#ifdef __APPLE__
    pthread_cond_init(&e->c, nullptr);   // macOS: no condattr clock; EventWait uses the relative wait
#else
    pthread_condattr_t attr;
    pthread_condattr_init(&attr);
    pthread_condattr_setclock(&attr, CLOCK_MONOTONIC);
    pthread_cond_init(&e->c, &attr);
    pthread_condattr_destroy(&attr);
#endif
    return e;
}
void EventDestroy(Event* e) {
    if (!e) return;
    pthread_cond_destroy(&e->c);
    pthread_mutex_destroy(&e->m);
    free(e);
}
void EventSignal(Event* e) {
    pthread_mutex_lock(&e->m);
    e->set = 1;
    pthread_cond_signal(&e->c);
    pthread_mutex_unlock(&e->m);
}
bool EventWait(Event* e, uint32_t timeout_ms) {
    timespec until;
    clock_gettime(CLOCK_MONOTONIC, &until);
    until.tv_sec += timeout_ms / 1000;
    until.tv_nsec += (long)(timeout_ms % 1000) * 1000000L;
    if (until.tv_nsec >= 1000000000L) { until.tv_sec++; until.tv_nsec -= 1000000000L; }
    pthread_mutex_lock(&e->m);
    while (!e->set) {
#ifdef __APPLE__
        timespec now, rel;
        clock_gettime(CLOCK_MONOTONIC, &now);
        rel.tv_sec = until.tv_sec - now.tv_sec;
        rel.tv_nsec = until.tv_nsec - now.tv_nsec;
        if (rel.tv_nsec < 0) { rel.tv_sec--; rel.tv_nsec += 1000000000L; }
        if (rel.tv_sec < 0) break;
        if (pthread_cond_timedwait_relative_np(&e->c, &e->m, &rel) != 0) break;
#else
        if (pthread_cond_timedwait(&e->c, &e->m, &until) != 0) break;
#endif
    }
    const bool got = e->set != 0;
    e->set = 0;
    pthread_mutex_unlock(&e->m);
    return got;
}

struct Thread {
    pthread_t id;
    void (*entry)(void*);
    void* arg;
    volatile int done;
};
static void* ThreadTrampoline(void* p) {
    Thread* t = (Thread*)p;
    t->entry(t->arg);
    __atomic_store_n(&t->done, 1, __ATOMIC_RELEASE);
    return nullptr;
}
Thread* ThreadStart(void (*entry)(void*), void* arg) {
    Thread* t = (Thread*)calloc(1, sizeof *t);
    if (!t) return nullptr;
    t->entry = entry;
    t->arg = arg;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 256 * 1024);
    const int rc = pthread_create(&t->id, &attr, ThreadTrampoline, t);
    pthread_attr_destroy(&attr);
    if (rc != 0) { free(t); return nullptr; }
    return t;
}
bool ThreadJoin(Thread* t, uint32_t timeout_ms) {
    const int64_t until = MonoMs() + timeout_ms;
    while (!__atomic_load_n(&t->done, __ATOMIC_ACQUIRE)) {
        if (MonoMs() >= until) return false;
        SleepMs(2);
    }
    pthread_join(t->id, nullptr);
    free(t);
    return true;
}

bool NetworkUp() { return true; }
void ResolveAbort() {}
void ResolveAbortFor(const volatile int*) {}
#ifdef NP_HTTPS_TEST_HOOKS
static volatile uint32_t g_stallMs;
void TestStallResolve(uint32_t ms) { g_stallMs = ms; }
#endif

// getaddrinfo cannot be interrupted: it runs on a helper thread that is abandoned (and cleans up after itself) on time-out / cancel.
namespace {
struct ResolveJob {
    char host[256];
    uint32_t ip;
    int rc;
    volatile int done;
    volatile int refs;
};
void* ResolveMain(void* p) {
    ResolveJob* job = (ResolveJob*)p;
    addrinfo hints;
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* res = nullptr;
    const int rc = getaddrinfo(job->host, nullptr, &hints, &res);
    if (rc == 0 && res) {
        job->ip = ntohl(((sockaddr_in*)res->ai_addr)->sin_addr.s_addr);
        job->rc = 0;
    } else {
        job->rc = rc ? rc : -1;
    }
    if (res) freeaddrinfo(res);
    __atomic_store_n(&job->done, 1, __ATOMIC_RELEASE);
    if (__atomic_sub_fetch(&job->refs, 1, __ATOMIC_ACQ_REL) == 0) free(job);
    return nullptr;
}
}  // namespace

int Resolve(const char* host, uint32_t* ip, uint32_t timeout_ms, const volatile int* cancel, int* sys) {
    in_addr literal;
    if (inet_pton(AF_INET, host, &literal) == 1) { *ip = ntohl(literal.s_addr); return 0; }
    ResolveJob* job = (ResolveJob*)calloc(1, sizeof *job);
    if (!job) { *sys = ENOMEM; return NET_FAIL; }
    if (strlen(host) >= sizeof job->host) { free(job); *sys = EINVAL; return NET_FAIL; }
    strcpy(job->host, host);
    job->refs = 2;
    pthread_t th;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    if (pthread_create(&th, &attr, ResolveMain, job) != 0) { pthread_attr_destroy(&attr); free(job); *sys = EAGAIN; return NET_FAIL; }
    pthread_attr_destroy(&attr);
    const int64_t until = MonoMs() + timeout_ms;
    int result = NET_TIMEOUT;
#ifdef NP_HTTPS_TEST_HOOKS
    // a stuck resolver: does not look at `cancel` for a while, then goes on as usual
    for (const int64_t stall_until = MonoMs() + g_stallMs; MonoMs() < stall_until;) SleepMs(5);
#endif
    for (;;) {
        if (__atomic_load_n(&job->done, __ATOMIC_ACQUIRE)) {
            if (job->rc == 0) { *ip = job->ip; result = 0; } else { *sys = job->rc; result = NET_FAIL; }
            break;
        }
        if (*cancel) { result = NET_CANCELLED; break; }
        if (MonoMs() >= until) break;
        SleepMs(5);
    }
    if (__atomic_sub_fetch(&job->refs, 1, __ATOMIC_ACQ_REL) == 0) free(job);
    return result;
}

void SockInit(Sock* s) { s->fd = -1; s->ep = -1; }

int SockWait(Sock* s, bool for_write, uint32_t ms) {
    pollfd p;
    p.fd = s->fd;
    p.events = for_write ? POLLOUT : POLLIN;
    p.revents = 0;
    const int n = poll(&p, 1, (int)ms);
    if (n < 0) return errno == EINTR ? 0 : -1;
    if (n == 0) return 0;
    return 1;   // (POLLERR / POLLHUP are reported by the next send / recv)
}

int SockConnect(Sock* s, uint32_t ip, uint16_t port, uint32_t timeout_ms, const volatile int* cancel, int* sys) {
    s->fd = socket(AF_INET, SOCK_STREAM, 0);
#ifdef SO_NOSIGPIPE
    if (s->fd >= 0) { int one = 1; setsockopt(s->fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one); }
#endif
    if (s->fd < 0) { *sys = errno; return NET_FAIL; }
    fcntl(s->fd, F_SETFL, fcntl(s->fd, F_GETFL) | O_NONBLOCK);
    int one = 1;
    if (setsockopt(s->fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one) != 0) nph::Log("TCP_NODELAY refused (errno %d)", errno);   // Galaxy: logged
    sockaddr_in sa;
    memset(&sa, 0, sizeof sa);
    sa.sin_family = AF_INET;
    sa.sin_port = htons(port);
    sa.sin_addr.s_addr = htonl(ip);
    if (connect(s->fd, (sockaddr*)&sa, sizeof sa) == 0) return 0;
    if (errno != EINPROGRESS) { *sys = errno; return NET_FAIL; }
    int64_t left = timeout_ms;
    while (left > 0) {
        if (*cancel) return NET_CANCELLED;
        const uint32_t slice = left > 50 ? 50 : (uint32_t)left;
        const int w = SockWait(s, true, slice);
        if (w < 0) { *sys = errno; return NET_FAIL; }
        if (w > 0) {
            int err = 0;
            socklen_t len = sizeof err;
            if (getsockopt(s->fd, SOL_SOCKET, SO_ERROR, &err, &len) < 0) { *sys = errno; return NET_FAIL; }
            if (err != 0) { *sys = err; return err == ETIMEDOUT ? NET_TIMEOUT : NET_FAIL; }
            return 0;
        }
        left -= slice;
    }
    return NET_TIMEOUT;
}

int SockSend(Sock* s, const void* buf, size_t len, int* sys) {
    const ssize_t n = send(s->fd, buf, len, MSG_NOSIGNAL);
    if (n > 0) return (int)n;
    if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) return 0;
    *sys = n < 0 ? errno : EPIPE;
    return -1;
}
int SockRecv(Sock* s, void* buf, size_t len, int* sys) {
    const ssize_t n = recv(s->fd, buf, len, 0);
    if (n > 0) return (int)n;
    if (n == 0) return -2;
    if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) return 0;
    *sys = errno;
    return -1;
}
void SockClose(Sock* s) {
    if (s->fd >= 0) { close(s->fd); s->fd = -1; }
}

static int FillRandom(unsigned char* out, size_t len) {
#ifdef __APPLE__
    while (len > 0) {                               // getentropy: at most 256 bytes a call
        const size_t n = len < 256 ? len : 256;
        if (getentropy(out, n) != 0) return -1;
        out += n;
        len -= n;
    }
    return 0;
#else
    while (len > 0) {
        const ssize_t n = getrandom(out, len, 0);
        if (n <= 0) return -1;
        out += n;
        len -= (size_t)n;
    }
    return 0;
#endif
}
#endif

}  // namespace nph

// ---- what mbedtls asks its host for ----------------------------------------------------------------------------------------------
#if defined(NP_HAVE_MBEDTLS)
#include "mbedtls/build_info.h"
#include "mbedtls/entropy.h"
#include "mbedtls/platform_time.h"
#include "mbedtls/platform_util.h"

extern "C" {
// MBEDTLS_ENTROPY_HARDWARE_ALT: the only entropy source (the Vita's secure RNG; getrandom on the host).
int mbedtls_hardware_poll(void* data, unsigned char* output, size_t len, size_t* olen) {
    (void)data;
    if (nph::FillRandom(output, len) != 0) return MBEDTLS_ERR_ENTROPY_SOURCE_FAILED;
    *olen = len;
    return 0;
}
// MBEDTLS_PLATFORM_MS_TIME_ALT: a monotonic millisecond counter (TLS 1.3 ticket ages, nothing we depend on).
mbedtls_ms_time_t mbedtls_ms_time(void) { return (mbedtls_ms_time_t)nph::MonoMs(); }
// MBEDTLS_PLATFORM_TIME_MACRO: the wall clock the certificate dates are checked against. 0 would make every certificate "not yet
// valid"; np_https refuses to start a handshake without a believable clock (CLOCK error) so this is never consulted blind.
long long np_https_wallclock(long long* out) {
    const long long t = (long long)nph::WallSeconds();
    if (out) *out = t;
    return t;
}
// MBEDTLS_PLATFORM_GMTIME_R_ALT: seconds -> broken-down UTC, with no C library time zone code.
struct tm* mbedtls_platform_gmtime_r(const mbedtls_time_t* tt, struct tm* out) {
    long long t = (long long)*tt;
    if (t < 0) return nullptr;
    long long days = t / 86400;
    const long long rem = t % 86400;
    // civil_from_days
    days += 719468;
    const long long era = (days >= 0 ? days : days - 146096) / 146097;
    const unsigned doe = (unsigned)(days - era * 146097);
    const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const long long y = (long long)yoe + era * 400;
    const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const unsigned mp = (5 * doy + 2) / 153;
    const unsigned d = doy - (153 * mp + 2) / 5 + 1;
    const unsigned m = mp < 10 ? mp + 3 : mp - 9;
    memset(out, 0, sizeof *out);
    out->tm_year = (int)(y + (m <= 2) - 1900);
    out->tm_mon = (int)m - 1;
    out->tm_mday = (int)d;
    out->tm_hour = (int)(rem / 3600);
    out->tm_min = (int)(rem % 3600 / 60);
    out->tm_sec = (int)(rem % 60);
    return out;
}
}
#endif
