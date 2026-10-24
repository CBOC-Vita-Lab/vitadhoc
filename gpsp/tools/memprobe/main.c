/* memprobe -- what does THIS console give a GBAdhoc-shaped EBOOT?
 *
 * docs/MEMORY-BUDGET.md.  One run writes memprobe.txt next to the EBOOT and
 * returns to the XMB.  It carries GBAdhoc's own memory settings (the same
 * module/heap/stack macros as psp/main_psp.c, MEMSIZE=1 in PARAM.SFO like a
 * playable or harness64 build), so its numbers are the ones GBAdhoc gets,
 * shifted only by the difference in image size, which it reports:
 *
 *     GBAdhoc heap at boot = heap_plus_image - (GBAdhoc's image)
 *
 * where image = __executable_start.._end (text through .bss).  Both images
 * load at the same address and the heap is the next block, so the sum is a
 * property of the console, not of the binary.
 *
 * What it records, in order:
 *   1. model, firmware, CFW
 *   2. memory OUTSIDE the heap at entry (libc's start-up already took the
 *      heap block before main): kernel free/max, and a bisected
 *      sceKernelAllocPartitionMemory per partition id (what user code can
 *      still take, and where)
 *   3. the heap: size (newlib's single block), largest single malloc,
 *      greedy 1 MiB blocks (gpSP's ROM loop)
 *   4. md5 of every GBADHOC* folder's EBOOT.PBP / gbadhoc_me.prx AS THIS
 *      CONSOLE READS THEM (the 2026-10-02 Go black screen was a damaged
 *      EBOOT on the card, not memory)
 *   5. LAST, after the file is saved once: the kernel's own partition table
 *      (SysMemForKernel through ARK's systemctrl + kubridge), when offered
 */
#include <pspkernel.h>
#include <pspdebug.h>
#include <pspdisplay.h>
#include <pspctrl.h>
#include <psppower.h>
#include <psputils.h>
#include <pspsysmem.h>
#include <kubridge.h>
#include <systemctrl.h>
#include <malloc.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* EXACTLY psp/main_psp.c's settings: the probe must be shaped like the app. */
PSP_MODULE_INFO("gbadhoc-memprobe", 0, 1, 0);
PSP_MAIN_THREAD_ATTR(THREAD_ATTR_USER | THREAD_ATTR_VFPU);
PSP_MAIN_THREAD_STACK_SIZE_KB(256);
PSP_HEAP_SIZE_KB(-1024);

#define MEMPROBE_VERSION "memprobe 1"

extern char __executable_start[], _end[];
extern SceUID __psp_heap_blockid;   /* libcglue: newlib's heap block */

/* ------------------------------------------------------------- output --- */
static char g_out[24 * 1024];   /* static: the report costs no heap */
static int  g_len;

static void out(const char *fmt, ...)
{
   va_list ap;
   int n;
   if (g_len >= (int)sizeof(g_out) - 1)
      return;
   va_start(ap, fmt);
   n = vsnprintf(g_out + g_len, sizeof(g_out) - g_len, fmt, ap);
   va_end(ap);
   if (n > 0)
      g_len += n;
   if (g_len > (int)sizeof(g_out) - 1)
      g_len = sizeof(g_out) - 1;
}

/* ------------------------------------------------------- exit callback --- */
static volatile int g_quit;
static int exit_cb(int a, int b, void *c)
{
   (void)a; (void)b; (void)c;
   g_quit = 1;
   return 0;
}
static int cb_thread(SceSize args, void *argp)
{
   int id;
   (void)args; (void)argp;
   id = sceKernelCreateCallback("memprobe exit", exit_cb, NULL);
   sceKernelRegisterExitCallback(id);
   sceKernelSleepThreadCB();
   return 0;
}

/* ------------------------------------------------------------- model --- */
static const char *model_name(int m)
{
   switch (m)
   {
      case 0:  return "PSP-1000";
      case 1:  return "PSP-2000";
      case 2:  return "PSP-3000";
      case 3:  return "PSP-3000 (04g)";
      case 4:  return "PSP Go (N1000)";
      case 6:  return "PSP-3000 (07g)";
      case 8:  return "PSP-3000 (09g)";
      case 10: return "PSP Street (E1000)";
      default: return "unknown";
   }
}

/* --------------------------------------------------- partition probing --- */
/* The largest block user code can take from partition `pid` right now,
 * bisected with sceKernelAllocPartitionMemory and handed straight back.
 * *head = where the kernel put the largest successful one. */
