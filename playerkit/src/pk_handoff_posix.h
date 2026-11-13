/* HandoffSys on POSIX semaphores, for the Linux tests and the twin (not for the console, which uses sceKernel
 * semaphores). Copyright CBOC-Vita. MIT licence (see playerkit/LICENSE). */
#ifndef PK_HANDOFF_POSIX_H
#define PK_HANDOFF_POSIX_H

#include "pk_handoff.h"
#include <semaphore.h>
#include <time.h>
#include <errno.h>

namespace pk {

class PosixHandoffSys : public HandoffSys {
public:
	uint64_t now_us()
	{
		struct timespec ts;
		clock_gettime(CLOCK_MONOTONIC, &ts);
		return (uint64_t) ts.tv_sec * 1000000u + (uint64_t) ts.tv_nsec / 1000u;
	}
	void *sem_create() { sem_t *s = new sem_t; sem_init(s, 0, 0); return s; }
	void sem_destroy(void *s) { ::sem_destroy((sem_t *) s); delete (sem_t *) s; }
	void sem_post(void *s) { ::sem_post((sem_t *) s); }
	bool sem_wait(void *s, uint32_t timeout_us)
	{
		if (timeout_us == 0xFFFFFFFFu) {
			while (::sem_wait((sem_t *) s) != 0 && errno == EINTR) {}
			return true;
		}
		struct timespec ts;
		clock_gettime(CLOCK_REALTIME, &ts);
		const uint64_t ns = (uint64_t) ts.tv_nsec + (uint64_t) timeout_us * 1000u;
		ts.tv_sec += (time_t) (ns / 1000000000u);
		ts.tv_nsec = (long) (ns % 1000000000u);
		int r;
		while ((r = ::sem_timedwait((sem_t *) s, &ts)) != 0 && errno == EINTR) {}
		return r == 0;
	}
};

}  // namespace pk

#endif
