/* heap_vita.c -- GBAdhoc's replacement for newlib's Vita heap setup.
 *
 * vitasdk's newlib (libc/sys/vita/sbrk.c) defines _sbrk_r, _init_vita_heap,
 * _free_vita_heap and _get_vita_heap_size in one object.  crt0 calls
 * _init_vita_heap first thing, before main(), and it allocates one memory
 * block for malloc.  This file defines the same four symbols, so the linker
 * takes them from here and never pulls that object out of libc.a -- the
 * same mechanism RetroArch uses (bootstrap/vita/sbrk.c).  This is an
 * independent implementation of that small interface, written for GBAdhoc
 * (GPL-2.0-or-later like the rest of gpsp/vita); nothing is copied from
 * RetroArch, whose frontend is GPL-3.0.
 *
 * What it does differently, and why (jit_vita.h has the long version):
 *
 *  1. THE VM BLOCK COMES FIRST.  The dynarec's executable memory is
 *     allocated before the heap, so the kernel places it directly behind
 *     the program and generated code stays within a 24-bit branch of it.
 *
 *  2. THE HEAP IS SIZED TO WHAT IS THERE.  It asks for GBADHOC_HEAP_BYTES
 *     but never more than the free user memory less GBADHOC_HEAP_RESERVE,
 *     and halves the request while the kernel refuses.  A heap that fails
 *     to allocate is a process that cannot even open its log, so it is
 *     worth more than one attempt.
 *
 * THE NUMBER.  128 MiB.  The largest thing the heap ever holds is a
 * two-cartridge cable session: two 32 MiB ROMs (64 MiB).  Around that: both
 * GB cartridges of a GB link (16 MiB, never at the same time as the GBA
 * ones), save-state buffers (0.4 MiB each, a handful), artwork being
 * decoded for the game list (a few MiB, transient), netdrv (0.4 MiB).  Call
 * it 90 MiB at the very worst.  The rewind buffer (rewind_vita.h: 24 MiB by
 * default) comes on top of that and is the one tenant that yields: it asks
 * plat_heap_free() first and never takes the heap's last 40 MiB.  A Vita application has about 256 MiB of
 * user memory on the standard budget; the program is ~6, the VM block 16, so
 * 128 for the heap leaves ~100 MiB to everything the kernel allocates on the
 * process's behalf: GXM's parameter buffer and the renderer's rings
 * (~20 MiB), system modules for networking and the ad hoc dialog, thread
 * stacks.
 *
 * So the heap does NOT depend on the extended-memory budget.  The SFO asks
 * for it anyway (ATTRIBUTE2 = 12, "+109 MiB"; VITA_ATTRIBUTE2 in the
 * Makefile): it is what this vitasdk's vita-mksfoex writes by default and
 * what vitaQuake -- the reference for the ad hoc connection dialog -- ships
 * with, so it is the configuration the system dialogs are known to work in.
 * With it or without it, step 2 above sizes the heap from what is really
 * free.
 *
 * Runs before main() and before newlib's own initialisation: nothing in
 * _init_vita_heap may call malloc or stdio.  What it found is left in
 * gbadhoc_mem for main() to log.
 */
#include <errno.h>
#include <reent.h>
#include <stddef.h>
#include <psp2/kernel/sysmem.h>
#include <psp2/kernel/threadmgr.h>

#include "jit_vita.h"

gbadhoc_mem_layout gbadhoc_mem = { -1, NULL, 0, -1, NULL, 0, 0, 0, 0 };

static char *heap_cur, *heap_end;
static SceKernelLwMutexWork heap_mutex;
static int heap_mutex_ok;

