/* pk_csearch.h -- "Search memory": find the address of a game's lives, money or health by narrowing a list of
 * candidates, as RetroArch's and mGBA's cheat search do. MIT licence (CBOC-Vita).
 *
 * One header, plain C that also compiles as C++, so the main app (C, GPL) and the two players (C++, their own
 * licences) use the very same engine and the very same wording. Nothing here draws or reads a file.
 *
 * THE MACHINE.  The engine knows the machine only through a read callback in the console's RetroAchievements
 * address space (fe_host_ra_read, gbcore_ra_read, snes_host_ra_read, md_host_ra_read ...) and a short list of
 * regions of it to search (the work RAM: what a typed RAM code can reach). Reads have no side effects.
 *
 * A SEARCH.  pk_cs_start(width 1, 2 or 4) snapshots the regions and makes every aligned address a candidate.
 * Each pk_cs_step() compares the candidates' current value with a typed number (Equal to, Not equal to, Greater
 * than, Less than) or with the snapshot of the previous step (Changed, Unchanged, Increased, Decreased), keeps
 * the ones that match and takes a new snapshot. Values are unsigned. Memory: one byte of snapshot per searched
 * byte plus one bit per address (the GBA's 288 KB of work RAM costs 288 KB + 36 KB, freed by pk_cs_free).
 * A step scans in 4 KB chunks and is meant to run with the menu open and the game paused.
 *
 * A RESULT becomes a cheat with pk_cs_code(): the console's own typed RAM code, keeping the value now there. */
#ifndef PK_CSEARCH_H
#define PK_CSEARCH_H

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PK_CS_MAX_REGIONS 4
#define PK_CS_LIST_MAX    50      /* results are listed (address and value) when there are this many or fewer */
#define PK_CS_CHUNK       4096

/* the steps, in the order the menu shows them */
enum { PK_CS_EQ = 0, PK_CS_NE, PK_CS_GT, PK_CS_LT, PK_CS_CHANGED, PK_CS_UNCHANGED, PK_CS_INC, PK_CS_DEC, PK_CS_OPS };
/* how a 2 or 4 byte value is laid out in the machine */
enum { PK_CS_LE = 0, PK_CS_BE, PK_CS_MD };   /* PK_CS_MD: little endian words, the high word first (the Mega Drive's work
                                                 * RAM as the core holds it, md/frontend/host.h) */
/* which typed code a result becomes */
enum { PK_CS_GBA = 0, PK_CS_GB, PK_CS_NES, PK_CS_WS, PK_CS_SNES, PK_CS_MDW, PK_CS_Z80 };

typedef uint32_t (*pk_cs_read_fn)(void *ud, uint32_t addr, uint8_t *buf, uint32_t n);

typedef struct pk_cs_region { uint32_t base, size; } pk_cs_region;

typedef struct pk_cs
{
	pk_cs_read_fn read;
	void *ud;
	pk_cs_region reg[PK_CS_MAX_REGIONS];
	int nreg;
	int order;                   /* PK_CS_LE / BE / MD */
	int kind;                    /* PK_CS_GBA ... for pk_cs_code */
	uint32_t total;              /* bytes in all regions */
	uint8_t *snap;               /* total bytes: the values at the last step */
	uint8_t *bits;               /* (total + 7) / 8: bit i = the address at byte offset i is a candidate */
	int width;                   /* 1, 2 or 4; 0 = no search */
	uint32_t count;
	int steps;                   /* steps since pk_cs_start */
} pk_cs;

static inline void pk_cs_init(pk_cs *cs, pk_cs_read_fn read, void *ud, int kind, int order,
                              const pk_cs_region *reg, int nreg)
{
	int i;
	memset(cs, 0, sizeof *cs);
	cs->read = read;
	cs->ud = ud;
	cs->kind = kind;
	cs->order = order;
	if (nreg > PK_CS_MAX_REGIONS)
		nreg = PK_CS_MAX_REGIONS;
	for (i = 0; i < nreg; i++)
	{
		cs->reg[i] = reg[i];
		cs->total += reg[i].size;
	}
	cs->nreg = nreg;
}

static inline void pk_cs_free(pk_cs *cs)
{
	free(cs->snap);
	free(cs->bits);
	cs->snap = 0;
	cs->bits = 0;
	cs->width = 0;
	cs->count = 0;
	cs->steps = 0;
}

static inline int pk_cs_active(const pk_cs *cs) { return cs->width != 0; }

/* byte offset in the searched bytes -> address in the machine's map */
static inline uint32_t pk_cs_addr(const pk_cs *cs, uint32_t off)
{
	int i;
	for (i = 0; i < cs->nreg; i++)
	{
		if (off < cs->reg[i].size)
			return cs->reg[i].base + off;
		off -= cs->reg[i].size;
	}
	return 0;
}

