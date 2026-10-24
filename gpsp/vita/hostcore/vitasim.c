/* vitasim.c -- the Vita's VM-block calls on Linux, for the VITASIM twin.
 *
 * `make ARCH=armhf VITASIM=1` builds the GBA core with -DVITA, so the code
 * that runs under qemu-arm is the Vita's own memory path and not the Linux
 * one: libretro.c's VITA glue asks vita/jit_vita.c for a region, both core
 * instances are carved out of ONE 16 MiB block with the Vita cache sizes
 * (gpsp_config.h), jit_vita.c's branch-reach check runs against the real
 * distances, and every publish goes through platform_cache_sync ->
 * sceKernelSyncVMDomain.  What is simulated is only the kernel: a "VM block"
 * is an anonymous RWX mapping, placed just behind the program the way
 * heap_vita.c arranges for the first block on a console (later blocks land
 * wherever mmap puts them, as a late block does there), and
 * sceKernelSyncVMDomain is the compiler's cache flush.
 *
 * It cannot say where a real Vita kernel places a block, nor whether its VM
 * domain behaves as documented.  It does prove the carving, the keying of
 * regions by instance, the sync calls' arguments (a wrong block id or a
 * range outside the block is fatal here) and the reach arithmetic.
 */
#define _GNU_SOURCE
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>

#include <psp2/kernel/sysmem.h>
#include <psp2/kernel/threadmgr.h>
#include "../jit_vita.h"

extern char _end[];

#define MAX_BLOCKS 4
static struct { uint8_t *base; uint32_t bytes; } blocks[MAX_BLOCKS];
static int nblocks;
static int domain_open;

gbadhoc_mem_layout gbadhoc_mem = { -1, NULL, 0, -1, NULL, 0, 0, 0, 0 };

SceUID sceKernelAllocMemBlockForVM(const char *name, SceSize size)
{
   void *hint = NULL, *p;
   (void)name;
   if (nblocks == MAX_BLOCKS)
      return -1;
   if (nblocks == 0)
      /* The first block sits behind the program.  4 MiB are left for the C
       * library's break, which starts at _end in a static binary.
       * VITASIM_FAR=1 puts it 1 GiB up instead: the case heap_vita.c exists
       * to prevent, to see jit_vita.c turn it down and the core carry on
       * with its interpreter. */
      hint = getenv("VITASIM_FAR")
           ? (void *)(uintptr_t)0x40000000u
           : (void *)(((uintptr_t)_end + 0x4FFFFFu) & ~(uintptr_t)0xFFFFF);
   p = mmap(hint, size, PROT_READ | PROT_WRITE | PROT_EXEC,
            MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
   if (p == MAP_FAILED)
      return -1;
   blocks[nblocks].base = p;
   blocks[nblocks].bytes = size;
   return 0x100 + nblocks++;
}

int sceKernelGetMemBlockBase(SceUID uid, void **base)
{
   if (uid < 0x100 || uid >= 0x100 + nblocks)
      return -1;
   *base = blocks[uid - 0x100].base;
   return 0;
}

int sceKernelFreeMemBlock(SceUID uid)
{
   (void)uid;                  /* blocks live as long as the process */
   return 0;
}

int sceKernelOpenVMDomain(void)  { domain_open++; return 0; }
int sceKernelCloseVMDomain(void) { domain_open--; return 0; }

int sceKernelSyncVMDomain(SceUID uid, void *data, SceSize size)
{
   uint8_t *p = data;
   /* What a console would turn into silent stale code is fatal here. */
   if (!domain_open || uid < 0x100 || uid >= 0x100 + nblocks ||
       p < blocks[uid - 0x100].base ||
       p + size > blocks[uid - 0x100].base + blocks[uid - 0x100].bytes)
   {
      fprintf(stderr, "vitasim: BAD sceKernelSyncVMDomain uid=%x data=%p "
              "size=%u (domain_open=%d)\n", (unsigned)uid, data,
              (unsigned)size, domain_open);
      abort();
   }
   __builtin___clear_cache((char *)p, (char *)p + size);
   return 0;
}

int sceKernelGetFreeMemorySize(SceKernelFreeMemorySizeInfo *info)
{
   info->size_user = info->size_cdram = info->size_phycont = 0;
   return 0;
}

int sceKernelGetThreadId(void) { return 1; }

uint32_t gbadhoc_heap_used(void) { return 0; }

/* heap_vita.c's first step, before main(): the VM block, placed first. */
__attribute__((constructor))
static void vitasim_boot(void)
{
   void *base = NULL;
   gbadhoc_mem.vm_uid = sceKernelAllocMemBlockForVM("gbadhoc_jit",
                                                    GBADHOC_VM_BLOCK_BYTES);
   if (gbadhoc_mem.vm_uid >= 0 &&
       sceKernelGetMemBlockBase(gbadhoc_mem.vm_uid, &base) == 0)
   {
      gbadhoc_mem.vm_base = base;
      gbadhoc_mem.vm_bytes = GBADHOC_VM_BLOCK_BYTES;
   }
}
