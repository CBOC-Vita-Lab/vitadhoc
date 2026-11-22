/* interp_stub.c -- the recompiler's symbols the frontend names, for a twin
 * built without a recompiler (DYNAREC=0: ARCH=aarch64, or ARCH=x86 DYNAREC=0).
 *
 * cpu_threaded.c (the recompiler) is not compiled there, but the frontend
 * reads its counters and sets its switches, and the memory report reads the
 * translation cache pointer.  Each has the value "no recompiler": nothing
 * translated, no emergencies, no cache, setters ignored.  Instance B (the
 * gbab_ copy of the core) never references these, so they are not renamed.
 * Compiled only when DYNAREC=0 (hostcore/Makefile).
 */
#include <stdint.h>

uint8_t *rom_translation_cache;
uint32_t smc_partial_emergencies;

uint32_t gpsp_jit_counters(uint32_t *out, uint32_t n)
{
   uint32_t i;
   for (i = 0; i < n; i++)
      out[i] = 0;
   return 0;
}

uint32_t gpsp_jit_worst_line(char *buf, uint32_t cap)
{
   if (cap)
      buf[0] = 0;
   return 0;
}

void gpsp_jit_lazy_set(uint32_t on, uint32_t budget)
{
   (void)on;
   (void)budget;
}

void gpsp_smc_partial_set(uint32_t on)
{
   (void)on;
}

uint32_t gpsp_smc_partial_active(void)
{
   return 0;
}
