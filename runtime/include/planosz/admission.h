/* On-device admission controller: re-validates a compiled plan before any side effect.
 * Pure C, no RTOS dependency, so it is unit-tested on the host with plain gcc. */
#ifndef PLANOSZ_ADMISSION_H_
#define PLANOSZ_ADMISSION_H_

#include <stdbool.h>
#include "planosz/errors.h"
#include "planosz/ir.h"

struct planosz_policy {
	uint32_t granted_perm_mask;
	bool allow_hazardous;
	/* true: the executor runs steps one after another -> WCET bound is the SUM of step WCETs.
	 * false: concurrent steps overlap -> WCET bound is the critical path. Must match the executor. */
	bool sequential_executor;
};

struct planosz_admission_result {
	enum planosz_code code; /* PLANOSZ_OK == ACCEPT */
	int step;               /* offending step index, or -1 */
	int other_step;         /* second step for conflicts, or -1 */
	uint32_t wcet_ms;       /* critical-path WCET (valid when timing stage was reached) */
};

struct planosz_admission_result planosz_admit(const struct planosz_ir_plan *plan,
					      const struct planosz_policy *policy);

#endif /* PLANOSZ_ADMISSION_H_ */
