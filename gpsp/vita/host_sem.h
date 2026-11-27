/* host_sem.h -- a counting semaphore for the host twin's threads.
 *
 * Linux: sem_t.  macOS: sem_init() always fails there (unnamed POSIX
 * semaphores are not implemented) and there is no sem_timedwait(), so the
 * same four calls are a mutex and a condition variable.  Only the host
 * twin's thread glue includes it (render_thread.c, upscale_vita.c,
 * handoff_glue.cpp); the console uses sceKernel semaphores.
 *
 *   host_sem_init(s, 0)    host_sem_post(s)    host_sem_destroy(s)
 *   host_sem_wait(s)       0 once taken (retry on -1/EINTR)
 *   host_sem_timedwait(s, &at)    at: CLOCK_REALTIME; 0 taken, -1 and
 *                                  errno ETIMEDOUT when it ran out
 */
#ifndef HOST_SEM_H
#define HOST_SEM_H

#include <errno.h>
#include <pthread.h>
#include <time.h>

#ifdef __APPLE__

typedef struct host_sem
{
   pthread_mutex_t m;
   pthread_cond_t  c;
   unsigned        n;
} host_sem_t;

static inline int host_sem_init(host_sem_t *s, unsigned value)
{
   if (pthread_mutex_init(&s->m, NULL) != 0)
      return -1;
   if (pthread_cond_init(&s->c, NULL) != 0)
   {
      pthread_mutex_destroy(&s->m);
      return -1;
   }
   s->n = value;
   return 0;
}

static inline int host_sem_destroy(host_sem_t *s)
{
   pthread_cond_destroy(&s->c);
   pthread_mutex_destroy(&s->m);
   return 0;
}

static inline int host_sem_post(host_sem_t *s)
{
   pthread_mutex_lock(&s->m);
   s->n++;
   pthread_cond_signal(&s->c);
   pthread_mutex_unlock(&s->m);
   return 0;
}

static inline int host_sem_wait(host_sem_t *s)
{
   pthread_mutex_lock(&s->m);
   while (s->n == 0)
      pthread_cond_wait(&s->c, &s->m);
   s->n--;
   pthread_mutex_unlock(&s->m);
   return 0;
}

static inline int host_sem_trywait(host_sem_t *s)
{
   int got = 0;
   pthread_mutex_lock(&s->m);
   if (s->n)
   {
      s->n--;
      got = 1;
   }
   pthread_mutex_unlock(&s->m);
   if (!got)
   {
      errno = EAGAIN;
      return -1;
   }
   return 0;
}

static inline int host_sem_timedwait(host_sem_t *s, const struct timespec *at)
{
   int rc = 0;
   pthread_mutex_lock(&s->m);
   while (s->n == 0 && rc == 0)
      rc = pthread_cond_timedwait(&s->c, &s->m, at);
   if (s->n)
   {
      s->n--;
      rc = 0;
   }
   pthread_mutex_unlock(&s->m);
   if (rc != 0)
   {
      errno = rc == ETIMEDOUT ? ETIMEDOUT : EINVAL;
      return -1;
   }
   return 0;
}

#else

#include <semaphore.h>

typedef sem_t host_sem_t;
/* (functions, not macros: handoff_glue.cpp calls them from members that are
 * themselves named sem_post() and friends) */
static inline int host_sem_init(host_sem_t *s, unsigned value) { return sem_init(s, 0, value); }
static inline int host_sem_destroy(host_sem_t *s) { return sem_destroy(s); }
static inline int host_sem_post(host_sem_t *s) { return sem_post(s); }
static inline int host_sem_wait(host_sem_t *s) { return sem_wait(s); }
static inline int host_sem_trywait(host_sem_t *s) { return sem_trywait(s); }
static inline int host_sem_timedwait(host_sem_t *s, const struct timespec *at) { return sem_timedwait(s, at); }

#endif

#endif /* HOST_SEM_H */
