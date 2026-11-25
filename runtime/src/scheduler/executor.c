/* Parallel DAG executor.
 *
 * Coordinator (the calling thread) + N worker threads:
 *   - ready-set: a step is runnable when every dependency is DONE or SKIPPED;
 *   - `when` conditions are evaluated at dispatch time (false => SKIPPED, counts as complete);
 *   - resources (device-derived masks) are acquired before dispatch; admission makes a failed
 *     acquire impossible, so a failure here means reality disagrees with the plan => abort;
 *   - watchdogs: per-step budget = device-derived WCET, global plan deadline. A skill cannot
 *     be preempted, so a fired watchdog reports immediately, starts nothing new and lets the
 *     in-flight skills return before run_end (see executor.h).
 *
 * All shared state lives behind one mutex (`X.lock`); trace lines are emitted while holding it
 * with the timestamp read inside the critical section, so the trace is totally ordered and
 * t_ms never goes backwards. Skill bodies run OUTSIDE the lock - that is where steps overlap. */
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include "planosz/executor.h"
#include "planosz/hal.h"
#include "planosz/osal.h"
#include "planosz/resource.h"
#include "planosz/skill_table.gen.h"
#include "planosz/skills.h"
#include "planosz/trace.h"

enum step_state { ST_PENDING, ST_RUNNING, ST_DONE, ST_SKIPPED, ST_FAILED, ST_CANCELLED };

struct worker {
	struct planosz_thread th;
	struct planosz_sem go;
	int step; /* index being executed, -1 when idle (guarded by X.lock) */
};

static struct {
	const struct planosz_ir_plan *plan;
	const struct planosz_hal_ops *hal;
	uint32_t t0;
	struct planosz_derived dv[PLANOSZ_MAX_STEPS]; /* device-derived wcet + resource masks */
	uint8_t state[PLANOSZ_MAX_STEPS];
	uint32_t start_ms[PLANOSZ_MAX_STEPS];
	int32_t out[PLANOSZ_MAX_STEPS];
	uint32_t finished; /* bit i: step i DONE or SKIPPED */
	struct planosz_mutex lock;
	struct planosz_sem wake; /* workers -> coordinator */
	struct worker w[PLANOSZ_EXEC_MAX_WORKERS];
	unsigned int n_workers;
	bool stop;
	bool aborting;
	unsigned int in_flight;
	struct planosz_exec_result r;
	bool locks_ready;
} X;

static bool cond_holds(uint8_t op, int32_t lhs, int32_t rhs)
{
	switch (op) {
	case PLANOSZ_CMP_GT: return lhs > rhs;
	case PLANOSZ_CMP_GE: return lhs >= rhs;
	case PLANOSZ_CMP_LT: return lhs < rhs;
	case PLANOSZ_CMP_LE: return lhs <= rhs;
	case PLANOSZ_CMP_EQ: return lhs == rhs;
	default: return lhs != rhs;
	}
}

static uint32_t now_ms(void)
{
	return X.hal->uptime_ms() - X.t0;
}

