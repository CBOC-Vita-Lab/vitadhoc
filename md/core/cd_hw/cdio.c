/* VitAdhoc: the disc file layer of the Sega CD, see cdio.h for the design. Copyright CBOC-Vita. MIT licence for this file.
 *
 * Threads: the emulating thread owns everything except what the helper is handed by a block's state word.
 *   slot state FREE   : the emulating thread's
 *   slot state REQ    : asked for; the helper may claim it (REQ -> BUSY by compare-and-swap), the emulating thread may cancel it (REQ -> FREE)
 *   slot state BUSY   : the helper is reading / decoding into the slot: nobody else touches the slot
 *   slot state READY  : finished (data and len are valid); the helper never touches it again, the emulating thread reads and recycles it
 *   slot state ERR    : the helper could not read it; the emulating thread reads the bytes itself
 * The helper's list of caches and its descriptors are changed under one lock (a semaphore used as a mutex). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "thread_ops.h"
#include "cdio.h"
#include "libchdr/chd.h"
#include "libchdr/coretypes.h"
#include "zlib.h"

#ifdef __vita__
#include <psp2/io/fcntl.h>
static int io_open(const char *p) { return sceIoOpen(p, SCE_O_RDONLY, 0); }
static void io_close(int fd) { sceIoClose(fd); }
static int64_t io_size(int fd) { return (int64_t)sceIoLseek(fd, 0, SCE_SEEK_END); }
static long io_pread(int fd, void *b, size_t n, int64_t off) { return (long)sceIoPread(fd, b, (SceSize)n, (SceOff)off); }
#else
#include <fcntl.h>
#include <unistd.h>
#include <sys/types.h>
static int io_open(const char *p) { return open(p, O_RDONLY); }
static void io_close(int fd) { close(fd); }
static int64_t io_size(int fd) { off_t o = lseek(fd, 0, SEEK_END); return (int64_t)o; }
static long io_pread(int fd, void *b, size_t n, int64_t off) { return (long)pread(fd, b, n, (off_t)off); }
#endif

#define BLK 16384                 /* a block of a streamed file */
#define RAW_SLOTS 3               /* the block being read and two ahead */
#define CHD_SLOTS 8               /* hunks: the data track's and the audio track's, each with two ahead, and a spare */
#define MAXC 160                  /* caches the helper looks after: 99 tracks, the CHD, spare */
#define MAXF 256
#define EMU_FDS 16                /* descriptors the emulating thread keeps open at once (a 99 track disc has 99 files) */
#define HELP_FDS 8                /* the same for the helper */

enum { SLOT_FREE = 0, SLOT_REQ, SLOT_BUSY, SLOT_READY, SLOT_ERR };
enum { K_RAW = 0, K_CHD = 1 };

typedef struct {
	int state;                    /* atomic */
	unsigned seq;                 /* the order of the requests */
	int64_t blk;
	size_t len;                   /* bytes of data that are valid */
	unsigned long used;           /* recency (emulating thread) */
	int touched;                  /* the emulating thread took bytes from it */
	uint8_t *data;
} slot_t;

typedef struct cache {
	int kind;
	size_t blksz;
	int nslots;
	slot_t *s;
	uint8_t *mem;
	int64_t nblocks;
	struct cdio_file *file;       /* K_RAW */
	struct _chd_file *hchd;        /* K_CHD: the helper's decoder */
	int hfd;                      /* K_RAW: the helper's descriptor (changed under the lock only) */
	unsigned long hused;
	int registered;
	unsigned long tick;
} cache_t;

struct cdio_file {
	char *path;
	int fd;
	unsigned long fdused;
	int64_t size, pos;
	int stream;                   /* read ahead */
	int direct;                   /* no block cache: positioned reads of exactly what is asked (the file under a CHD) */
	int pinned;                   /* the descriptor stays open (used by the helper) */
	cache_t c;
};

static struct {
	const md_thread_ops *ops;
	int enabled, running;
	void *wake, *lock;
	int stop;
	cache_t *reg[MAXC]; int nreg;
	cdio_file *files[MAXF]; int nfiles;
	unsigned seq;
	unsigned long fdtick, hfdtick;          /* the emulating thread's and the helper's own clocks for their descriptors */
	int emu_fds, help_fds;
	int check, stress;
	unsigned erng, hrng;
	unsigned long st[9];
	struct { struct _chd_file *m, *h; cache_t c; uint32_t hunkbytes; int have; } chd;
} H;

