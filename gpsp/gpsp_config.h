
#ifndef GPSP_CONFIG_H
#define GPSP_CONFIG_H

#define GPSP_NAME                "gpSP"
#define GPSP_VERSION             "v1.1.0"
#define GPSP_NETPACKET_VERSION   "gpSP v1.0"

/* Default ROM buffer size in megabytes (this is a maximum value!) */
#ifndef ROM_BUFFER_SIZE
#define ROM_BUFFER_SIZE 32
#endif

/* Cache sizes and their config knobs */
#if defined(SMALL_TRANSLATION_CACHE)
  #define ROM_TRANSLATION_CACHE_SIZE (1024 * 1024 * 2)
  /* ONE RAM code cache size on every tier and on both cores of a link
   * session: two copies of the same machine then fill -- and, with
   * ENTRY_GATES, gate -- at the same moment whatever tier each one got.
   * 384 KiB, not 512: this array is static in both link cores, and instance
   * B's state is padded to whole MiB blocks (psp/gbalink_core_b.ld) with
   * under 128 KiB to spare, so 512 here cost a PSP-1000 1.2 MiB of image. */
  #define RAM_TRANSLATION_CACHE_SIZE (1024 * 384)
#elif defined(VITA) && defined(GBA_LINK)
  /* PS Vita, two core instances (the link mirror).  Translation caches come
   * out of ONE kernel VM block (vita/jit_vita.c), and a VM block is at most
   * 16 MiB.  The stock 10 MiB + 512 KiB twice is 21 MiB, so each instance
   * gets 7 MiB + 512 KiB: 15 MiB for the pair, 1 MiB of the block to spare.
   *
   * Why not a second block for instance B and the stock size each: the ARM
   * emitter calls its stubs with a 24-bit `bl` (reach 32 MiB).  One block
   * right behind the program keeps every cache within reach of both
   * instances' code; a second 16 MiB block starts at least 16 MiB further
   * out, and whichever instance owns it is then out of reach by
   * construction.  jit_vita.c does take a second block when a build asks for
   * more than fits (the -D overrides below, with ARM_FAR_STUBS), and logs the
   * distances either way.
   *
   * 7 MiB is 3.5x the PSP-1000's whole tier (2 MiB), which runs the entire
   * library; the 10 MiB tier exists for CFRU-class ROM hacks that flush-storm
   * a small cache.  Both sizes can be set from the build (-D). */
  #ifndef ROM_TRANSLATION_CACHE_SIZE
  #define ROM_TRANSLATION_CACHE_SIZE (1024 * 1024 * 7)
  #endif
  #ifndef RAM_TRANSLATION_CACHE_SIZE
  #define RAM_TRANSLATION_CACHE_SIZE (1024 * 512)
  #endif
#else
  #define ROM_TRANSLATION_CACHE_SIZE (1024 * 1024 * 10)
  #define RAM_TRANSLATION_CACHE_SIZE (1024 * 512)
#endif

/* RUNTIME_JIT_CACHE (PSP): the sizes above are the SMALL tier, always present
 * as static arrays.  A console whose heap can afford it gets these instead,
 * allocated at startup (cpu_threaded.c, dynarec_select_translation_caches).
 * They are the sizes the old BIG_JIT=1 build compiled in. */
#if defined(RUNTIME_JIT_CACHE)
  #define ROM_TRANSLATION_CACHE_SIZE_LARGE (1024 * 1024 * 10)
  #define RAM_TRANSLATION_CACHE_SIZE_LARGE (1024 * 384)   /* = the SMALL tier's, see above */
#endif

/* LAYOUT_PIN (PSP, OPT-IN: defined only by `make LAYOUT_PIN=1`, off by
 * default; docs/LAYOUT-PINNING.md): where the translation caches sit
 * in the Allegrex instruction cache.  It is 16 KiB, 2-way, with 64 B lines
 * (measured; docs/CACHE-MAP.md), so a line's set is address bits 6..12 and
 * only (address mod 8 KiB) decides what it competes with.  The emitter writes
 * its hottest code -- the memory-access stubs every guest load and store goes
 * through -- at the START of the ROM cache (mips_emit.h init_emitter), so the
 * cache's base decides whether those stubs fight the dispatcher and
 * update_gba.  Unpinned, the base was wherever the heap (LARGE tier) or .bss
 * (SMALL tier) happened to put it, and moved with every unrelated change.
 * Pinned, both tiers start at JIT_PIN_OFFSET mod JIT_PIN_WAY: the sets the
 * pinned static hot path (psp/layout/hot.ord) leaves free, chosen by the
 * twin's cache simulation.  Costs at most JIT_PIN_WAY + JIT_PIN_OFFSET bytes
 * of alignment per tier. */
#if defined(LAYOUT_PIN)
  #define JIT_PIN_WAY 8192
  #ifndef JIT_PIN_OFFSET
  #define JIT_PIN_OFFSET 0x1500
  #endif
#endif

/* Should be an upperbound to the maximum number of bytes a single JIT'ed
   instruction can take. STM/LDM are tipically the biggest ones */
#define TRANSLATION_CACHE_LIMIT_THRESHOLD (1024 * 2)

/* Hash table size for ROM trans cache lookups */
#define ROM_BRANCH_HASH_BITS                           16
#define ROM_BRANCH_HASH_SIZE   (1 << ROM_BRANCH_HASH_BITS)

/* RFU Multiplayer config, do not mess around too much with it */
#define MAX_RFU_NETPLAYERS       32

/* Serial modes (multiplayer serial). */
#define MAX_SERMULT_NETPLAYERS    4

#endif
