#define _POSIX_C_SOURCE 200809L
/* pthread implementation of the OSAL (host tests). */
#include <errno.h>
#include <time.h>
#include "planosz/osal.h"

void planosz_mutex_init(struct planosz_mutex *m) { pthread_mutex_init(&m->m, NULL); }
void planosz_mutex_lock(struct planosz_mutex *m) { pthread_mutex_lock(&m->m); }
void planosz_mutex_unlock(struct planosz_mutex *m) { pthread_mutex_unlock(&m->m); }

void planosz_sem_init(struct planosz_sem *s, unsigned int initial)
{
	pthread_condattr_t attr;

	pthread_mutex_init(&s->m, NULL);
	pthread_condattr_init(&attr);
	pthread_condattr_setclock(&attr, CLOCK_MONOTONIC);
	pthread_cond_init(&s->c, &attr);
	pthread_condattr_destroy(&attr);
	s->count = initial;
}

void planosz_sem_give(struct planosz_sem *s)
{
	pthread_mutex_lock(&s->m);
	s->count++;
	pthread_cond_signal(&s->c);
	pthread_mutex_unlock(&s->m);
}

int planosz_sem_take(struct planosz_sem *s, int32_t timeout_ms)
{
	struct timespec dl;
	int rc = 0;

	pthread_mutex_lock(&s->m);
	if (timeout_ms >= 0) {
		clock_gettime(CLOCK_MONOTONIC, &dl);
		dl.tv_sec += timeout_ms / 1000;
		dl.tv_nsec += (long)(timeout_ms % 1000) * 1000000L;
		if (dl.tv_nsec >= 1000000000L) {
			dl.tv_sec++;
			dl.tv_nsec -= 1000000000L;
		}
	}
	while (s->count == 0 && rc == 0) {
		if (timeout_ms < 0) {
			pthread_cond_wait(&s->c, &s->m);
		} else {
			rc = pthread_cond_timedwait(&s->c, &s->m, &dl);
		}
	}
	if (s->count > 0) {
		s->count--;
		rc = 0;
	} else {
		rc = -1;
	}
	pthread_mutex_unlock(&s->m);
	return rc;
}

static void *trampoline(void *p)
{
	struct planosz_thread *t = p;

	t->fn(t->arg);
	return NULL;
}

int planosz_thread_start(struct planosz_thread *t, unsigned int slot, void (*fn)(void *), void *arg)
{
	(void)slot;
	t->fn = fn;
	t->arg = arg;
	return pthread_create(&t->t, NULL, trampoline, t) == 0 ? 0 : -1;
}

void planosz_thread_join(struct planosz_thread *t) { pthread_join(t->t, NULL); }
