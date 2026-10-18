/* hostcore/vitasim: the handful of Vita kernel calls the core's VITA glue and
 * vita/jit_vita.c make, declared for a Linux build (vitasim.c implements
 * them).  Only the VITASIM twin sees this directory; the real build uses
 * vitasdk's <psp2/kernel/sysmem.h>. */
#ifndef VITASIM_PSP2_KERNEL_SYSMEM_H
#define VITASIM_PSP2_KERNEL_SYSMEM_H

#include <stddef.h>

typedef int SceUID;
typedef unsigned int SceSize;

typedef struct SceKernelFreeMemorySizeInfo
{
   int size;
   int size_user;
   int size_cdram;
   int size_phycont;
} SceKernelFreeMemorySizeInfo;

SceUID sceKernelAllocMemBlockForVM(const char *name, SceSize size);
int    sceKernelGetMemBlockBase(SceUID uid, void **base);
int    sceKernelFreeMemBlock(SceUID uid);
int    sceKernelOpenVMDomain(void);
int    sceKernelCloseVMDomain(void);
int    sceKernelSyncVMDomain(SceUID uid, void *data, SceSize size);
int    sceKernelGetFreeMemorySize(SceKernelFreeMemorySizeInfo *info);

#endif
