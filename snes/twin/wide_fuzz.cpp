/*
 * Test only: random PPU register / VRAM / CGRAM / OAM writes between frames for the margin strip cache proof (wide_twin --fuzz SEED).
 * The writes go through the PPU's own register entry point, as a game's would, so every input of the renderer can change. They change what the
 * game does; the proof compares the cache against a fresh draw within the same run (wide_cache_verify). Every random number is drawn into its own variable, so the stream is the same under any compiler (x86 and ARM runs can be compared).
 * Copyright CBOC-Vita. MIT licence.
 */
#include <stdint.h>
#include "snes9x.h"
#include "ppu.h"
#include "memmap.h"
#include "wide_fuzz.h"

static uint64_t g_s;
static uint32_t rnd(void)
{
	g_s ^= g_s << 13; g_s ^= g_s >> 7; g_s ^= g_s << 17;
	return (uint32_t) (g_s >> 16);
}
static void W(uint16_t a, uint8_t b) { S9xSetPPU(b, a); }

void wide_fuzz_seed(unsigned seed) { g_s = 0x9E3779B97F4A7C15ull ^ ((uint64_t) seed * 0x2545F4914F6CDD1Dull) ^ 1; for (int i = 0; i < 8; i++) rnd(); }

/* the hook of cpuexec.cpp (CBOC_TEST_LINE_HOOK, built into snes_wide_twin only): called before each visible line is drawn */
void (*g_s9x_test_line_hook)(int) = 0;
static unsigned long g_mid_calls, g_mid_writes;
static void one_change(bool mid);

static void mid_hook(int line)
{
	(void) line;
	g_mid_calls++;
	if (rnd() % 120) return;		// about two changes in a 224 line frame
	g_mid_writes++;
	one_change(true);
}
void wide_fuzz_mid(int on) { g_s9x_test_line_hook = on ? mid_hook : 0; }
unsigned long wide_fuzz_mid_calls(void) { return g_mid_calls; }
unsigned long wide_fuzz_mid_writes(void) { return g_mid_writes; }

void wide_fuzz_frame(int level)
{
	// level 1: a change in about one frame of three; 2: nearly every frame, several changes
	if (level < 2 && rnd() % 3) return;
	const int n = level < 2 ? 1 : 1 + rnd() % 3;
	for (int k = 0; k < n; k++)
		one_change(false);
}

/* mid: a write between lines. Not $2133 interlace / overscan there: the picture pitch and height change in the middle of a frame, and the Mode 7 mosaic layer
 * (DrawTileMosaic, unchanged Snes9x code) then writes outside the screen buffer (Valgrind): a finding, hardware applies those bits at the next frame, not part of this proof */