static unsigned long ld(const unsigned long *p) { return __atomic_load_n(p, __ATOMIC_RELAXED); }
static void add(unsigned long *p, unsigned long v) { __atomic_fetch_add(p, v, __ATOMIC_RELAXED); }
static unsigned long now_us(void) { return H.ops && H.ops->now_us ? (unsigned long)H.ops->now_us(H.ops->user) : 0; }
static void lock(void) { H.ops->sem_wait(H.ops->user, H.lock); }
static void unlock(void) { H.ops->sem_post(H.ops->user, H.lock); }
static unsigned rnd(unsigned *s) { unsigned x = *s ? *s : 2463534242u; x ^= x << 13; x ^= x >> 17; x ^= x << 5; *s = x; return x; }

/* ---- files --------------------------------------------------------------------------------------------------- */

static int emu_fd(cdio_file *f)
{
	if (f->fd >= 0) { f->fdused = ++H.fdtick; return f->fd; }
	if (H.emu_fds >= EMU_FDS) {                 /* the budget: the descriptor of the file used least recently is closed (it is opened again when read) */
		cdio_file *v = NULL;
		int i;
		for (i = 0; i < H.nfiles; i++) { cdio_file *g = H.files[i]; if (g && g->fd >= 0 && !g->pinned && (!v || g->fdused < v->fdused)) v = g; }
		if (v) { io_close(v->fd); v->fd = -1; H.emu_fds--; }
	}
	f->fd = io_open(f->path);
	if (f->fd >= 0) { H.emu_fds++; f->fdused = ++H.fdtick; }
	return f->fd;
}

cdio_file *cdio_open(const char *path)
{
	cdio_file *f;
	int fd, i;
	if (!path || !*path || H.nfiles >= MAXF) return NULL;
	fd = io_open(path);
	if (fd < 0) return NULL;
	f = (cdio_file *)calloc(1, sizeof *f);
	if (!f) { io_close(fd); return NULL; }
	f->path = (char *)malloc(strlen(path) + 1);
	if (!f->path) { io_close(fd); free(f); return NULL; }
	strcpy(f->path, path);
	f->size = io_size(fd);
	if (f->size < 0) f->size = 0;
	f->fd = fd; f->fdused = ++H.fdtick;
	f->c.hfd = -1; f->c.kind = K_RAW; f->c.file = f;
	for (i = 0; i < MAXF; i++) if (!H.files[i]) { H.files[i] = f; break; }
	H.nfiles++;
	H.emu_fds++;
	if (H.emu_fds > EMU_FDS) { H.emu_fds--; io_close(f->fd); f->fd = -1; }     /* over the budget at once: opened again when read */
	return f;
}

static void cache_free(cache_t *c)
{
	free(c->mem); free(c->s);
	c->mem = NULL; c->s = NULL; c->nslots = 0;
}

static void unregister_cache(cache_t *c)           /* the lock is held */
{
	int i;
	for (i = 0; i < H.nreg; i++) if (H.reg[i] == c) { H.reg[i] = H.reg[--H.nreg]; break; }
	c->registered = 0;
}

static void wait_idle(cache_t *c)                  /* no slot of c is being worked on (the cache is no longer registered) */
{
	int i;
	for (i = 0; i < c->nslots; i++) {
		int st;
		while ((st = __atomic_load_n(&c->s[i].state, __ATOMIC_ACQUIRE)) == SLOT_BUSY) H.ops->yield(H.ops->user);
		if (st == SLOT_REQ) { int e = SLOT_REQ; __atomic_compare_exchange_n(&c->s[i].state, &e, SLOT_FREE, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE); }
		while (__atomic_load_n(&c->s[i].state, __ATOMIC_ACQUIRE) == SLOT_BUSY) H.ops->yield(H.ops->user);
	}
}

int cdio_close(cdio_file *f)
{
	int i;
	if (!f) return 0;
	if (f->c.registered) {
		lock();
		unregister_cache(&f->c);
		wait_idle(&f->c);                          /* a block the helper is on finishes first */
		if (f->c.hfd >= 0) { io_close(f->c.hfd); f->c.hfd = -1; H.help_fds--; }
		unlock();
	}
	if (f->fd >= 0) { io_close(f->fd); H.emu_fds--; }
	for (i = 0; i < MAXF; i++) if (H.files[i] == f) { H.files[i] = NULL; H.nfiles--; break; }
	cache_free(&f->c);
	free(f->path);
	free(f);
	return 0;
}

