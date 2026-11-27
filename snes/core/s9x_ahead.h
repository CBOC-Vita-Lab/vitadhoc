/*
 * CBOC-Vita local addition (not part of upstream Snes9x). MIT licence.
 * Run-ahead's light snapshot of the machine (ahead.cpp). Between two frames, on the emulating thread:
 *   S9xAheadBegin();     before the real frame (keeps the picture state of the frame drawn last)
 *   ... the real frame, not drawn ...
 *   S9xAheadSave();      the machine as it is now (false: the cartridge is not supported, S9xAheadUnsupported says why)
 *   ... the look-ahead frame, drawn, silent ...
 *   S9xAheadRestore();   back to the saved machine; the caches that follow it are kept valid
 */
#ifndef S9X_AHEAD_H
#define S9X_AHEAD_H

#include <stdint.h>

struct S9xAheadStats
{
	uint64_t saves, restores;
	uint64_t vram_blocks;		/* 32 byte blocks of video RAM the look-ahead frames changed (put back) */
	uint64_t obj_restores;		/* look-ahead frames that rebuilt the sprite tables */
	uint64_t bytes;				/* bytes a save copies (without the sound side and the sprite tables) */
};

const char *S9xAheadUnsupported (void);		/* NULL, or the reason this cartridge cannot be run ahead */
void S9xAheadReset (void);					/* a new game */
void S9xAheadBegin (void);
bool S9xAheadSave (void);
bool S9xAheadRestore (void);
void S9xAheadGetStats (S9xAheadStats *out);

#endif