static unsigned part_largest(int pid, unsigned *head, int *err)
{
   unsigned lo = 0, hi = 64u << 20;
   SceUID id;
   *head = 0;
   *err = 0;
   id = sceKernelAllocPartitionMemory(pid, "mp_probe", PSP_SMEM_Low, 4096,
                                      NULL);
   if (id < 0)
   {
      *err = id;
      return 0;
   }
   sceKernelFreePartitionMemory(id);
   lo = 4096;
   while (hi - lo > 4096u)
   {
      unsigned mid = (lo + (hi - lo) / 2u) & ~4095u;
      if (mid <= lo)
         break;
      id = sceKernelAllocPartitionMemory(pid, "mp_probe", PSP_SMEM_Low, mid,
                                         NULL);
      if (id >= 0)
      {
         lo = mid;
         *head = (unsigned)sceKernelGetBlockHeadAddr(id);
         sceKernelFreePartitionMemory(id);
      }
      else
         hi = mid;
   }
   return lo;
}

/* The kernel's partition table, through the CFW: SysMemForKernel is not
 * importable from a user EBOOT, so ask systemctrl where the function is and
 * call it with kubridge.  Kernel NIDs are randomised per firmware; the 6.60
 * NID is tried first, then the pre-6.xx one (ARK resolves old NIDs).  Every
 * failure is reported, never fatal: on a console without ARK/PRO the stubs
 * stay unlinked and return an error code instead of an address. */
typedef struct
{
   SceSize      size;
   unsigned int startaddr;
   unsigned int memsize;
   unsigned int attr;
} mp_part_info;

static unsigned kfunc(unsigned nid660, unsigned nid_old)
{
   unsigned f = (unsigned)sctrlHENFindFunction("sceSystemMemoryManager",
                                               "SysMemForKernel", nid660);
   if ((f & 0xFC000000u) != 0x88000000u)
      f = (unsigned)sctrlHENFindFunction("sceSystemMemoryManager",
                                         "SysMemForKernel", nid_old);
   return ((f & 0xFC000000u) == 0x88000000u) ? f : 0u;
}

static unsigned kcall2(unsigned f, unsigned a1, unsigned a2, int *rc)
{
   KernelCallArg args;
   memset(&args, 0, sizeof(args));
   args.arg1 = a1;
   args.arg2 = a2;
   *rc = kuKernelCall((void *)f, &args);
   return args.ret1;
}

static void kernel_partitions(void)
{
   /* SysMemForKernel NIDs, from pspsdk's libpspsysmem_kernel_660.a and
    * libpspkernel.a stubs. */
   unsigned f_info = kfunc(0xC4EEAF20u, 0x55A40B2Cu);  /* QueryMemoryPartitionInfo */
   unsigned f_tot  = kfunc(0x0115B0F8u, 0x9697CD32u);  /* PartitionTotalFreeMemSize */
   unsigned f_max  = kfunc(0x13F4A0DEu, 0xE6581468u);  /* PartitionMaxFreeMemSize */
   int pid;
   out("\n[kernel partitions] (SysMemForKernel via sctrlHENFindFunction + kuKernelCall)\n");
   out("fn query=%08x total_free=%08x max_free=%08x\n", f_info, f_tot, f_max);
   if (!f_info)
   {
      out("unavailable: the CFW did not resolve the kernel function\n");
      return;
   }
   out("pid      start       size       attr   total_free     max_free\n");
   for (pid = 1; pid <= 12; pid++)
   {
      static mp_part_info info;   /* static: user memory the kernel fills */
      int rc, r1 = 0, r2 = 0;
      unsigned ret, tf = 0, mf = 0;
      memset(&info, 0, sizeof(info));
      info.size = sizeof(info);
      ret = kcall2(f_info, (unsigned)pid, (unsigned)&info, &rc);
      if (rc < 0 || (int)ret < 0)
      {
         out("%3d  (query rc=%08x ret=%08x)\n", pid, (unsigned)rc, ret);
         continue;
      }
      if (f_tot)
         tf = kcall2(f_tot, (unsigned)pid, 0, &r1);
      if (f_max)
         mf = kcall2(f_max, (unsigned)pid, 0, &r2);
      out("%3d  0x%08x %10u  0x%04x %12u %12u\n", pid, info.startaddr,
          info.memsize, info.attr, tf, mf);
   }
}

/* ---------------------------------------------------------------- heap --- */
/* newlib's heap is ONE partition block taken at the first sbrk (all but 1 MiB
 * of the largest free block, PSP_HEAP_SIZE_KB(-1024)).  Its size = what has
 * been sbrk'd (mallinfo arena) + the part not yet sbrk'd (bisected). */
static unsigned sbrk_tail(void)
{
   unsigned lo = 0, hi = 64u << 20;
   while (hi - lo > 4096u)
   {
      unsigned mid = (lo + (hi - lo) / 2u) & ~4095u;
      if (mid <= lo)
         break;
      if (sbrk((ptrdiff_t)mid) != (void *)-1)
      {
         sbrk(-(ptrdiff_t)mid);
         lo = mid;
      }
      else
         hi = mid;
   }
   return lo;
}