/* newlib's malloc grows (and trims) its arena through this. */
void *_sbrk_r(struct _reent *reent, ptrdiff_t incr)
{
   char *prev;

   if (!heap_mutex_ok || sceKernelLockLwMutex(&heap_mutex, 1, NULL) < 0)
   {
      reent->_errno = ENOMEM;
      return (void *)-1;
   }
   prev = heap_cur;
   if (!gbadhoc_mem.heap_base ||
       (incr > 0 && (size_t)incr > (size_t)(heap_end - heap_cur)) ||
       (incr < 0 && (size_t)-incr > (size_t)(heap_cur - (char *)gbadhoc_mem.heap_base)))
   {
      sceKernelUnlockLwMutex(&heap_mutex, 1);
      reent->_errno = ENOMEM;
      return (void *)-1;
   }
   heap_cur += incr;
   sceKernelUnlockLwMutex(&heap_mutex, 1);
   return prev;
}

static uint32_t free_user(void)
{
   SceKernelFreeMemorySizeInfo info;
   info.size = sizeof(info);
   if (sceKernelGetFreeMemorySize(&info) < 0)
      return 0;
   return (uint32_t)info.size_user;
}

void _init_vita_heap(void)
{
   uint32_t want = GBADHOC_HEAP_BYTES, avail;
   void *base = NULL;

   heap_mutex_ok = sceKernelCreateLwMutex(&heap_mutex, "gbadhoc sbrk", 0, 0,
                                          NULL) >= 0;
   gbadhoc_mem.free_user_boot = free_user();

   /* 1. executable memory, while nothing else has been placed */
   gbadhoc_mem.vm_uid = sceKernelAllocMemBlockForVM("gbadhoc_jit",
                                                    GBADHOC_VM_BLOCK_BYTES);
   if (gbadhoc_mem.vm_uid >= 0 &&
       sceKernelGetMemBlockBase(gbadhoc_mem.vm_uid, &base) >= 0)
   {
      gbadhoc_mem.vm_base  = (uint8_t *)base;
      gbadhoc_mem.vm_bytes = GBADHOC_VM_BLOCK_BYTES;
   }

   /* 2. the heap, never more than is there */
   gbadhoc_mem.heap_wanted = want;
   avail = free_user();
   if (avail > GBADHOC_HEAP_RESERVE && want > avail - GBADHOC_HEAP_RESERVE)
      want = (avail - GBADHOC_HEAP_RESERVE) & ~0xFFFFFu;
   while (want >= 16u * 1024u * 1024u)
   {
      int uid = sceKernelAllocMemBlock("gbadhoc_heap",
                                       SCE_KERNEL_MEMBLOCK_TYPE_USER_RW,
                                       want, NULL);
      base = NULL;
      if (uid >= 0 && sceKernelGetMemBlockBase(uid, &base) >= 0 && base)
      {
         gbadhoc_mem.heap_uid   = uid;
         gbadhoc_mem.heap_base  = (uint8_t *)base;
         gbadhoc_mem.heap_bytes = want;
         break;
      }
      if (uid >= 0)
         sceKernelFreeMemBlock(uid);
      want = (want / 2) & ~0xFFFFFu;
   }
   heap_cur = (char *)gbadhoc_mem.heap_base;
   heap_end = heap_cur + gbadhoc_mem.heap_bytes;
   gbadhoc_mem.free_user_after = free_user();
}

void _free_vita_heap(void)
{
   if (heap_mutex_ok)
      sceKernelDeleteLwMutex(&heap_mutex);
   heap_mutex_ok = 0;
   if (gbadhoc_mem.heap_uid >= 0)
      sceKernelFreeMemBlock(gbadhoc_mem.heap_uid);
   if (gbadhoc_mem.vm_uid >= 0)
      sceKernelFreeMemBlock(gbadhoc_mem.vm_uid);
   gbadhoc_mem.heap_uid = gbadhoc_mem.vm_uid = -1;
   gbadhoc_mem.heap_base = gbadhoc_mem.vm_base = NULL;
   heap_cur = heap_end = NULL;
}

unsigned int _get_vita_heap_size(void)
{
   return gbadhoc_mem.heap_bytes;
}

uint32_t gbadhoc_heap_used(void)
{
   return gbadhoc_mem.heap_base ? (uint32_t)(heap_cur - (char *)gbadhoc_mem.heap_base) : 0;
}