/* The value of `w` bytes at b[0..w-1] (bytes in machine address order). */
static inline uint32_t pk_cs_value(const pk_cs *cs, const uint8_t *b, int w)
{
	if (w == 1)
		return b[0];
	if (w == 2)
		return cs->order == PK_CS_BE ? (uint32_t)(b[0] << 8 | b[1]) : (uint32_t)(b[0] | b[1] << 8);
	if (cs->order == PK_CS_BE)
		return (uint32_t)b[0] << 24 | (uint32_t)b[1] << 16 | (uint32_t)b[2] << 8 | b[3];
	if (cs->order == PK_CS_MD)
		return (uint32_t)(b[0] | b[1] << 8) << 16 | (uint32_t)(b[2] | b[3] << 8);
	return (uint32_t)b[0] | (uint32_t)b[1] << 8 | (uint32_t)b[2] << 16 | (uint32_t)b[3] << 24;
}

static inline uint32_t pk_cs_max_value(int w) { return w == 1 ? 0xFFu : w == 2 ? 0xFFFFu : 0xFFFFFFFFu; }

static inline int pk_cs_bit(const pk_cs *cs, uint32_t off) { return (cs->bits[off >> 3] >> (off & 7)) & 1; }
static inline void pk_cs_clear(pk_cs *cs, uint32_t off) { cs->bits[off >> 3] &= (uint8_t)~(1u << (off & 7)); }

/* Read searched bytes [off, off+n) (inside one region; the caller keeps to that) into out. Returns the bytes read. */
static inline uint32_t pk_cs_fetch(const pk_cs *cs, uint32_t off, uint8_t *out, uint32_t n)
{
	int i;
	uint32_t base = 0;
	for (i = 0; i < cs->nreg; i++)
	{
		if (off < base + cs->reg[i].size)
		{
			uint32_t in = off - base, k = cs->reg[i].size - in;
			if (k > n)
				k = n;
			return cs->read ? cs->read(cs->ud, cs->reg[i].base + in, out, k) : 0;
		}
		base += cs->reg[i].size;
	}
	return 0;
}

/* One pass over every region in chunks of 4 KB (each chunk starts at a multiple of the width inside its region, so no
 * value straddles two). `op` < 0 is the first pass (start); otherwise a step. */
static inline void pk_cs_pass(pk_cs *cs, int op, uint32_t value)
{
	uint8_t buf[PK_CS_CHUNK];
	const uint32_t w = (uint32_t)cs->width;
	uint32_t base = 0, n = 0;
	int r;
	for (r = 0; r < cs->nreg; r++)
	{
		uint32_t size = cs->reg[r].size, in;
		for (in = 0; in < size; in += PK_CS_CHUNK)
		{
			uint32_t want = size - in < PK_CS_CHUNK ? size - in : PK_CS_CHUNK, got, i;
			if (op >= 0)
			{
				int any = 0;
				for (i = 0; i < want && !any; i++)
					any = pk_cs_bit(cs, base + in + i);
				if (!any)
					continue;
			}
			got = cs->read ? cs->read(cs->ud, cs->reg[r].base + in, buf, want) : 0;
			if (got > want)
				got = want;
			for (i = 0; i + w <= want; i += w)
			{
				const uint32_t o = base + in + i;
				if (op < 0)
				{
					if (i + w <= got)
					{
						cs->bits[o >> 3] |= (uint8_t)(1u << (o & 7));
						n++;
					}
				}
				else if (pk_cs_bit(cs, o))
				{
					int keep = 0;
					if (i + w <= got)
					{
						uint32_t cur = pk_cs_value(cs, buf + i, (int)w), old = pk_cs_value(cs, cs->snap + o, (int)w);
						switch (op)
						{
						case PK_CS_EQ: keep = cur == value; break;
						case PK_CS_NE: keep = cur != value; break;
						case PK_CS_GT: keep = cur > value; break;
						case PK_CS_LT: keep = cur < value; break;
						case PK_CS_CHANGED: keep = cur != old; break;
						case PK_CS_UNCHANGED: keep = cur == old; break;
						case PK_CS_INC: keep = cur > old; break;
						case PK_CS_DEC: keep = cur < old; break;
						}
					}
					if (keep)
						n++;
					else
						pk_cs_clear(cs, o);
				}
			}
			memcpy(cs->snap + base + in, buf, got);
		}
		base += size;
	}
	cs->count = n;
}

/* Begin a new search over `width` byte unsigned values: every aligned address the machine answers for is a candidate.
 * Returns the number of candidates, or -1 (no memory, a width other than 1, 2, 4). */
