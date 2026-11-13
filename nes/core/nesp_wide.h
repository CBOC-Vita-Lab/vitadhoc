/* NESP: what the wide view (nes/frontend/host.c + widekit) needs from the PPU, recorded without changing emulation.
 * Ours (CBOC-Vita), MIT licence; included by ppu.c (GPL) and host.c.
 *
 * Everything here is write-only from the emulation's point of view: nothing in this file is read by the CPU, PPU, APU or
 * a mapper, nothing is saved in a state, and with nesp_on == 0 the recorder does one test per PPU write and per line. */
#ifndef NESP_WIDE_H
#define NESP_WIDE_H
#include "fceu-types.h"

#define NESP_MAXM 96      /* widest margin per side in pixels */
#define NESP_MBUF 104     /* row length of a margin buffer */
#define NESP_NTP 8        /* distinct physical nametable pages that get write stamps */

enum {
	NESPL_VALID = 1,      /* the line was rendered this frame */
	NESPL_STABLE = 2,     /* nothing that shapes the picture changed during the line (scroll, fine X, BG pattern banks,
	                         nametable mapping, mask/emphasis bits, palette) and background was on */
	NESPL_DRAWN = 4,      /* margin pixels were rendered for this line (nesp_wl / nesp_wr / nesp_wm) */
	NESPL_BLOCK = 8,      /* the board has a PPU fetch hook / MMC5 / PEC-586: margins are not rendered */
	NESPL_BGOFF = 16      /* background rendering was off */
};

typedef struct NespLine {
	const uint8_t *bp[4]; /* base pointers of the four 1 KiB BG pattern pages in force at the start of the line */
	const uint8_t *np[4]; /* nametable page pointers (the mirroring) at the start of the line */
	uint8_t pal[16];      /* PALRAM[0..15] at the start of the line */
	uint16_t v0;          /* loopy address (RefreshAddr) at the start of the line */
	uint8_t x0;           /* fine X at the start */
	uint8_t p0, p1;       /* PPU[0] and PPU[1] at the start */
	uint8_t flags;        /* NESPL_* */
	uint8_t bd;           /* backdrop colour index PALRAM[0] at the end of the line */
} NespLine;

extern int nesp_on;               /* 1: record stamps and lines, render margins (set by the host) */
extern int nesp_margin;           /* px per side to render, 0..NESP_MAXM */
extern uint32_t nesp_frame;       /* counts FCEUPPU_Loop calls (never reset) */
extern NespLine nesp_line[240];
/* Margin pixels are RAW: palette entry as the background renderer makes it (bit 6 set on the backdrop colour), before the
 * grayscale / emphasis pass of the line (apply it with the line's p1 when reading: nes/frontend/wide.c post_px). */
extern uint8_t nesp_wl[240][NESP_MBUF];   /* left margin, index i = 0 is the pixel at x = -nesp_margin, i = margin - 1 is x = -1 */
extern uint8_t nesp_wr[240][NESP_MBUF];   /* right margin, index d = 0 is the pixel at x = 256 */
extern uint8_t nesp_sprm[240];           /* per line: bit k set when a sprite pixel was drawn at x = k (k < 8)  */
extern uint8_t nesp_wm[240][16];          /* the background pixels x = 0..15 (raw) as the line would show them without the left mask
                                             and without sprites */
extern uint8_t nesp_wmr[240][8];          /* the background pixels x = 248..255 (raw), without sprites */

/* nametable write stamps: (frame << 1) | rendering_was_on; indexed by physical page id, offset in the page */
extern uint32_t nesp_ntstamp[NESP_NTP][1024];
extern const uint8_t *nesp_ntptr[NESP_NTP];
extern int nesp_nnt;
int nesp_ntid(const uint8_t *page);       /* physical id of a nametable page pointer (registers new pointers) */
void nesp_clear_stamps(void);
int nesp_plain_fetch(void);               /* 1 when the BG fetch path has no hook (margins can be rendered read only) */
const uint8_t *nesp_chr_base(int *size);  /* first CHR chip, for stable bank identities (NULL for CHR RAM only) */
extern uint32_t nesp_ntgen;               /* counts every change of nesp_ntstamp (write stamp or clear): a copy of the stamps is
                                             current while this is unchanged */
extern uint32_t nesp_ntlast;              /* nesp_frame of the latest stamp write */
extern uint32_t nesp_chrw;                /* bytes written to CHR RAM through $2007 (the host reads and clears it) */
#endif
