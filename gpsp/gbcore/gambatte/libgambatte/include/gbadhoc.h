/* GBAdhoc additions to libgambatte (see ../LOCAL-PATCHES.md).  GPL-2.0-or-later, as the rest of this directory.
 *
 * gambatte_wall_time: the wall clock the cartridge clocks read (the frontend's hook, UTC seconds; 0 = no reading yet).
 * gambatte_gbc_pixel: a Game Boy Color palette entry (BGR555) as the frontend draws it (RGB565).  Both are defined by
 * gbcore/gambatte_host.cpp. */
#ifndef GBADHOC_GAMBATTE_H
#define GBADHOC_GAMBATTE_H

#include <stdint.h>

extern "C" {
uint64_t gambatte_wall_time(void);
uint16_t gambatte_gbc_pixel(unsigned bgr15);
}

namespace gambatte {

/* The MBC3 clock as the cartridge holds it.  The registers are what the game reads (latched); baseTime and haltTime
 * place the running count on the wall clock. */
struct GbadhocRtc {
	uint64_t baseTime, haltTime;
	unsigned char dh, dl, h, m, s;
	bool hasClock;
};

}

#endif