static unsigned largest_malloc(void)
{
   unsigned lo = 0, hi = 64u << 20;
   while (hi - lo > 1024u)
   {
      unsigned mid = lo + (hi - lo) / 2u;
      void * volatile p = malloc(mid);
      if (p)
      {
         free(p);
         lo = mid;
      }
      else
         hi = mid;
   }
   return lo;
}

/* ------------------------------------------------------------ md5 scan --- */
static unsigned char g_buf[64 * 1024];

static int md5_file(const char *path, char hex[33], unsigned *size)
{
   SceKernelUtilsMd5Context ctx;
   unsigned char d[16];
   SceUID fd = sceIoOpen(path, PSP_O_RDONLY, 0);
   int n, i;
   *size = 0;
   if (fd < 0)
      return fd;
   sceKernelUtilsMd5BlockInit(&ctx);
   while ((n = sceIoRead(fd, g_buf, sizeof(g_buf))) > 0)
   {
      sceKernelUtilsMd5BlockUpdate(&ctx, g_buf, (u32)n);
      *size += (unsigned)n;
   }
   sceIoClose(fd);
   sceKernelUtilsMd5BlockResult(&ctx, d);
   for (i = 0; i < 16; i++)
      snprintf(hex + 2 * i, 3, "%02x", d[i]);
   return n < 0 ? n : 0;
}

static void scan_games(const char *dev)
{
   static const char *files[] = { "EBOOT.PBP", "gbadhoc_me.prx" };
   char dir[64], path[384];
   SceIoDirent de;
   SceUID d;
   int found = 0;
   snprintf(dir, sizeof(dir), "%s/PSP/GAME", dev);
   out("\n[installed images, as this console reads them] %s\n", dir);
   d = sceIoDopen(dir);
   if (d < 0)
   {
      out("cannot open %s (%08x)\n", dir, (unsigned)d);
      return;
   }
   memset(&de, 0, sizeof(de));
   while (sceIoDread(d, &de) > 0)
   {
      unsigned k;
      if (strncasecmp(de.d_name, "GBADHOC", 7) != 0)
      {
         memset(&de, 0, sizeof(de));
         continue;
      }
      for (k = 0; k < sizeof(files) / sizeof(files[0]); k++)
      {
         char hex[33] = "";
         unsigned sz;
         int rc;
         snprintf(path, sizeof(path), "%s/%s/%s", dir, de.d_name, files[k]);
         rc = md5_file(path, hex, &sz);
         if (rc == 0)
            out("md5 %s %10u %s/%s\n", hex, sz, de.d_name, files[k]);
         else if (k == 0)
            out("md5 (error %08x) %s/%s\n", (unsigned)rc, de.d_name, files[k]);
         found++;
      }
      memset(&de, 0, sizeof(de));
   }
   sceIoDclose(d);
   if (!found)
      out("no GBADHOC* folders\n");
}

static SceUID save(const char *path)
{
   SceUID fd = sceIoOpen(path, PSP_O_WRONLY | PSP_O_CREAT | PSP_O_TRUNC, 0777);
   if (fd >= 0)
   {
      sceIoWrite(fd, g_out, g_len);
      sceIoClose(fd);
   }
   return fd;
}

