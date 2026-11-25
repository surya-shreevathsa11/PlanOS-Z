/* On-device admission controller: re-validates a plan before any side effect.
 *
 * Trust boundary: the host-supplied `wcet_ms`, `excl_mask`, `shared_mask` and `mutex_mask`
 * fields of the IR are IGNORED for every decision. The device recomputes WCET and resource
 * claims from its own generated skill table (planosz_registry_derive) and derives branch
 * exclusivity from the `when` conditions. What the plan can influence is limited to: which
 * skills, their (range-checked) args, dependencies, conditions, permissions and deadline.
 * Pure C, no RTOS dependency, so it is unit-tested on the host with plain gcc. */
#ifndef PLANOSZ_ADMISSION_H_
#define PLANOSZ_ADMISSION_H_

#include <stdbool.h>
#include "planosz/errors.h"
#include "planosz/ir.h"

struct planosz_policy {
	uint32_t granted_perm_mask;
	bool allow_hazardous;
	/* Executor model the WCET bound must match (it MUST equal the executor's worker count):
	 *   0   unbounded parallelism      -> critical path
	 *   1   sequential executor        -> sum of step WCETs
	 *   N>1 N workers                  -> critical path if the plan can never run more than N
	 *                                     steps at once, else Graham's list-scheduling bound
	 *                                     min(sum, cp + ceil((sum - cp) / N)). */
	uint8_t workers;
};

struct planosz_admission_result {
	enum planosz_code code; /* PLANOSZ_OK == ACCEPT */
	int step;               /* offending step index, or -1 */
	int other_step;         /* second step for conflicts, or -1 */
	uint32_t wcet_ms;       /* WCET bound for the policy's executor (valid when timing stage reached) */
	uint32_t critical_path_ms;
	uint8_t max_concurrency; /* most steps that could ever run at once (valid when timing stage reached) */
	uint8_t ir_wcet_mismatch; /* steps whose IR-claimed wcet_ms differs from the device's value (info) */
};

struct planosz_admission_result planosz_admit(const struct planosz_ir_plan *plan,
					      const struct planosz_policy *policy);

/* Two steps whose `when` conditions are complementary tests of the same observation can
 * never both run. Derived from device-visible fields only. */
bool planosz_steps_mutually_exclusive(const struct planosz_ir_step *a, const struct planosz_ir_step *b);

#endif /* PLANOSZ_ADMISSION_H_ */
