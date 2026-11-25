/* Executor: runs an ADMITTED plan as a DAG on a pool of worker threads.
 *
 * The calling thread is the coordinator: it keeps the ready-set (steps whose dependencies
 * finished or were skipped), evaluates `when` conditions, takes resource locks and hands
 * ready steps to idle workers - up to PLANOSZ_EXEC_WORKERS steps run concurrently. It also
 * runs two watchdogs: a per-step budget (the device-derived WCET) and the plan deadline.
 *
 * Limits (documented, not hidden): a skill cannot be preempted. When a watchdog fires, the
 * step is reported (EXEC_TIMEOUT / cancel) immediately, no new step is started, and the run
 * ends once the still-running skills return (they are HAL calls bounded by their contracts).
 * One run at a time (the ingress layer serialises submissions). */
#ifndef PLANOSZ_EXECUTOR_H_
#define PLANOSZ_EXECUTOR_H_

#include "planosz/errors.h"
#include "planosz/ir.h"

/* Worker count. Zephyr: CONFIG_PLANOSZ_EXECUTOR_PARALLEL (default y) + CONFIG_PLANOSZ_EXEC_WORKERS
 * (default 2); with the parallel option off the executor is single-worker (sequential).
 * Host tests: 2 by default, up to 8 via planosz_exec_run_workers(). */
#if defined(CONFIG_PLANOSZ_EXEC_WORKERS)
#define PLANOSZ_EXEC_WORKERS CONFIG_PLANOSZ_EXEC_WORKERS
#define PLANOSZ_EXEC_MAX_WORKERS CONFIG_PLANOSZ_EXEC_WORKERS
#elif defined(CONFIG_PLANOSZ)
#define PLANOSZ_EXEC_WORKERS 1
#define PLANOSZ_EXEC_MAX_WORKERS 1
#else
#define PLANOSZ_EXEC_WORKERS 2
#define PLANOSZ_EXEC_MAX_WORKERS 8
#endif
/* Admission on the device must use this same worker count (see planosz_policy.workers). */
#define PLANOSZ_EXECUTOR_PARALLEL (PLANOSZ_EXEC_WORKERS > 1)

struct planosz_exec_result {
	enum planosz_code code;
	uint32_t total_ms;
	int failed_step; /* -1 if none */
	uint8_t peak_concurrency; /* most steps that were in flight at once */
};

/* Run with the configured worker count. */
struct planosz_exec_result planosz_exec_run(const struct planosz_ir_plan *plan);
/* Run with `workers` workers (1 = sequential); clamped to 1..PLANOSZ_EXEC_MAX_WORKERS. */
struct planosz_exec_result planosz_exec_run_workers(const struct planosz_ir_plan *plan, unsigned int workers);

#endif /* PLANOSZ_EXECUTOR_H_ */