/* --------------------------------------------------------------- main --- */
int main(int argc, char *argv[])
{
   char base[160] = "ms0:/PSP/GAME/GBADHOC-MEMPROBE", dev[16] = "ms0:";
   char path[200];
   unsigned img = (unsigned)(_end - __executable_start);
   int k_free0, k_max0, k_free1, k_max1, model, pid;
   unsigned heap_arena, heap_tail, heap_total, big, blocks = 0, left;
   struct mallinfo mi;
   SceUID fd;
   SceUID th;
   void **blk;

   /* ---- 1-2. AT ENTRY: nothing below may malloc before the census. ---- */
   k_free0 = sceKernelTotalFreeMemSize();
   k_max0  = sceKernelMaxFreeMemSize();
   mi = mallinfo();   /* arena > 0 here means libc took the heap before main */

   out("%s (GBAdhoc docs/MEMORY-BUDGET.md)\n", MEMPROBE_VERSION);
   model = kuKernelGetModel();
   out("model=%d %s (kuKernelGetModel; 0=1000 1=2000 2=3000 4=Go 10=Street)\n",
       model, model_name(model));
   out("devkit=0x%08x (sceKernelDevkitVersion)  cfw_hen=0x%08x minor=0x%08x\n",
       (unsigned)sceKernelDevkitVersion(), (unsigned)sctrlHENGetVersion(),
       (unsigned)sctrlHENGetMinorVersion());
   if (argc > 0 && argv && argv[0])
   {
      const char *slash = strrchr(argv[0], '/');
      const char *colon = strchr(argv[0], ':');
      out("argv0=%s\n", argv[0]);
      if (slash && (size_t)(slash - argv[0]) < sizeof(base))
      {
         memcpy(base, argv[0], (size_t)(slash - argv[0]));
         base[slash - argv[0]] = 0;
      }
      if (colon && (size_t)(colon - argv[0] + 1) < sizeof(dev))
      {
         memcpy(dev, argv[0], (size_t)(colon - argv[0] + 1));
         dev[colon - argv[0] + 1] = 0;
      }
   }
   out("settings: PSP_HEAP_SIZE_KB(-1024) stack=256KiB MEMSIZE=1 "
       "(= GBAdhoc playable/harness64)\n");
   out("image=%u (0x%08x..0x%08x)\n", img, (unsigned)__executable_start,
       (unsigned)_end);

   out("\n[entry: outside the heap (libc took the heap before main)]\n");
   out("k_total_free=%d k_max_free=%d libc_arena_before_main=%u\n",
       k_free0, k_max0, (unsigned)mi.arena);
   out("user-allocatable per partition (bisected sceKernelAllocPartitionMemory, Low):\n");
   for (pid = 1; pid <= 12; pid++)
   {
      unsigned head;
      int err;
      unsigned sz = part_largest(pid, &head, &err);
      if (err)
         out("  pid %2d: refused %08x\n", pid, (unsigned)err);
      else
         out("  pid %2d: largest=%u at 0x%08x\n", pid, sz, head);
   }


   /* ---- 3. THE HEAP (taken by libc start-up; arena>0 above says so) ---- */
   {
      void * volatile first = malloc(16);
      free(first);
   }
   mi = mallinfo();
   heap_arena = (unsigned)mi.arena;
   heap_tail  = sbrk_tail();
   heap_total = heap_arena + heap_tail;
   k_free1 = sceKernelTotalFreeMemSize();
   k_max1  = sceKernelMaxFreeMemSize();
   big = largest_malloc();
   out("\n[heap]\n");
   out("heap_block=%u (arena %u + unsbrk'd %u)  head=0x%08x\n", heap_total,
       heap_arena, heap_tail,
       (unsigned)sceKernelGetBlockHeadAddr(__psp_heap_blockid));
   out("k_total_free=%d k_max_free=%d (outside the heap)\n", k_free1, k_max1);
   out("largest_malloc=%u\n", big);
   /* gpSP's ROM loop: 1 MiB blocks until malloc fails (cap 64), then what is
    * left beside them; everything is handed back afterwards. */
   blk = (void **)malloc(64 * sizeof(void *));
   if (blk)
   {
      unsigned n;
      while (blocks < 64 && (blk[blocks] = malloc(1u << 20)) != NULL)
         blocks++;
      left = largest_malloc();
      for (n = blocks; n; n--)
         free(blk[n - 1]);
      out("greedy_1mib_blocks=%u then largest=%u\n", blocks, left);
      free(blk);
   }

   out("\n[derived]\n");
   out("heap_plus_image=%u   GBAdhoc heap at boot = heap_plus_image - its image\n",
       heap_total + img);

   /* ---- 4. what the card holds, from this console's side ---- */
   scan_games(dev);

   /* Saved BEFORE the kernel calls: if a CFW mis-resolves a kernel function,
    * everything above is already on the card. */
   snprintf(path, sizeof(path), "%s/memprobe.txt", base);
   fd = save(path);

   /* ---- 5. the kernel's partition table (last: the only risky part) ---- */
   kernel_partitions();
   out("\ndone\n");
   fd = save(path);

   /* ---- screen, then back to the XMB ---- */
   th = sceKernelCreateThread("memprobe cb", cb_thread, 0x11, 0x1000, 0, 0);
   if (th >= 0)
      sceKernelStartThread(th, 0, 0);
   pspDebugScreenInit();
   pspDebugScreenPrintf("GBAdhoc memprobe -- %s\n", fd >= 0 ? path :
                        "(could not write memprobe.txt)");
   pspDebugScreenPrintf("model %d %s  heap %u  largest %u\n", model,
                        model_name(model), heap_total, big);
   pspDebugScreenPrintf("heap_plus_image %u\n\n", heap_total + img);
   pspDebugScreenPrintf("Returning to the XMB in 10 s (X: now).\n");
   {
      int t;
      SceCtrlData pad;
      for (t = 0; t < 600 && !g_quit; t++)
      {
         sceCtrlPeekBufferPositive(&pad, 1);
         if (pad.Buttons & PSP_CTRL_CROSS)
            break;
         sceDisplayWaitVblankStart();
      }
   }
   sceKernelExitGame();
   return 0;
}
