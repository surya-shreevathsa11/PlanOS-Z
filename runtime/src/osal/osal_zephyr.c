/* Zephyr implementation of the OSAL: k_mutex / k_sem / statically allocated worker threads. */
#include <zephyr/kernel.h>
#include "planosz/executor.h"
#include "planosz/osal.h"

K_THREAD_STACK_ARRAY_DEFINE(worker_stacks, PLANOSZ_EXEC_MAX_WORKERS, CONFIG_PLANOSZ_WORKER_STACK_SIZE);

void planosz_mutex_init(struct planosz_mutex *m) { k_mutex_init(&m->m); }
void planosz_mutex_lock(struct planosz_mutex *m) { (void)k_mutex_lock(&m->m, K_FOREVER); }
void planosz_mutex_unlock(struct planosz_mutex *m) { (void)k_mutex_unlock(&m->m); }

void planosz_sem_init(struct planosz_sem *s, unsigned int initial) { k_sem_init(&s->s, initial, K_SEM_MAX_LIMIT); }
void planosz_sem_give(struct planosz_sem *s) { k_sem_give(&s->s); }

int planosz_sem_take(struct planosz_sem *s, int32_t timeout_ms)
{
	return k_sem_take(&s->s, timeout_ms < 0 ? K_FOREVER : K_MSEC(timeout_ms)) == 0 ? 0 : -1;
}

static void trampoline(void *p1, void *p2, void *p3)
{
	struct planosz_thread *t = p1;

	ARG_UNUSED(p2);
	ARG_UNUSED(p3);
	t->fn(t->arg);
}

int planosz_thread_start(struct planosz_thread *t, unsigned int slot, void (*fn)(void *), void *arg)
{
	if (slot >= PLANOSZ_EXEC_MAX_WORKERS) {
		return -1;
	}
	t->fn = fn;
	t->arg = arg;
	return k_thread_create(&t->t, worker_stacks[slot], K_THREAD_STACK_SIZEOF(worker_stacks[slot]), trampoline, t,
			       NULL, NULL, K_PRIO_PREEMPT(CONFIG_PLANOSZ_WORKER_PRIORITY), 0, K_NO_WAIT) != NULL ? 0 : -1;
}

void planosz_thread_join(struct planosz_thread *t) { (void)k_thread_join(&t->t, K_FOREVER); }
