/* See ahead_glue.h.  playerkit's pk_ahead.h governor (header only, MIT). */
#include "ahead_glue.h"
#include "../../playerkit/src/pk_ahead.h"
#include <stdio.h>

static pk::AheadGovernor g_gov;

extern "C" {

void ahead_begin(int mode, uint32_t frame_us, int ratio_x100, uint32_t overhead_us)
{
	pk::AheadConfig c;
	if (frame_us) c.frame_us = frame_us;
	if (ratio_x100 > 0) c.ratio_x100 = ratio_x100;
	c.overhead_us = overhead_us;
	g_gov.configure(c);
	g_gov.set_mode(mode);
}

void ahead_set_mode(int mode) { g_gov.set_mode(mode); }
int ahead_mode(void) { return g_gov.mode(); }

int ahead_decide(int blocked)
{
	g_gov.set_blocked(blocked != 0);
	return g_gov.want() ? 1 : 0;
}

void ahead_cost(uint32_t emu_us, int ran_ahead) { g_gov.add(emu_us, ran_ahead != 0); }

int ahead_on(void) { return g_gov.on() ? 1 : 0; }

int ahead_census_line(char *buf, size_t n)
{
	const int m = g_gov.mode();
	if (m == AHEAD_MODE_OFF && !g_gov.frames_ahead())
		return 0;
	snprintf(buf, n, "ahead: mode=%s on=%d frames=%u/%u plain_ms=%.2f ahead_ms=%.2f ratio=%.2f starts=%u stops=%u",
	         m == AHEAD_MODE_OFF ? "off" : m == AHEAD_MODE_AUTO ? "auto" : "on", g_gov.want() ? 1 : 0, g_gov.frames_ahead(), g_gov.frames(),
	         g_gov.plain_us() / 1000.0, g_gov.ahead_us() / 1000.0, g_gov.ratio_x100() / 100.0, g_gov.starts(), g_gov.stops());
	g_gov.census_reset();
	return 1;
}

}