static inline int32_t pk_cs_start(pk_cs *cs, int width)
{
	pk_cs_free(cs);
	if ((width != 1 && width != 2 && width != 4) || cs->total < (uint32_t)width)
		return -1;
	cs->snap = (uint8_t *)calloc(cs->total, 1);
	cs->bits = (uint8_t *)calloc((cs->total + 7) / 8, 1);
	if (!cs->snap || !cs->bits)
	{
		pk_cs_free(cs);
		return -1;
	}
	cs->width = width;
	pk_cs_pass(cs, -1, 0);
	return (int32_t)cs->count;
}

/* One refinement. `value` is used by EQ/NE/GT/LT (the caller has checked it fits: pk_cs_parse). Returns the number of
 * candidates left, or -1 when no search is active. */
static inline int32_t pk_cs_step(pk_cs *cs, int op, uint32_t value)
{
	if (!cs->width || !cs->snap || op < 0 || op >= PK_CS_OPS)
		return -1;
	pk_cs_pass(cs, op, value);
	cs->steps++;
	return (int32_t)cs->count;
}

/* The first `max` candidates in address order: their machine address and their value now (read live). Returns the
 * number listed. */
static inline int pk_cs_list(const pk_cs *cs, int max, uint32_t *addr, uint32_t *val)
{
	int n = 0;
	uint32_t o;
	if (!cs->width)
		return 0;
	for (o = 0; o < cs->total && n < max; o++)
	{
		uint8_t b[4];
		if (!pk_cs_bit(cs, o))
			continue;
		memset(b, 0, sizeof b);
		if (pk_cs_fetch(cs, o, b, (uint32_t)cs->width) < (uint32_t)cs->width)
			memset(b, 0, sizeof b);
		addr[n] = pk_cs_addr(cs, o);
		val[n] = pk_cs_value(cs, b, cs->width);
		n++;
	}
	return n;
}

/* ---- from a result to the typed code of each console ----------------------------------------------------------- */

/* The byte of the value `v` (width w) at index k of the machine's byte order. */
static inline uint8_t pk_cs_vbyte(const pk_cs *cs, uint32_t v, int w, int k)
{
	if (w == 1)
		return (uint8_t)v;
	if (cs->order == PK_CS_BE)
		return (uint8_t)(v >> (8 * (w - 1 - k)));
	if (cs->order == PK_CS_MD)
	{
		/* bytes: w0 low, w0 high, w1 low, w1 high where the value is w0:w1 (high word first) */
		static const int sh[4] = { 16, 24, 0, 8 };
		return w == 2 ? (uint8_t)(v >> (8 * k)) : (uint8_t)(v >> sh[k]);
	}
	return (uint8_t)(v >> (8 * k));
}

/* The code for the result at machine address `addr`, keeping the value `v` it holds now (pk_cs_list's). Several
 * pokes are joined by '+' (a one-byte console writes a wider value byte by byte). Returns the length, or 0 when
 * `addr` cannot be reached by a typed code or `cap` is too small. */
static inline int pk_cs_code(const pk_cs *cs, uint32_t addr, uint32_t v, char *out, size_t cap)
{
	const int w = cs->width;
	char t[160];
	size_t len = 0;
	int k;
	if (!w || cap < 2)
		return 0;
	out[0] = 0;
	if (cs->kind == PK_CS_GBA)
	{
		/* RAM address: AAAAAAAA with VV, VVVV or VVVVVVVV */
		uint32_t real = addr < 0x8000 ? 0x03000000u + addr : 0x02000000u + (addr - 0x8000);
		if (w == 1) snprintf(t, sizeof t, "%08X %02X", (unsigned)real, (unsigned)v);
		else if (w == 2) snprintf(t, sizeof t, "%08X %04X", (unsigned)real, (unsigned)v);
		else snprintf(t, sizeof t, "%08X %08X", (unsigned)real, (unsigned)v);
		if (strlen(t) + 1 > cap)
			return 0;
		strcpy(out, t);
		return (int)strlen(out);
	}
	if (cs->kind == PK_CS_MDW)
	{
		/* AAAAAA:VVVV, a word at an even 68000 address. "Keep the value" writes back what is there now, so a byte or a
		 * long is the word or words it lies in, read as they are (the core's layout: the byte at X is the 68000's byte at
		 * X ^ 1, so the word is the two bytes at the even X, low byte first). */
		uint32_t a, first = addr & ~1u, last = (addr + (uint32_t)w - 1) & ~1u;
		(void)v;
		for (a = first; a <= last; a += 2)
		{
			uint8_t b[2];
			if (!cs->read || cs->read(cs->ud, a, b, 2) != 2)
				return 0;
			snprintf(t, sizeof t, "%s%06X:%04X", len ? "+" : "", (unsigned)(0xFF0000u + a), (unsigned)(b[0] | b[1] << 8));
			if (len + strlen(t) + 1 > cap)
				return 0;
			strcpy(out + len, t);
			len += strlen(t);
		}
		return (int)len;
	}
	for (k = 0; k < w; k++)
	{
		unsigned b = pk_cs_vbyte(cs, v, w, k);
		uint32_t a = addr + (uint32_t)k;
		switch (cs->kind)
		{
		case PK_CS_GB:                        /* GameShark 01VVAAAA, the address low byte first */
			snprintf(t, sizeof t, "%s01%02X%02X%02X", k ? "+" : "", b, (unsigned)(a & 0xFF), (unsigned)((a >> 8) & 0xFF));
			break;
		case PK_CS_NES:
			snprintf(t, sizeof t, "%s%04X:%02X", k ? "+" : "", (unsigned)a, b);
			break;
		case PK_CS_WS:
			snprintf(t, sizeof t, "%s%05X:%02X", k ? "+" : "", (unsigned)a, b);
			break;
		case PK_CS_SNES:                      /* 7E0000 + the work RAM's offset */
			snprintf(t, sizeof t, "%s%06X:%02X", k ? "+" : "", (unsigned)(a < 0x10000 ? 0x7E0000u + a : 0x7F0000u + (a - 0x10000)), b);
			break;
		case PK_CS_Z80:                       /* AAAA:VV, the Z80's address of the work RAM */
			snprintf(t, sizeof t, "%s%04X:%02X", k ? "+" : "", (unsigned)(0xC000u + a), b);
			break;
		default:
			return 0;
		}
		if (len + strlen(t) + 1 > cap)
			return 0;
		strcpy(out + len, t);
		len += strlen(t);
	}
	return (int)len;
}

