/* jit_vita.h -- the Vita's executable memory and heap arrangement.
 *
 *   heap_vita.c  replaces newlib's heap setup.  Before main() it allocates
 *                the kernel VM block (executable memory for the dynarec)
 *                and only THEN the C heap, so the VM block is the first
 *                thing the kernel places after the program itself.
 *   jit_vita.c   opens the VM domain on the emulation thread and carves the
 *                block into one translation-cache region per core instance
 *                (gpsp_vita_jit_region, called by libretro.c's VITA glue).
 *
 * WHY THE ORDER MATTERS.  gpSP's ARM emitter reaches its stubs and C helpers
 * with `bl`, a 24-bit branch: 32 MiB either way.  Stock vitasdk newlib takes
 * its heap (128 MiB by default) before main() runs, so a VM block allocated
 * afterwards can sit 128 MiB from .text and every generated call lands
 * somewhere else.  Allocating the block first puts it directly behind the
 * program; with the program under 16 MiB the far end of a 16 MiB block is
 * still inside 32 MiB.  Nothing here can PROVE where the kernel puts a
 * block, so gbadhoc_mem_report() logs every address and distance, and
 * gpsp_vita_jit_region() refuses a region that is out of reach (the core
 * then runs on its interpreter instead of branching into the weeds).
 */
#ifndef GBADHOC_JIT_VITA_H
#define GBADHOC_JIT_VITA_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* One kernel VM block.  16 MiB is the most sceKernelAllocMemBlockForVM
 * gives in one block. */
#define GBADHOC_VM_BLOCK_BYTES  (16u * 1024u * 1024u)
/* What newlib's malloc gets: see heap_vita.c for the arithmetic. */
#define GBADHOC_HEAP_BYTES      (128u * 1024u * 1024u)
/* Memory left to the kernel's own allocations for this process when the
 * heap has to be sized down (thread stacks, GXM, system modules). */
#define GBADHOC_HEAP_RESERVE    (32u * 1024u * 1024u)

/* Filled in by heap_vita.c before main(); read-only afterwards. */
typedef struct gbadhoc_mem_layout
{
   int      vm_uid;            /* < 0: the allocation failed (the error)   */
   uint8_t *vm_base;
   uint32_t vm_bytes;
   int      heap_uid;
   uint8_t *heap_base;
   uint32_t heap_bytes;        /* what the heap actually got               */
   uint32_t heap_wanted;       /* GBADHOC_HEAP_BYTES                       */
   uint32_t free_user_boot;    /* free user memory before either block     */
   uint32_t free_user_after;   /* ...and after both                        */
} gbadhoc_mem_layout;
extern gbadhoc_mem_layout gbadhoc_mem;

/* The C heap's current break, for the report (heap_vita.c). */
uint32_t gbadhoc_heap_used(void);

/* Executable memory for one core instance; see libretro/libretro.c.
 * Returns the VM block's UID, or a negative value (no block, no room, or
 * out of branch reach). */
int gpsp_vita_jit_region(const void *key, unsigned bytes, void **base);

/* A QUESTION FOR A CONSOLE (vita.ini vm_probe, plat_vita.c): may a thread
 * other than the one that opened the VM domain run translated code, and
 * write it?  A second machine on a thread of its own -- the wide view's
 * scout at full speed, beside the game instead of in its idle time --
 * depends on the answer, and no emulator of the console gives it.  A few
 * instructions in a spare page of the VM block, one step per call:
 *   0  (the first thread) take the page, write "return 41", publish, run;
 *   1  run what step 0 wrote                 (on the second thread);
 *   2  sceKernelOpenVMDomain()               (on the second thread);
 *   3  write "return 43", publish, run       (on the second thread);
 *   4  write "return 44", publish, run       (the first thread again).
 * *value: what the code returned, or step 2's result.  < 0: no page. */
int gpsp_vita_jit_probe(int step, int *value);

/* One line per fact through `line` (printf-style): program segments, VM
 * block, heap, every region handed out and its distance from the code. */
void gbadhoc_mem_report(void (*line)(const char *fmt, ...));

#ifdef __cplusplus
}
#endif

#endif /* GBADHOC_JIT_VITA_H */
