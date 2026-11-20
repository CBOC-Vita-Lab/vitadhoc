/* A host stand-in for the console's threadmgr, for tests/stdio_lock only: the lightweight mutex is a pthread
 * mutex (non-recursive, like the console's with attr 0), thread ids come from the pthread. */
#ifndef FAKE_THREADMGR_H
#define FAKE_THREADMGR_H
typedef struct SceKernelLwMutexWork { long long pad[8]; } SceKernelLwMutexWork;
int sceKernelCreateLwMutex(SceKernelLwMutexWork *w, const char *name, unsigned attr, int init, void *opt);
int sceKernelLockLwMutex(SceKernelLwMutexWork *w, int count, unsigned *timeout);
int sceKernelUnlockLwMutex(SceKernelLwMutexWork *w, int count);
int sceKernelGetThreadId(void);
int sceKernelDelayThread(unsigned us);
#endif
