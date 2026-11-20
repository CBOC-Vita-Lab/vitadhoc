/* SPDX-License-Identifier: MIT
 * pk_link_net_shim.c: the one symbol transport_lan.c needs that lives in the main app's session driver.
 *
 * transport_lan.c re-checksums a netdrv roster frame after it rewrites an address in it (the NAT alias of the main
 * app's Game Boy / GBA link), and calls nd_crc16 for that. A player (snes.bin, md.bin) never carries a netdrv frame:
 * pk_link uses the transport's datagram lane only, so that code is never reached. The symbol must still resolve, and
 * the player must not link the session driver (a third-party GPL file) to get it, so here is a plain CRC-16/CCITT of
 * our own, with the same name. Copyright CBOC-Vita. MIT licence (see playerkit/LICENSE). */
#include <stddef.h>
#include <stdint.h>

uint16_t nd_crc16(uint16_t crc, const void *data, size_t len)
{
	const uint8_t *p = (const uint8_t *) data;
	while (len--) {
		crc ^= (uint16_t) ((uint16_t) *p++ << 8);
		for (int i = 0; i < 8; i++) crc = (uint16_t) ((crc & 0x8000) ? (crc << 1) ^ 0x1021 : crc << 1);
	}
	return crc;
}
