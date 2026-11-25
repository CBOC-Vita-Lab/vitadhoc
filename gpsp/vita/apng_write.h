/* SPDX-License-Identifier: MIT OR GPL-2.0-or-later
 * Copyright (c) 2026 VitAdhoc contributors */
/* apng_write.h -- an animated PNG, written a frame at a time.
 *
 * The clip export (clip_export.h) saves the last ten seconds of a game as an
 * APNG: a PNG any viewer opens (it shows the first picture), and that the
 * browsers, Discord, Telegram and most picture viewers play.  The writer is
 * minimal and sits on the PNG writer the app already has: each frame is
 * deflated by stb_image_write's stbi_write_png_to_mem, and its IDAT is taken
 * over as the frame's IDAT (the first) or fdAT (the others).  Nothing else
 * is linked in.
 *
 * A frame after the first only holds the rectangle that changed from the
 * frame before (blend SOURCE, dispose NONE: the picture is the one before
 * with that rectangle replaced), which is most of what a game moves; a
 * frame that changed nothing is one pixel.  Lossless: every frame decodes to
 * exactly the pictures given.
 *
 * The number of frames is told at the start (the acTL chunk comes before the
 * first picture).  Memory: one copy of the previous frame and the one frame
 * being deflated, whatever the length.
 *
 *    apng *a = apng_begin(sink, ctx, w, h, frames);
 *    for each frame:  apng_add(a, rgb, delay_ms);
 *    apng_end(a);                       (also frees; 0 = a whole file)
 *
 * `sink` receives the file's bytes in order and returns 0 to go on.  `rgb` is
 * w * h * 3 bytes, 8 bits a channel.  Portable C: the same file in the
 * twin's tests. */
#ifndef GBADHOC_APNG_WRITE_H
#define GBADHOC_APNG_WRITE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct apng apng;
typedef int (*apng_sink)(void *ctx, const void *data, size_t len);

#define APNG_MAX_W 4096u
#define APNG_MAX_H 4096u

/* NULL = refused (a size of 0 or over APNG_MAX_*, no frames) or no memory.
 * The signature, IHDR and acTL are written at once. */
apng *apng_begin(apng_sink sink, void *ctx, unsigned w, unsigned h,
                 unsigned frames);
/* The next picture, shown for delay_ms (1..65535).  0 = written; -1 = the
 * sink failed, or more frames than were announced (the file is then
 * unusable: call apng_end and throw it away). */
int apng_add(apng *a, const uint8_t *rgb, unsigned delay_ms);
/* IEND, then everything freed.  0 = a whole file (every announced frame was
 * added and the sink never failed). */
int apng_end(apng *a);

/* CRC-32 of PNG (exposed for the tests). */
uint32_t apng_crc32(uint32_t crc, const void *data, size_t len);

#ifdef __cplusplus
}
#endif

#endif /* GBADHOC_APNG_WRITE_H */
