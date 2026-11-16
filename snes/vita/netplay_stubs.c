/* The part of the mirror session (fe_gblink.c) that drives two Game Boys. A SNES link never runs it (fe_gblink_local.machines
 * is always set), so these answer "no machine" and keep gbcore / TGB Dual (GPL) out of snes.bin.
 * Copyright CBOC-Vita. MIT licence for this file (see snes/frontend/LICENSE). */
#include "gbcore_dual.h"

gbdual_t *gbdual_create(const gbdual_config_t *config) { (void) config; return 0; }
void gbdual_destroy(gbdual_t *dual) { (void) dual; }
int gbdual_advance(gbdual_t *dual, gbdual_input_fn input, void *userdata) { (void) dual; (void) input; (void) userdata; return -1; }
uint64_t gbdual_sync_hash(gbdual_t *dual) { (void) dual; return 0; }
gbcore_t *gbdual_core(const gbdual_t *dual, int slot) { (void) dual; (void) slot; return 0; }
const gbcore_api_t *gbdual_api(int slot) { (void) slot; return 0; }
uint8_t *gbcore_rom_alloc(size_t rom_size, size_t *alloc) { (void) rom_size; if (alloc) *alloc = 0; return 0; }