/* The address as the console's own codes write it (the list shows this). */
static inline void pk_cs_addr_text(const pk_cs *cs, uint32_t addr, char *out, size_t cap)
{
	switch (cs->kind)
	{
	case PK_CS_GBA: snprintf(out, cap, "%08X", (unsigned)(addr < 0x8000 ? 0x03000000u + addr : 0x02000000u + (addr - 0x8000))); break;
	case PK_CS_GB: case PK_CS_NES: snprintf(out, cap, "%04X", (unsigned)addr); break;
	case PK_CS_WS: snprintf(out, cap, "%05X", (unsigned)addr); break;
	case PK_CS_SNES: snprintf(out, cap, "%06X", (unsigned)(addr < 0x10000 ? 0x7E0000u + addr : 0x7F0000u + (addr - 0x10000))); break;
	case PK_CS_MDW: snprintf(out, cap, "%06X", (unsigned)(0xFF0000u + ((cs->width == 1 ? addr ^ 1u : addr) & 0xFFFFu))); break;
	case PK_CS_Z80: snprintf(out, cap, "%04X", (unsigned)(0xC000u + addr)); break;
	default: snprintf(out, cap, "%X", (unsigned)addr); break;
	}
}

/* ---- the words, the same on every console ----------------------------------------------------------------------- */

static inline const char *pk_cs_size_name(int width) { return width == 1 ? "1 byte" : width == 2 ? "2 bytes" : "4 bytes"; }
static inline const char *pk_cs_op_name(int op)
{
	static const char *const n[PK_CS_OPS] = { "Equal to", "Not equal to", "Greater than", "Less than",
	                                          "Changed", "Unchanged", "Increased", "Decreased" };
	return op >= 0 && op < PK_CS_OPS ? n[op] : "";
}
/* does the step ask for a typed number? */
static inline int pk_cs_op_needs_value(int op) { return op <= PK_CS_LT; }

/* A typed number: decimal, or hex with 0x. Returns 1 and the value when it is a number that fits `width`. */
static inline int pk_cs_parse(const char *s, int width, uint32_t *out)
{
	uint64_t v = 0;
	int hex = 0, digits = 0;
	while (*s == ' ')
		s++;
	if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X'))
	{
		hex = 1;
		s += 2;
	}
	for (; *s && *s != ' '; s++)
	{
		int d;
		if (*s >= '0' && *s <= '9') d = *s - '0';
		else if (hex && *s >= 'a' && *s <= 'f') d = *s - 'a' + 10;
		else if (hex && *s >= 'A' && *s <= 'F') d = *s - 'A' + 10;
		else return 0;
		v = v * (hex ? 16u : 10u) + (uint64_t)d;
		if (v > 0xFFFFFFFFull)
			return 0;
		digits++;
	}
	while (*s == ' ')
		s++;
	if (!digits || *s || v > pk_cs_max_value(width))
		return 0;
	*out = (uint32_t)v;
	return 1;
}

#ifdef __cplusplus
}
#endif

#endif /* PK_CSEARCH_H */
