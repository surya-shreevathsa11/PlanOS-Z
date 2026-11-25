/* Minimal OS abstraction for the executor: mutex, counting semaphore with timeout, thread
 * start/join. Two implementations: runtime/src/osal/osal_zephyr.c (device, native_sim, ESP32)
 * and runtime/src/osal/osal_posix.c (pthreads; used by the host C tests so the SAME parallel
 * executor source is exercised with real threads under plain gcc). */
#ifndef PLANOSZ_OSAL_H_
#define PLANOSZ_OSAL_H_

#include <stdint.h>

#ifdef __ZEPHYR__
#include <zephyr/kernel.h>

struct planosz_mutex {
	struct k_mutex m;
};
struct planosz_sem {
	struct k_sem s;
};
struct planosz_thread {
	struct k_thread t;
	void (*fn)(void *);
	void *arg;
};
#else
#include <pthread.h>

struct planosz_mutex {
	pthread_mutex_t m;
};
struct planosz_sem {
	pthread_mutex_t m;
	pthread_cond_t c;
	unsigned int count;
};
struct planosz_thread {
	pthread_t t;
	void (*fn)(void *);
	void *arg;
};
#endif

void planosz_mutex_init(struct planosz_mutex *m);
void planosz_mutex_lock(struct planosz_mutex *m);
void planosz_mutex_unlock(struct planosz_mutex *m);

void planosz_sem_init(struct planosz_sem *s, unsigned int initial);
void planosz_sem_give(struct planosz_sem *s);
/* timeout_ms < 0: wait forever. Returns 0 when taken, -1 on timeout. */
int planosz_sem_take(struct planosz_sem *s, int32_t timeout_ms);

/* `slot` selects a preallocated stack (Zephyr); must be < PLANOSZ_EXEC_MAX_WORKERS. 0 / -1. */
int planosz_thread_start(struct planosz_thread *t, unsigned int slot, void (*fn)(void *), void *arg);
void planosz_thread_join(struct planosz_thread *t);

#endif /* PLANOSZ_OSAL_H_ */
