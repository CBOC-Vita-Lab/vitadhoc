/* The blocked / NEON picture rotation equals the one-pixel-at-a-time definition on every kind of picture.
 *   cc -I../frontend -o rotate_test twin/tests/rotate_test.c frontend/layout.c && ./rotate_test
 * (also built for ARM with NEON: the transpose in registers is what is checked there)
 * Copyright CBOC-Vita. MIT licence. */
#include <stdio.h>
#include <string.h>
#include "layout.h"
#include "host.h"

int main(void)
{
	static uint16_t src[WSWAN_WIDTH * WSWAN_HEIGHT], a[WSWAN_WIDTH * WSWAN_HEIGHT], b[WSWAN_WIDTH * WSWAN_HEIGHT];
	unsigned rs = 99, kind, i;
	int fails = 0;
	for (kind = 0; kind < 4; kind++) {
		for (i = 0; i < sizeof src / sizeof src[0]; i++) {
			rs = rs * 1664525u + 1013904223u;
			src[i] = kind == 0 ? (uint16_t) i : kind == 1 ? (uint16_t) (rs >> 16) : kind == 2 ? (uint16_t) (i % WSWAN_WIDTH) : (uint16_t) (i / WSWAN_WIDTH * 257u);
		}
		memset(a, 0xA5, sizeof a); memset(b, 0x5A, sizeof b);
		wsl_rotate_ccw(a, src);
		wsl_rotate_ccw_ref(b, src);
		if (memcmp(a, b, sizeof a)) { printf("rotate: kind %u differs\n", kind); fails++; }
	}
	printf("rotate: %d failed\n", fails);
	return fails != 0;
}
