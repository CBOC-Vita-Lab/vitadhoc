/*
 * YX5200 MP3 module (cartridge hardware of a few homebrew and hack games): inert stand-in for VitAdhoc.
 * The upstream file decodes MP3 tracks read from files next to the ROM with minimp3. md.bin has no files next to the
 * ROM and does not carry minimp3, so the module is accepted and ignored: a game that uses it runs, without those tracks.
 * Not part of Genesis Plus GX (see md/core/PROVENANCE.md). Copyright CBOC-Vita. MIT licence.
 */
#include "shared.h"

void yx5200_init(int samplerate) { (void)samplerate; }
void yx5200_reset(void) {}
void yx5200_write(unsigned int rx_data) { (void)rx_data; }
void yx5200_update(unsigned int samples) { (void)samples; }
int yx5200_context_save(uint8 *state) { (void)state; return 0; }
int yx5200_context_load(uint8 *state) { (void)state; return 0; }
