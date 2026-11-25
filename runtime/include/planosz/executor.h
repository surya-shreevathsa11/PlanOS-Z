/* Executor: runs an ADMITTED plan in dependency order, with per-step timeout,
 * deadline watchdog and NDJSON tracing. v0.1: topological (sequential) order;
 * parallel DAG dispatch is roadmap phase 5. */
#ifndef PLANOSZ_EXECUTOR_H_
#define PLANOSZ_EXECUTOR_H_

#include "planosz/errors.h"
#include "planosz/ir.h"

/* The v0.1 executor is sequential, so admission on the device must use the sequential
 * (sum of WCETs) timing model. Flip to 1 only together with a parallel dispatcher. */
#define PLANOSZ_EXECUTOR_PARALLEL 0

struct planosz_exec_result {
	enum planosz_code code;
	uint32_t total_ms;
	int failed_step; /* -1 if none */
};

struct planosz_exec_result planosz_exec_run(const struct planosz_ir_plan *plan);

#endif /* PLANOSZ_EXECUTOR_H_ */
