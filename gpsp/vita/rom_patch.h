/* rom_patch.h -- ROM hacks and translations: IPS, BPS and UPS patches.
 *
 * A patch beside a game -- Game.ips, Game.bps or Game.ups next to Game.gba
 * (the first of the three that is there) -- is applied as the game starts.
 * The game's own file is never changed: the patched cartridge is written to
 * <data>/patched/ once and used from there while neither the game nor the
 * patch changes (main_vita.c patch_prepare).  The battery save and the
 * states stay the game's, named after its file, as with any soft patch.
 *
 *   IPS  records of (offset, bytes) or (offset, run of one byte); an "EOF"
 *        trailer, optionally followed by the size to cut the result to.  No
 *        checksums: it is applied to whatever file it sits beside.
 *   BPS  beat's format: the source's and the result's sizes and CRC32s and
 *        the patch's own CRC32, so a patch made for another release of the
 *        game is refused rather than applied to the wrong bytes.
 *   UPS  sizes and CRC32s as well; applied either way round (a UPS patch
 *        also turns its result back into its source).
 *
 * Pure functions on memory: no files, no heap of their own. */
#ifndef ROM_PATCH_H
#define ROM_PATCH_H

#include <stddef.h>
#include <stdint.h>

enum { ROM_PATCH_NONE = 0, ROM_PATCH_IPS = 1, ROM_PATCH_BPS = 2,
       ROM_PATCH_UPS = 3 };

/* The kind a patch's first bytes say it is (0 = none of the three). */
int rom_patch_kind(const uint8_t *patch, size_t plen);

/* The size of the result of applying `patch` to a source of `slen` bytes,
 * or 0 when the patch is malformed (`why` says how, when given). */
size_t rom_patch_size(const uint8_t *patch, size_t plen, size_t slen,
                      const char **why);

/* Apply: dst (dcap >= rom_patch_size) receives the result; *dlen its size.
 * 0 = applied; -1 = refused, with the reason in *why ("not a patch",
 * "made for another version of the game", "damaged", "too large"). */
int rom_patch_apply(const uint8_t *patch, size_t plen,
                    const uint8_t *src, size_t slen,
                    uint8_t *dst, size_t dcap, size_t *dlen,
                    const char **why);

#endif
