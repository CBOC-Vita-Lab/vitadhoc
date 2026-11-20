/* VitAdhoc: what the core needs from the platform to run its worker threads (VDP line renderer, FM synthesis). The core has no thread
 * code of its own: the frontend hands over these functions (md_host_set_threads in md/frontend/host.h), so the same core runs on the Vita
 * (sceKernel threads and semaphores), on Linux (pthreads) and with the workers off. Copyright CBOC-Vita. MIT licence for this file. */
#ifndef MD_THREAD_OPS_H
#define MD_THREAD_OPS_H

#ifdef __cplusplus
extern "C" {
#endif

/* worker ids */
enum { MD_WORKER_VDP = 0, MD_WORKER_FM = 1, MD_WORKERS = 2 };

typedef struct md_thread_ops {
	void *user;
	/* a counting semaphore starting at 0 */
	void *(*sem_create)(void *user);
	void (*sem_destroy)(void *user, void *sem);
	void (*sem_post)(void *user, void *sem);
	void (*sem_wait)(void *user, void *sem);
	/* runs fn(arg) on a new thread (the platform picks the core and priority for worker `id`); 0 on success */
	int (*spawn)(void *user, int id, void (*fn)(void *arg), void *arg);
	/* waits until fn of worker `id` has returned */
	void (*wait_end)(void *user, int id);
	/* gives up the rest of the time slice (a short wait for the other side) */
	void (*yield)(void *user);
	/* optional (NULL = no timing in the census): microseconds from any steady clock */
	unsigned long long (*now_us)(void *user);
} md_thread_ops;

#ifdef __cplusplus
}
#endif
#endif
