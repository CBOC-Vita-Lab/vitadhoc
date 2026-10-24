/* jit_vita.c -- executable memory for the dynarec on the PS Vita.
 * jit_vita.h says why the block is where it is; this file hands it out.
 *
 * THE VITA'S RULES for self-written code, as far as homebrew has established
 * them:
 *   - executable memory comes from sceKernelAllocMemBlockForVM (16 MiB at
 *     most per block); heap_vita.c took one before main();
 *   - sceKernelOpenVMDomain() makes it writable AND executable -- for the
 *     calling thread.  Code must be emitted on the thread that opened the
 *     domain.  Here that is the emulation thread: fe_host boots both core
 *     instances on it and every translation happens inside retro_run;
 *   - after writing, sceKernelSyncVMDomain(uid, start, len) publishes the
 *     range to the instruction side (cpu_threaded.c platform_cache_sync).
 *
 * TWO CORE INSTANCES, ONE BLOCK.  A GBA_LINK build runs the player's machine
 * (instance A) and the link partner's (instance B, the same object code with
 * every global renamed gbab_*).  B's references to anything outside the core
 * are not renamed, so both instances call the one gpsp_vita_jit_region()
 * below and tell themselves apart by the key they pass.  Each key gets its
 * own slice of the block, and gets the same slice again if it asks again.
 * With the Vita sizes in gpsp_config.h (7 MiB + 512 KiB per instance) the
 * pair takes 15 MiB of the 16.
 *
 * A request that does not fit the first block gets a second VM block.  That
 * one is allocated now, long after the heap, so it is far from the program;
 * it is only usable when the emitter reaches its stubs through veneers
 * (-DARM_FAR_STUBS).  Without them the reach check below turns it down.
 */
#include <stddef.h>
#include <psp2/kernel/sysmem.h>
#include <psp2/kernel/threadmgr.h>

#include "jit_vita.h"

/* Linker symbols: the program's code, and the end of its data. */
extern char __executable_start[], etext[], _end[];

/* A 24-bit ARM branch reaches +-32 MiB; keep a page of margin for the
 * pc+8 bias and the last instruction of a block. */
#define BRANCH_REACH  (32u * 1024u * 1024u - 4096u)

#define MAX_REGIONS 4
static struct
{
   const void *key;
   uint8_t    *at;            /* where the memory is                       */
   uint8_t    *base;          /* == at, or NULL: refused as out of reach
                               * (kept so it is not asked for again)       */
   uint32_t    bytes;
   int         uid;
   uint32_t    far_bytes;     /* farthest distance to any program code     */
} g_regions[MAX_REGIONS];
static unsigned g_nregions;

static uint32_t g_used;       /* bytes handed out of the first block */
static int      g_domain = -1;        /* sceKernelOpenVMDomain's result  */
static int      g_domain_thread;      /* ...and the thread it was for    */

/* The farthest any byte of [base, base+bytes) is from any byte of .text. */
static uint32_t farthest_from_code(const uint8_t *base, uint32_t bytes)
{
   uintptr_t lo = (uintptr_t)__executable_start, hi = (uintptr_t)etext;
   uintptr_t b = (uintptr_t)base, e = b + bytes;
   uintptr_t d1 = e > lo ? e - lo : 0;       /* code start to region end   */
   uintptr_t d2 = hi > b ? hi - b : 0;       /* region start to code end   */
   return (uint32_t)(d1 > d2 ? d1 : d2);
}

static int in_reach(uint32_t far_bytes)
{
#ifdef ARM_FAR_STUBS
   (void)far_bytes;
   return 1;
#else
   return far_bytes < BRANCH_REACH;
#endif
}

int gpsp_vita_jit_region(const void *key, unsigned bytes, void **base)
{
   unsigned i;
   uint8_t *at = NULL;
   int uid = -1;

   *base = NULL;
   for (i = 0; i < g_nregions; i++)
      if (g_regions[i].key == key)
      {
         /* The same instance again (a reload, or B put back to a fresh
          * boot): the same memory.  A larger request than last time is a
          * different build of the core and cannot happen in one process. */
         if (!g_regions[i].base || bytes > g_regions[i].bytes)
            return -1;
         *base = g_regions[i].base;
         return g_regions[i].uid;
      }
   if (g_nregions == MAX_REGIONS)
      return -1;

   /* Once, on the thread that will emit the code. */
   if (g_domain < 0 && gbadhoc_mem.vm_uid >= 0)
   {
      g_domain = sceKernelOpenVMDomain();
      g_domain_thread = sceKernelGetThreadId();
      if (g_domain < 0)
         g_domain = 0;        /* reported; some firmware returns an error
                               * for a domain that is already usable */
   }

   bytes = (bytes + 4095u) & ~4095u;
   if (gbadhoc_mem.vm_uid >= 0 && g_used + bytes <= gbadhoc_mem.vm_bytes)
   {
      at = gbadhoc_mem.vm_base + g_used;
      uid = gbadhoc_mem.vm_uid;
      g_used += bytes;
   }
   else
   {
      /* Does not fit the first block: a block of its own. */
      void *p = NULL;
      uint32_t blk = (bytes + 0xFFFFFu) & ~0xFFFFFu;
      uid = sceKernelAllocMemBlockForVM("gbadhoc_jit2", blk);
      if (uid >= 0 && sceKernelGetMemBlockBase(uid, &p) >= 0 && p)
         at = (uint8_t *)p;
      else if (uid >= 0)
      {
         sceKernelFreeMemBlock(uid);
         uid = -1;
      }
   }

   g_regions[g_nregions].key   = key;
   g_regions[g_nregions].bytes = bytes;
   g_regions[g_nregions].uid   = uid;
   g_regions[g_nregions].at    = at;
   g_regions[g_nregions].base  = NULL;
   g_regions[g_nregions].far_bytes = at ? farthest_from_code(at, bytes) : 0;
   if (at && in_reach(g_regions[g_nregions].far_bytes))
      g_regions[g_nregions].base = at;
   g_nregions++;

   if (!g_regions[g_nregions - 1].base)
      return -1;              /* the caller falls back to the interpreter */
   *base = at;
   return uid;
}