void cdio_stream(cdio_file *f) { if (f) f->stream = 1; }

static size_t direct_read(cdio_file *f, int64_t off, uint8_t *dst, size_t len)
{
	size_t got = 0;
	const int fd = f->pinned ? f->fd : emu_fd(f);        /* (a pinned file belongs to the helper thread: its descriptor is never closed behind its back) */
	if (fd < 0) return 0;
	while (got < len) {
		const long r = io_pread(fd, dst + got, len - got, off + (int64_t)got);
		if (r <= 0) break;
		got += (size_t)r;
	}
	return got;
}

/* ---- the cache ----------------------------------------------------------------------------------------------- */

static int cache_alloc(cache_t *c, int kind, size_t blksz, int nslots, int64_t nblocks)
{
	int i;
	c->kind = kind; c->blksz = blksz; c->nslots = nslots; c->nblocks = nblocks;
	c->s = (slot_t *)calloc((size_t)nslots, sizeof(slot_t));
	c->mem = (uint8_t *)malloc(blksz * (size_t)nslots);
	if (!c->s || !c->mem) { cache_free(c); return 0; }
	for (i = 0; i < nslots; i++) { c->s[i].data = c->mem + blksz * (size_t)i; c->s[i].blk = -1; }
	return 1;
}

static int find_slot(cache_t *c, int64_t blk)
{
	int i;
	for (i = 0; i < c->nslots; i++)
		if (__atomic_load_n(&c->s[i].state, __ATOMIC_ACQUIRE) != SLOT_FREE && c->s[i].blk == blk) return i;
	return -1;
}

static int pick_victim(cache_t *c)
{
	int i, best = -1;
	for (i = 0; i < c->nslots; i++) {
		const int st = __atomic_load_n(&c->s[i].state, __ATOMIC_ACQUIRE);
		if (st == SLOT_FREE) return i;
		if (st == SLOT_ERR) { best = i; break; }
		if (st == SLOT_READY && (best < 0 || c->s[i].used < c->s[best].used)) best = i;
	}
	return best;
}

static void register_cache(cache_t *c)
{
	lock();
	if (!c->registered && H.nreg < MAXC) { H.reg[H.nreg++] = c; c->registered = 1; }
	unlock();
}

/* ask the helper for a block (a no-op when it is there, being read, or asked for) */
static void request(cache_t *c, int64_t blk)
{
	int v;
	slot_t *s;
	if (blk < 0 || blk >= c->nblocks || !c->registered) return;
	v = find_slot(c, blk);
	if (v >= 0) { c->s[v].used = ++c->tick; return; }          /* there already: it stays young, so the least recently used block is one that is behind */
	v = pick_victim(c);
	if (v < 0) return;
	s = &c->s[v];
	if (__atomic_load_n(&s->state, __ATOMIC_ACQUIRE) == SLOT_READY && !s->touched) add(&H.st[7], 1);      /* read ahead for nothing */
	s->blk = blk; s->len = 0; s->touched = 0; s->used = ++c->tick;
	__atomic_store_n(&s->seq, ++H.seq, __ATOMIC_RELAXED);
	__atomic_store_n(&s->state, SLOT_REQ, __ATOMIC_RELEASE);
	H.ops->sem_post(H.ops->user, H.wake);
}

