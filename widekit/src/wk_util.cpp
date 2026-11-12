/* widekit - MIT licence. */
#include "wk_util.h"

uint32_t wk_crc32(uint32_t crc, const void *data, size_t len)
{
    static uint32_t table[256];
    static int built;
    if (!built) {
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t c = i;
            for (int k = 0; k < 8; k++)
                c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            table[i] = c;
        }
        built = 1;
    }
    const uint8_t *p = (const uint8_t *)data;
    crc = ~crc;
    while (len--)
        crc = table[(crc ^ *p++) & 0xFF] ^ (crc >> 8);
    return ~crc;
}
