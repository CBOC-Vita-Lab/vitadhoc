/* fe_core_shim.c -- what frontend-common/fe_host.c's GBA link code expects
 * from a PSP core and a Vita core does not have.
 *
 * fe_host.c section 19 ("a cable session owns instance B's memory") reloads
 * instance A around a session and, for that, names four things that exist
 * only in a RUNTIME_JIT_CACHE build of the core (cpu_threaded.c): the
 * translation-cache tier, its replay flag, the release function and the
 * static SMALL array used as scratch.  The Vita never takes that path -- it
 * boots with gba_dual_late = 0, B always loaded (main_vita.c) -- but the
 * references are unconditional, so the link needs the names.  They are
 * defined here, with the values that make the unused path harmless, rather
 * than by putting #ifdef VITA into a file the PSP and desktop builds share.
 *
 * (If late mode is ever wanted on the Vita, these become real: the tier is
 * "LARGE" by construction and release has nothing to free, because
 * vita/jit_vita.c hands each core instance the same region again.)
 */
#include <stdint.h>

/* 2 = JIT_CACHE_LARGE: gba_scratch_get() then takes its scratch from the
 * heap and never touches rom_translation_cache_static. */
uint32_t jit_cache_tier = 2;
uint32_t jit_cache_replay;
uint8_t  rom_translation_cache_static[64];

void dynarec_release_translation_caches(void);
void dynarec_release_translation_caches(void)
{
}