/* the slot holding `blk` finished, waiting for the helper when it is on it: 0 = there is no such block (read it yourself) */
static int take_ready(cache_t *c, int64_t blk, int *out_slot)
{
	int si = find_slot(c, blk), st;
	if (si < 0) return 0;
	st = __atomic_load_n(&c->s[si].state, __ATOMIC_ACQUIRE);
	if (st == SLOT_REQ) {                          /* not started: the emulating thread is quicker than waiting for the wake-up */
		int e = SLOT_REQ;
		if (__atomic_compare_exchange_n(&c->s[si].state, &e, SLOT_FREE, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) return 0;
		st = e;
	}
	if (st == SLOT_BUSY) {
		const unsigned long t0 = now_us();
		add(&H.st[3], 1);
		while ((st = __atomic_load_n(&c->s[si].state, __ATOMIC_ACQUIRE)) == SLOT_BUSY) H.ops->yield(H.ops->user);
		add(&H.st[4], now_us() - t0);
	}
	if (st == SLOT_ERR) { __atomic_store_n(&c->s[si].state, SLOT_FREE, __ATOMIC_RELEASE); return 0; }
	if (st != SLOT_READY) return 0;
	if (H.stress && !(rnd(&H.erng) & 3)) return 0;        /* stress: pretend it was not there */
	*out_slot = si;
	return 1;
}

/* a streamed file read through the cache and the helper */
static size_t async_read(cdio_file *f, int64_t off, uint8_t *dst, size_t len)
{
	cache_t *c = &f->c;
	size_t got = 0;
	int64_t last = off / BLK;
	if (!c->s) {
		if (!cache_alloc(c, K_RAW, BLK, RAW_SLOTS, (f->size + BLK - 1) / BLK)) return direct_read(f, off, dst, len);
		register_cache(c);
	}
	add(&H.st[0], 1);
	while (got < len) {
		const int64_t pos = off + (int64_t)got, blk = pos / BLK;
		const size_t boff = (size_t)(pos % BLK);
		int si;
		last = blk;
		if (take_ready(c, blk, &si)) {
			slot_t *s = &c->s[si];
			size_t k;
			if (boff >= s->len) break;                 /* the end of the file */
			k = s->len - boff; if (k > len - got) k = len - got;
			memcpy(dst + got, s->data + boff, k);
			if (H.check) {
				uint8_t tmp[BLK];
				const size_t n = direct_read(f, pos, tmp, k);
				if (n != k || memcmp(tmp, dst + got, k)) add(&H.st[8], 1);
			}
			s->touched = 1; s->used = ++c->tick;
			add(&H.st[1], 1);
			got += k;
		} else {
			const unsigned long t0 = now_us();
			const size_t n = direct_read(f, pos, dst + got, len - got);     /* exactly the bytes asked for */
			add(&H.st[2], 1); add(&H.st[4], now_us() - t0);
			got += n;
			last = (off + (int64_t)got - 1) / BLK;
			if (last < blk) last = blk;
			break;
		}
	}
	if (got) last = (off + (int64_t)got - 1) / BLK;
	request(c, last); request(c, last + 1); request(c, last + 2);
	return got;
}

/* no helper: a miss fills the one block of the file, as a buffered stream does */
static size_t sync_read(cdio_file *f, int64_t off, uint8_t *dst, size_t len)
{
	cache_t *c = &f->c;
	size_t got = 0;
	if (!c->s && !cache_alloc(c, K_RAW, BLK, 1, (f->size + BLK - 1) / BLK)) return direct_read(f, off, dst, len);
	if (f->stream) add(&H.st[0], 1);
	while (got < len) {
		const int64_t pos = off + (int64_t)got, blk = pos / BLK;
		const size_t boff = (size_t)(pos % BLK);
		slot_t *s = &c->s[0];
		size_t k;
		if (s->state != SLOT_READY || s->blk != blk) {
			const unsigned long t0 = f->stream ? now_us() : 0;
			s->len = direct_read(f, blk * BLK, s->data, BLK);
			s->blk = blk; s->state = SLOT_READY;
			if (f->stream) { add(&H.st[2], 1); add(&H.st[4], now_us() - t0); }
		} else if (f->stream) add(&H.st[1], 1);
		if (boff >= s->len) break;
		k = s->len - boff; if (k > len - got) k = len - got;
		memcpy(dst + got, s->data + boff, k);
		got += k;
	}
	return got;
}

size_t cdio_read(void *p, size_t size, size_t n, cdio_file *f)
{
	size_t want, got;
	if (!f || !size || !n) return 0;
	want = size * n;
	if (f->pos >= f->size) return 0;
	if ((int64_t)want > f->size - f->pos) want = (size_t)(f->size - f->pos);
	if (f->direct) got = direct_read(f, f->pos, (uint8_t *)p, want);
	else if (f->stream && H.running) got = async_read(f, f->pos, (uint8_t *)p, want);
	else got = sync_read(f, f->pos, (uint8_t *)p, want);
	f->pos += (int64_t)got;
	return got / size;
}

int cdio_seek(cdio_file *f, long off, int whence)
{
	int64_t t;
	if (!f) return -1;
	t = whence == SEEK_SET ? (int64_t)off : whence == SEEK_CUR ? f->pos + off : whence == SEEK_END ? f->size + off : -1;
	if (t < 0) return -1;
	f->pos = t;
	return 0;
}

long cdio_tell(cdio_file *f) { return f ? (long)f->pos : 0; }

char *cdio_gets(char *s, int n, cdio_file *f)
{
	size_t want, got, i;
	if (!f || n < 2 || f->pos >= f->size) return NULL;
	want = (size_t)(n - 1);
	if ((int64_t)want > f->size - f->pos) want = (size_t)(f->size - f->pos);
	got = f->direct ? direct_read(f, f->pos, (uint8_t *)s, want) : sync_read(f, f->pos, (uint8_t *)s, want);
	for (i = 0; i < got; i++) if (s[i] == '\n') { got = i + 1; break; }
	if (!got) return NULL;
	s[got] = 0;
	f->pos += (int64_t)got;
	return s;
}

/* ---- CHD ----------------------------------------------------------------------------------------------------- */

static uint64_t cio_fsize(core_file *c) { return (uint64_t)((cdio_file *)c->argp)->size; }
static size_t cio_fread(void *p, size_t size, size_t n, core_file *c) { return cdio_read(p, size, n, (cdio_file *)c->argp); }
static int cio_fseek(core_file *c, int64_t off, int whence) { return cdio_seek((cdio_file *)c->argp, (long)off, whence); }
static int core_fclose_nonowner(core_file *c) { free(c); return 0; }              /* the track file stays open: the core closes it */
static int core_fclose_owner(core_file *c) { cdio_close((cdio_file *)c->argp); free(c); return 0; }

static core_file *core_wrap(cdio_file *f, int owner)
{
	core_file *c = (core_file *)malloc(sizeof *c);
	if (!c) return NULL;
	c->argp = f;
	c->fsize = cio_fsize; c->fread = cio_fread; c->fseek = cio_fseek;
	c->fclose = owner ? core_fclose_owner : core_fclose_nonowner;
	return c;
}

int cdio_chd_open(cdio_file *f, struct _chd_file **out)
{
	core_file *core;
	chd_error e;
	cdio_file *hf;
	*out = NULL;
	if (!f) return CHDERR_FILE_NOT_FOUND;
	f->direct = 1;                                  /* libchdr reads compressed hunks at arbitrary offsets: exactly those bytes */
	core = core_wrap(f, 0);
	if (!core) return CHDERR_OUT_OF_MEMORY;
	e = chd_open_core_file(core, CHD_OPEN_READ, NULL, out);
	if (e != CHDERR_NONE) { *out = NULL; return (int)e; }
	/* the helper's own decoder over a second descriptor of the same file (a decoder has state: two threads never share one) */
	memset(&H.chd, 0, sizeof H.chd);
	H.chd.m = *out;
	hf = cdio_open(f->path);
	if (hf) {
		core_file *hc = core_wrap(hf, 1);
		if (hc) {
			hf->direct = 1; hf->pinned = 1;
			if (hf->fd < 0) emu_fd(hf);
			if (chd_open_core_file(hc, CHD_OPEN_READ, NULL, &H.chd.h) != CHDERR_NONE) H.chd.h = NULL;
		} else cdio_close(hf);
	}
	{
		const chd_header *hd = chd_get_header(*out);
		H.chd.hunkbytes = hd->hunkbytes;
		H.chd.have = H.chd.h && hd->hunkbytes && hd->hunkbytes <= (1u << 20) && hd->totalhunks;
		if (H.chd.have) {
			H.chd.c.hchd = H.chd.h;
			if (!cache_alloc(&H.chd.c, K_CHD, hd->hunkbytes, CHD_SLOTS, (int64_t)hd->totalhunks)) H.chd.have = 0;
		}
	}
	return 0;
}

void cdio_chd_close(struct _chd_file *chd)
{
	if (!chd) return;
	if (H.chd.m == chd) {
		if (H.chd.c.registered) {
			lock();
			unregister_cache(&H.chd.c);
			wait_idle(&H.chd.c);
			unlock();
		}
		cache_free(&H.chd.c);
		if (H.chd.h) chd_close(H.chd.h);              /* closes its descriptor too */
		memset(&H.chd, 0, sizeof H.chd);
	}
	chd_close(chd);
}

int cdio_chd_read(struct _chd_file *chd, uint32_t hunknum, uint8_t *dst)
{
	cache_t *c = &H.chd.c;
	int si, err;
	unsigned long t0;
	if (chd != H.chd.m || !H.chd.have) return (int)chd_read(chd, hunknum, dst);
	if (!c->registered && H.running) register_cache(c);
	add(&H.st[0], 1);
	if (c->registered && take_ready(c, hunknum, &si)) {
		slot_t *s = &c->s[si];
		memcpy(dst, s->data, H.chd.hunkbytes);
		if (H.check) {
			uint8_t *tmp = (uint8_t *)malloc(H.chd.hunkbytes);
			if (tmp) {
				if (chd_read(chd, hunknum, tmp) != CHDERR_NONE || memcmp(tmp, dst, H.chd.hunkbytes)) add(&H.st[8], 1);
				free(tmp);
			}
		}
		s->touched = 1; s->used = ++c->tick;
		add(&H.st[1], 1);
		err = CHDERR_NONE;
	} else {
		t0 = now_us();
		err = (int)chd_read(chd, hunknum, dst);
		add(&H.st[2], 1); add(&H.st[4], now_us() - t0);
	}
	if (c->registered) { request(c, (int64_t)hunknum + 1); request(c, (int64_t)hunknum + 2); }
	return err;
}

/* ---- the helper ---------------------------------------------------------------------------------------------- */

static void stress_pause(void)
{
	if (H.stress) { unsigned n = rnd(&H.hrng) % 300, i; for (i = 0; i < n; i++) H.ops->yield(H.ops->user); }
}

static void help_fd(cache_t *c)                    /* the lock is held; K_RAW */
{
	if (c->hfd < 0) {
		if (H.help_fds >= HELP_FDS) {
			cache_t *v = NULL;
			int i;
			for (i = 0; i < H.nreg; i++) { cache_t *g = H.reg[i]; if (g != c && g->kind == K_RAW && g->hfd >= 0 && (!v || g->hused < v->hused)) v = g; }
			if (v) { io_close(v->hfd); v->hfd = -1; H.help_fds--; }
		}
		c->hfd = io_open(c->file->path);
		if (c->hfd >= 0) H.help_fds++;
	}
	c->hused = ++H.hfdtick;
}

static void helper_work(cache_t *c, slot_t *s)
{
	const unsigned long t0 = now_us();
	int ok = 0;
	stress_pause();
	if (c->kind == K_RAW) {
		if (c->hfd >= 0) {
			size_t got = 0;
			const int64_t off = s->blk * BLK;
			for (;;) {
				long r;
				if (got >= BLK) break;
				r = io_pread(c->hfd, s->data + got, BLK - got, off + (int64_t)got);
				if (r < 0) { got = (size_t)-1; break; }
				if (r == 0) break;
				got += (size_t)r;
			}
			if (got != (size_t)-1) { s->len = got; ok = 1; }
		}
	} else {
		if (chd_read(c->hchd, (uint32_t)s->blk, s->data) == CHDERR_NONE) { s->len = c->blksz; ok = 1; }
	}
	stress_pause();
	add(&H.st[5], 1); add(&H.st[6], now_us() - t0);
	__atomic_store_n(&s->state, ok ? SLOT_READY : SLOT_ERR, __ATOMIC_RELEASE);
}

static void helper_main(void *arg)
{
	(void)arg;
	for (;;) {
		H.ops->sem_wait(H.ops->user, H.wake);
		if (__atomic_load_n(&H.stop, __ATOMIC_ACQUIRE)) break;
		for (;;) {
			cache_t *bc = NULL;
			slot_t *bs = NULL;
			unsigned bseq = 0;
			int i, j, claimed = 0;
			lock();
			for (;;) {                                  /* the oldest request of all the caches */
				bc = NULL; bs = NULL;
				for (i = 0; i < H.nreg; i++) {
					cache_t *c = H.reg[i];
					for (j = 0; j < c->nslots; j++)
						if (__atomic_load_n(&c->s[j].state, __ATOMIC_ACQUIRE) == SLOT_REQ) {
							const unsigned q = __atomic_load_n(&c->s[j].seq, __ATOMIC_RELAXED);     /* (a request may be cancelled and made again under our eyes: the claim below decides) */
							if (!bs || (int)(q - bseq) < 0) { bc = c; bs = &c->s[j]; bseq = q; }
						}
				}
				if (!bs) break;
				if (bc->kind == K_RAW) help_fd(bc);
				{ int e = SLOT_REQ; if (__atomic_compare_exchange_n(&bs->state, &e, SLOT_BUSY, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) { claimed = 1; break; } }
			}
			unlock();
			if (!claimed) break;
			helper_work(bc, bs);
			if (__atomic_load_n(&H.stop, __ATOMIC_ACQUIRE)) break;
		}
	}
}

/* zlib builds its fixed Huffman tables the first time one is needed (BUILDFIXED), into static memory: make that happen on one thread, before a second one decodes */
static void warm_zlib(void)
{
	static const unsigned char fixed[3] = { 0x4b, 0x04, 0x00 };       /* the fixed-table block of the byte "a" */
	z_stream z;
	unsigned char out[4];
	memset(&z, 0, sizeof z);
	if (inflateInit2(&z, -15) == Z_OK) {
		z.next_in = (Bytef *)fixed; z.avail_in = sizeof fixed; z.next_out = out; z.avail_out = sizeof out;
		inflate(&z, Z_FINISH);
		inflateEnd(&z);
	}
}

void cdio_set_ops(const md_thread_ops *ops, int enabled)
{
	H.ops = ops;
	H.enabled = ops && enabled && ops->sem_create && ops->sem_post && ops->sem_wait && ops->spawn && ops->wait_end && ops->yield;
}

int cdio_helper_running(void) { return H.running; }

void cdio_debug(int check, int stress) { H.check = check; H.stress = stress; }

void cdio_begin(void)
{
	int i;
	memset(H.st, 0, sizeof H.st);
	H.erng = 12345; H.hrng = 54321;
	if (!H.enabled || H.running) return;
	warm_zlib();
	/* every file starts again with caches of the helper's kind */
	for (i = 0; i < MAXF; i++) if (H.files[i] && H.files[i]->c.s && !H.files[i]->c.registered) cache_free(&H.files[i]->c);
	H.wake = H.ops->sem_create(H.ops->user);
	H.lock = H.ops->sem_create(H.ops->user);
	if (!H.wake || !H.lock) { if (H.ops->sem_destroy) { if (H.wake) H.ops->sem_destroy(H.ops->user, H.wake); if (H.lock) H.ops->sem_destroy(H.ops->user, H.lock); } H.wake = H.lock = NULL; return; }
	H.ops->sem_post(H.ops->user, H.lock);
	H.stop = 0; H.nreg = 0; H.seq = 0;
	if (H.ops->spawn(H.ops->user, MD_WORKER_DISC, helper_main, NULL)) {
		if (H.ops->sem_destroy) { H.ops->sem_destroy(H.ops->user, H.wake); H.ops->sem_destroy(H.ops->user, H.lock); }
		H.wake = H.lock = NULL;
		return;
	}
	H.running = 1;
	/* the first blocks of the data track and of the first hunks are wanted at once */
}

void cdio_end(void)
{
	int i;
	if (!H.running) return;
	__atomic_store_n(&H.stop, 1, __ATOMIC_RELEASE);
	H.ops->sem_post(H.ops->user, H.wake);
	H.ops->wait_end(H.ops->user, MD_WORKER_DISC);
	H.running = 0;
	for (i = 0; i < MAXF; i++) {
		cdio_file *f = H.files[i];
		if (f && f->c.registered) {
			f->c.registered = 0;
			if (f->c.hfd >= 0) { io_close(f->c.hfd); f->c.hfd = -1; H.help_fds--; }
			cache_free(&f->c);
		}
	}
	H.chd.c.registered = 0; H.nreg = 0;
	for (i = 0; i < H.chd.c.nslots; i++) { H.chd.c.s[i].state = SLOT_FREE; H.chd.c.s[i].blk = -1; }
	if (H.ops->sem_destroy) { H.ops->sem_destroy(H.ops->user, H.wake); H.ops->sem_destroy(H.ops->user, H.lock); }
	H.wake = H.lock = NULL;
}

void cdio_census(unsigned long out[9])
{
	int i;
	for (i = 0; i < 9; i++) out[i] = ld(&H.st[i]);
}