static void one_change(bool mid)
{
	{
		unsigned op = rnd() % 19;
		switch (op)
		{
		case 0: { const uint32_t m = rnd() % 3, b = rnd() & 0xff, k = rnd() % 2, j = rnd() % 2; W(0x2105, m == 0 ? (uint8_t) b : (uint8_t) ((b & 0xf8) | (k ? 1 : (j ? 0 : 3)))); break; }
		case 1: { const uint32_t r = rnd() % 4, v = rnd() & 0xff; W(0x2107 + r, (uint8_t) v); break; }
		case 2: { const uint32_t r = rnd() % 2, v = rnd() & 0xff; W(0x210b + r, (uint8_t) v); break; }
		case 3: { const uint32_t q = rnd() % 4, h = rnd() % 2, v1 = rnd() & 0xff, v2 = rnd() % 4; const uint16_t r = 0x210d + 2 * q + h; W(r, (uint8_t) v1); W(r, (uint8_t) v2); break; }
		case 4: W(0x2101, (uint8_t) rnd()); break;
		case 5: { const uint32_t r = rnd() % 9, v = rnd() & 0xff; W(0x2123 + r, (uint8_t) v); break; }
		case 6: { const uint32_t r1 = rnd() % 2, v1 = rnd() & 0xff, r2 = rnd() % 2, v2 = rnd() & 0xff; W(0x212c + r1, (uint8_t) v1); W(0x212e + r2, (uint8_t) v2); break; }
		case 7: { const uint32_t r = rnd() % 3, v = rnd() & 0xff; W(0x2130 + r, (uint8_t) v); break; }
		case 8: W(0x2133, (uint8_t) (rnd() & 0xc0)); break;
		case 9:	// VRAM: bytes inside the tile maps and character areas the BGs and sprites use, and anywhere
		{
			uint32_t a;
			const uint32_t kind = rnd() % 4, bg = rnd() % 4, off = rnd();
			switch (kind)
			{
			case 0: a = (uint32_t) PPU.BG[bg].SCBase + off % 0x1000; break;
			case 1: a = ((uint32_t) PPU.BG[bg].NameBase << 0) + off % 0x4000; break;
			case 2: a = ((uint32_t) PPU.OBJNameBase >> 1) + off % 0x2000; break;
			default: a = off & 0x7fff; break;
			}
			W(0x2115, 0x80); W(0x2116, (uint8_t) a); W(0x2117, (uint8_t) (a >> 8));
			for (int i = 1 + rnd() % 8; i > 0; i--) { const uint32_t lo = rnd() & 0xff, hi = rnd() & 0xff; W(0x2118, (uint8_t) lo); W(0x2119, (uint8_t) hi); }
			break;
		}
		case 10: { const uint32_t c = rnd() & 0xff; W(0x2121, (uint8_t) c); for (int i = 1 + rnd() % 3; i > 0; i--) { const uint32_t lo = rnd() & 0xff, hi = rnd() & 0x7f; W(0x2122, (uint8_t) lo); W(0x2122, (uint8_t) hi); } break; }
		case 11:	// OAM: a sprite at the left or right edge, or anywhere
		{
			const uint32_t s = rnd() % 128, c = rnd() % 3, e = rnd() % 24, f = rnd() % 2, g = rnd() % 24, h = rnd() & 0xff;
			const uint32_t x = (c == 0) ? 256 - 1 - e : (f ? g : h);
			const uint32_t y = rnd() % 224, t = rnd() & 0xff, at = rnd() & 0xff, hb = rnd() & 0xff;
			W(0x2102, (uint8_t) (s * 2)); W(0x2103, 0);
			W(0x2104, (uint8_t) x); W(0x2104, (uint8_t) y);
			W(0x2104, (uint8_t) t); W(0x2104, (uint8_t) at);
			W(0x2102, (uint8_t) (0 + (s >> 4) * 2 + 0)); W(0x2103, 1);
			W(0x2104, (uint8_t) hb);
			break;
		}
		case 12: W(0x2132, (uint8_t) rnd()); break;
		case 14: { const uint32_t v = rnd() & 0xff; W(0x2133, (uint8_t) (v & (mid ? 0x48 : 0x4f))); break; }	// interlace, OBJ interlace, overscan (between frames only: the picture pitch and height change), pseudo-hires, EXTBG
		case 15: { const uint32_t v = rnd() & 0xff, q = rnd() % 4; W(0x2100, (uint8_t) (q ? 0x0f : (v & 0x8f))); break; }	// forced blank and brightness
		case 16: { const uint32_t v = rnd() & 0xff; W(0x2106, (uint8_t) v); break; }	// mosaic
		case 17: { const uint32_t a = rnd() & 0xff, h = rnd() % 2; W(0x2102, (uint8_t) a); W(0x2103, (uint8_t) (0x80 | h)); break; }	// OAM priority rotation (first sprite = the one the address names)
		case 18: { const uint32_t v = rnd() & 0xff; W(0x2105, (uint8_t) ((v & 0xf0) | 0x09)); W(0x2133, (uint8_t) (rnd() & 0x08)); break; }	// BG3 priority bit, pseudo-hires
		default: { const uint32_t v0 = rnd() & 0xff, q = rnd() % 6, v1 = rnd() & 0xff, v2 = rnd() & 0xff; W(0x211a, (uint8_t) v0); const uint16_t r = 0x211b + q; W(r, (uint8_t) v1); W(r, (uint8_t) v2); break; }
		}
	}
}
