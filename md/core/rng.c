/*
 * The core's only source of random values (the position of the 68000 in the video frame after the RESET button).
 * Genesis Plus GX used rand(): its sequence differs between C libraries (glibc, newlib), which would break lockstep
 * between two consoles and the x86 / ARM twins. This is splitmix32 from a seed the host sets.
 * Not part of Genesis Plus GX (see md/core/PROVENANCE.md). Copyright CBOC-Vita. MIT licence.
 */
#include "shared.h"

static uint32_t rng_state = 0x6d645f31u;

void md_rand_seed(uint32_t seed) { rng_state = seed ^ 0x9e3779b9u; }

double md_rand_unit(void)
{
  uint32_t z = (rng_state += 0x9e3779b9u);
  z = (z ^ (z >> 16)) * 0x85ebca6bu;
  z = (z ^ (z >> 13)) * 0xc2b2ae35u;
  z ^= z >> 16;
  return (double)z / 4294967296.0;
}