/* Emit one trace line stamped "now". Caller holds X.lock. */
static void emit(const char *type, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void emit(const char *type, const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	planosz_trace_vemit(now_ms(), type, fmt, ap);
	va_end(ap);
}

/* ["a","b"] of the step ids in `mask` (valid JSON even when it has to stop early). */
static void json_names(char *buf, size_t cap, uint32_t mask)
{
	size_t n = 0;

	buf[n++] = '[';
	for (int j = 0; j < X.plan->n_steps && n + 2 < cap; j++) {
		const char *id = X.plan->steps[j].id;

		if (!(mask & (1u << j)) || n + strlen(id) + 5 >= cap) {
			continue;
		}
		n += (size_t)snprintf(buf + n, cap - n, "%s\"%s\"", n > 1 ? "," : "", id);
	}
	buf[n++] = ']';
	buf[n] = '\0';
}

/* ["adc0:shared","power_domain:shared"] from device resource masks. */
static void json_claims(char *buf, size_t cap, const struct planosz_derived *d)
{
	size_t n = 0;

	buf[n++] = '[';
	for (unsigned int b = 0; b < PLANOSZ_RES_COUNT; b++) {
		const char *mode = (d->excl_mask & (1u << b)) ? "exclusive" : (d->shared_mask & (1u << b)) ? "shared" : NULL;

		if (mode == NULL || n + strlen(planosz_res_names[b]) + strlen(mode) + 6 >= cap) {
			continue;
		}
		n += (size_t)snprintf(buf + n, cap - n, "%s\"%s:%s\"", n > 1 ? "," : "", planosz_res_names[b], mode);
	}
	buf[n++] = ']';
	buf[n] = '\0';
}

static void fail_run(enum planosz_code code, int step)
{
	if (X.r.code == PLANOSZ_OK) {
		X.r.code = code;
		X.r.failed_step = step;
	}
}

/* Stop dispatching; report every still-running step as cancelled. Caller holds X.lock. */
static void abort_run(void)
{
	X.aborting = true;
	for (int i = 0; i < X.plan->n_steps; i++) {
		if (X.state[i] == ST_RUNNING) {
			X.state[i] = ST_CANCELLED;
			emit("step_cancel", "\"step\":\"%s\",\"code\":\"EXEC_CANCELLED\"", X.plan->steps[i].id);
		}
	}
}

static void worker_main(void *arg)
{
	struct worker *w = arg;

	for (;;) {
		(void)planosz_sem_take(&w->go, -1);
		planosz_mutex_lock(&X.lock);
		if (X.stop) {
			planosz_mutex_unlock(&X.lock);
			return;
		}
		const int i = w->step;
		const struct planosz_ir_step *s = &X.plan->steps[i];

		planosz_mutex_unlock(&X.lock);

		int32_t out = 0;
		enum planosz_code rc = planosz_skill_dispatch(s, &out); /* the only part that runs unlocked */

		planosz_mutex_lock(&X.lock);
		planosz_res_release(X.dv[i].excl_mask, X.dv[i].shared_mask);
		if (X.state[i] == ST_RUNNING) { /* otherwise a watchdog/abort already reported this step */
			const uint32_t end = now_ms();
			const uint32_t dur = end - X.start_ms[i];

			if (rc == PLANOSZ_OK && dur > X.dv[i].wcet_ms) {
				rc = PLANOSZ_EXEC_TIMEOUT;
			}
			if (rc != PLANOSZ_OK) {
				X.state[i] = ST_FAILED;
				emit("step_error", "\"step\":\"%s\",\"code\":\"%s\"", s->id, planosz_code_str(rc));
				fail_run(rc, i);
				abort_run();
			} else {
				X.out[i] = out;
				X.state[i] = ST_DONE;
				X.finished |= 1u << i;
				emit("step_end", "\"step\":\"%s\",\"status\":\"OK\",\"dur_ms\":%u,\"out_milli\":%d", s->id,
				     (unsigned int)dur, (int)out);
			}
		}
		w->step = -1;
		X.in_flight--;
		planosz_mutex_unlock(&X.lock);
		planosz_sem_give(&X.wake);
	}
}

/* Hand ready steps to idle workers; evaluate conditions. Caller holds X.lock. */
static void dispatch_ready(void)
{
	const struct planosz_ir_plan *p = X.plan;
	bool progress = true;

	while (progress && !X.aborting) {
		progress = false;
		for (int i = 0; i < p->n_steps && !X.aborting; i++) {
			const struct planosz_ir_step *s = &p->steps[i];
			struct worker *idle = NULL;

			if (X.state[i] != ST_PENDING || (s->deps_mask & ~X.finished) != 0) {
				continue;
			}
			if (s->when_step >= 0 && !cond_holds(s->when_op, X.out[s->when_step], s->when_value_milli)) {
				X.state[i] = ST_SKIPPED;
				X.finished |= 1u << i;
				emit("step_skip", "\"step\":\"%s\",\"observed_milli\":%d", s->id, (int)X.out[s->when_step]);
				progress = true;
				continue;
			}
			for (unsigned int k = 0; k < X.n_workers; k++) {
				if (X.w[k].step < 0) {
					idle = &X.w[k];
					break;
				}
			}
			if (idle == NULL) {
				continue; /* all workers busy: stays ready until one frees up */
			}
			if (planosz_res_acquire(X.dv[i].excl_mask, X.dv[i].shared_mask) != 0) {
				X.state[i] = ST_FAILED;
				emit("step_error", "\"step\":\"%s\",\"code\":\"EXEC_SKILL_FAIL\",\"detail\":\"resource busy\"",
				     s->id);
				fail_run(PLANOSZ_EXEC_SKILL_FAIL, i);
				abort_run();
				break;
			}
			char needs[160], claims[200];

			json_names(needs, sizeof(needs), s->deps_mask);
			json_claims(claims, sizeof(claims), &X.dv[i]);
			X.state[i] = ST_RUNNING;
			X.start_ms[i] = now_ms();
			X.in_flight++;
			if (X.in_flight > X.r.peak_concurrency) {
				X.r.peak_concurrency = (uint8_t)X.in_flight;
			}
			emit("step_start", "\"step\":\"%s\",\"action\":\"%s\",\"skill\":%u,\"wcet_ms\":%u,\"needs\":%s,"
			     "\"claims\":%s", s->id, planosz_registry_get(s->skill_idx)->skill_id,
			     (unsigned int)s->skill_idx, (unsigned int)X.dv[i].wcet_ms, needs, claims);
			idle->step = i;
			planosz_sem_give(&idle->go);
			progress = true;
		}
	}
}

/* Per-step budget + plan deadline. Caller holds X.lock. */
static void check_watchdogs(void)
{
	const uint32_t t = now_ms();

	if (X.aborting) {
		return;
	}
	for (int i = 0; i < X.plan->n_steps; i++) {
		if (X.state[i] == ST_RUNNING && t - X.start_ms[i] > X.dv[i].wcet_ms) {
			X.state[i] = ST_FAILED;
			emit("step_error", "\"step\":\"%s\",\"code\":\"EXEC_TIMEOUT\",\"detail\":\"exceeded wcet_ms=%u\"",
			     X.plan->steps[i].id, (unsigned int)X.dv[i].wcet_ms);
			fail_run(PLANOSZ_EXEC_TIMEOUT, i);
			abort_run();
			return;
		}
	}
	const uint32_t full = X.plan->n_steps >= 32 ? 0xffffffffu : ((1u << X.plan->n_steps) - 1u);

	if ((X.finished & full) != full && t > X.plan->deadline_ms) {
		int first = -1;

		for (int i = 0; i < X.plan->n_steps && first < 0; i++) {
			if (X.state[i] == ST_RUNNING || X.state[i] == ST_PENDING) {
				first = i;
			}
		}
		emit("deadline_miss", "\"deadline_ms\":%u,\"code\":\"EXEC_TIMEOUT\"", (unsigned int)X.plan->deadline_ms);
		fail_run(PLANOSZ_EXEC_TIMEOUT, first);
		abort_run();
	}
}

/* ms until the next watchdog could fire (>= 1). Caller holds X.lock. */
static int32_t next_wake_ms(void)
{
	const uint32_t t = now_ms();
	uint32_t wait = X.plan->deadline_ms >= t ? X.plan->deadline_ms - t + 1 : 1;

	for (int i = 0; i < X.plan->n_steps; i++) {
		if (X.state[i] == ST_RUNNING) {
			const uint32_t budget_end = X.start_ms[i] + X.dv[i].wcet_ms;
			const uint32_t w = budget_end >= t ? budget_end - t + 1 : 1;

			if (w < wait) {
				wait = w;
			}
		}
	}
	return (int32_t)(wait > 0x7fffffffu ? 0x7fffffffu : wait);
}

struct planosz_exec_result planosz_exec_run_workers(const struct planosz_ir_plan *plan, unsigned int workers)
{
	const uint32_t full = plan->n_steps >= 32 ? 0xffffffffu : ((1u << plan->n_steps) - 1u);
	unsigned int started = 0;

	if (workers < 1) {
		workers = 1;
	}
	if (workers > PLANOSZ_EXEC_MAX_WORKERS) {
		workers = PLANOSZ_EXEC_MAX_WORKERS;
	}
	if (!X.locks_ready) {
		planosz_mutex_init(&X.lock);
		X.locks_ready = true;
	}
	memset(&X.state, 0, sizeof(X.state));
	memset(&X.out, 0, sizeof(X.out));
	memset(&X.start_ms, 0, sizeof(X.start_ms));
	X.plan = plan;
	X.hal = planosz_backend_ops();
	X.finished = 0;
	X.stop = false;
	X.aborting = false;
	X.in_flight = 0;
	X.n_workers = workers;
	X.r = (struct planosz_exec_result){ PLANOSZ_OK, 0, -1, 0 };
	X.t0 = X.hal->uptime_ms();
	planosz_sem_init(&X.wake, 0);
	planosz_res_reset();

	/* The device's own numbers - never the IR's. (Admission derived the same values.) */
	for (int i = 0; i < plan->n_steps; i++) {
		if (planosz_registry_derive(plan->steps[i].skill_idx, plan->steps[i].args, &X.dv[i]) != 0) {
			struct planosz_exec_result bad = { PLANOSZ_REJECT_BAD_ARGS, 0, i, 0 };

			return bad; /* never reached for admitted plans */
		}
	}

	planosz_mutex_lock(&X.lock);
	emit("run_start", "\"plan\":\"%s\",\"deadline_ms\":%u,\"steps\":%u,\"workers\":%u", plan->plan_id,
	     (unsigned int)plan->deadline_ms, (unsigned int)plan->n_steps, workers);
	planosz_mutex_unlock(&X.lock);

	for (unsigned int k = 0; k < workers; k++) {
		X.w[k].step = -1;
		planosz_sem_init(&X.w[k].go, 0);
		if (planosz_thread_start(&X.w[k].th, k, worker_main, &X.w[k]) != 0) {
			break;
		}
		started++;
	}
	X.n_workers = started;

	planosz_mutex_lock(&X.lock);
	if (started == 0) {
		X.aborting = true;
		fail_run(PLANOSZ_EXEC_BACKEND_UNAVAILABLE, -1);
	}
	while (!X.aborting) {
		check_watchdogs();
		if (X.aborting) {
			break;
		}
		dispatch_ready();
		if (X.aborting || (X.finished & full) == full) {
			break;
		}
		if (X.in_flight == 0) { /* nothing runnable, nothing running: cannot happen for an admitted DAG */
			fail_run(PLANOSZ_EXEC_CANCELLED, -1);
			X.aborting = true;
			break;
		}
		const int32_t wait = next_wake_ms();

		planosz_mutex_unlock(&X.lock);
		(void)planosz_sem_take(&X.wake, wait);
		planosz_mutex_lock(&X.lock);
	}
	/* drain: let in-flight skills return (they are bounded HAL calls), then stop the pool */
	while (X.in_flight > 0) {
		planosz_mutex_unlock(&X.lock);
		(void)planosz_sem_take(&X.wake, -1);
		planosz_mutex_lock(&X.lock);
	}
	X.stop = true;
	for (unsigned int k = 0; k < started; k++) {
		planosz_sem_give(&X.w[k].go);
	}
	planosz_mutex_unlock(&X.lock);
	for (unsigned int k = 0; k < started; k++) {
		planosz_thread_join(&X.w[k].th);
	}

	X.r.total_ms = now_ms();
	if (X.r.code == PLANOSZ_OK && X.r.total_ms > plan->deadline_ms) { /* finished, but after the deadline */
		emit("deadline_miss", "\"deadline_ms\":%u,\"code\":\"EXEC_TIMEOUT\"", (unsigned int)plan->deadline_ms);
		fail_run(PLANOSZ_EXEC_TIMEOUT, -1);
	}
	emit("run_end", "\"plan\":\"%s\",\"status\":\"%s\",\"code\":\"%s\",\"total_ms\":%u", plan->plan_id,
	     X.r.code == PLANOSZ_OK ? "OK" : "FAIL", planosz_code_str(X.r.code), (unsigned int)X.r.total_ms);
	return X.r;
}

struct planosz_exec_result planosz_exec_run(const struct planosz_ir_plan *plan)
{
	return planosz_exec_run_workers(plan, PLANOSZ_EXEC_WORKERS);
}