void gbadhoc_mem_report(void (*line)(const char *fmt, ...))
{
   SceKernelFreeMemorySizeInfo info;
   unsigned i;
   uintptr_t text = (uintptr_t)__executable_start, text_end = (uintptr_t)etext;
   uintptr_t image_end = (uintptr_t)_end;

   info.size = sizeof(info);
   info.size_user = info.size_cdram = info.size_phycont = 0;
   sceKernelGetFreeMemorySize(&info);

   line("mem_image text=%08x..%08x end=%08x text_kib=%u image_kib=%u",
        (unsigned)text, (unsigned)text_end, (unsigned)image_end,
        (unsigned)((text_end - text) >> 10), (unsigned)((image_end - text) >> 10));
   line("mem_vm uid=%08x base=%08x bytes=%u used=%u after_image_kib=%d "
        "domain=%d thread=%08x",
        (unsigned)gbadhoc_mem.vm_uid, (unsigned)(uintptr_t)gbadhoc_mem.vm_base,
        (unsigned)gbadhoc_mem.vm_bytes, (unsigned)g_used,
        gbadhoc_mem.vm_base
           ? (int)(((intptr_t)(uintptr_t)gbadhoc_mem.vm_base - (intptr_t)image_end) / 1024)
           : 0,
        g_domain, (unsigned)g_domain_thread);
   line("mem_heap uid=%08x base=%08x bytes=%u wanted=%u used=%u "
        "after_vm_kib=%d",
        (unsigned)gbadhoc_mem.heap_uid, (unsigned)(uintptr_t)gbadhoc_mem.heap_base,
        (unsigned)gbadhoc_mem.heap_bytes, (unsigned)gbadhoc_mem.heap_wanted,
        (unsigned)gbadhoc_heap_used(),
        (gbadhoc_mem.heap_base && gbadhoc_mem.vm_base)
           ? (int)(((intptr_t)(uintptr_t)gbadhoc_mem.heap_base -
                    (intptr_t)(uintptr_t)(gbadhoc_mem.vm_base + gbadhoc_mem.vm_bytes)) / 1024)
           : 0);
   line("mem_free user_boot=%u user_after_heap=%u user_now=%u cdram_now=%u "
        "phycont_now=%u",
        (unsigned)gbadhoc_mem.free_user_boot, (unsigned)gbadhoc_mem.free_user_after,
        (unsigned)info.size_user, (unsigned)info.size_cdram,
        (unsigned)info.size_phycont);
   for (i = 0; i < g_nregions; i++)
      line("mem_jit n=%u at=%08x bytes=%u uid=%08x far_kib=%u reach_kib=%u "
           "far_stubs=%d ok=%d",
           i, (unsigned)(uintptr_t)g_regions[i].at, (unsigned)g_regions[i].bytes,
           (unsigned)g_regions[i].uid, (unsigned)(g_regions[i].far_bytes >> 10),
           (unsigned)(BRANCH_REACH >> 10),
#ifdef ARM_FAR_STUBS
           1,
#else
           0,
#endif
           g_regions[i].base != NULL);
}

int gpsp_vita_jit_probe(int step, int *value)
{
   static int      key;
   static uint8_t *page;
   static int      uid = -1;
   uint32_t *w;
   int ret = step == 0 ? 41 : step == 3 ? 43 : 44;

   *value = 0;
   if (step == 0 && !page)
   {
      void *p = NULL;
      uid = gpsp_vita_jit_region(&key, 4096, &p);
      page = (uint8_t *)p;
   }
   if (!page || uid < 0)
      return -1;
   if (step == 2)
   {
      *value = sceKernelOpenVMDomain();
      return 0;
   }
   if (step == 1)
   {
      *value = ((int (*)(void))(void *)page)();
      return 0;
   }
   w = (uint32_t *)(void *)(page + (step == 0 ? 0 : step == 3 ? 64 : 128));
   w[0] = 0xE3A00000u | (uint32_t)ret;       /* mov r0, #ret */
   w[1] = 0xE12FFF1Eu;                       /* bx  lr       */
   sceKernelSyncVMDomain(uid, w, 8);
   *value = ((int (*)(void))(void *)w)();
   return 0;
}
