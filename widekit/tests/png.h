/* widekit tests - MIT licence.  Minimal PNG writer (stored deflate). */
#ifndef WK_TEST_PNG_H
#define WK_TEST_PNG_H
#include <stdint.h>
#include <vector>

// rgb: w*h*3 bytes.  Returns false on I/O error.
bool png_write(const char *path, int w, int h, const std::vector<uint8_t> &rgb);

static inline void rgb565_to_rgb(uint16_t p, uint8_t *o)
{
    o[0] = (uint8_t)(((p >> 11) & 31) * 255 / 31);
    o[1] = (uint8_t)(((p >> 5) & 63) * 255 / 63);
    o[2] = (uint8_t)((p & 31) * 255 / 31);
}
#endif
