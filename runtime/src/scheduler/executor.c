/* Executor v0.1: dependency-ordered (topological) execution of an admitted plan.
 * Enforces per-step WCET timeout, global deadline, resource locks, emits NDJSON. */
#include <stdbool.h>
#include "planosz/executor.h"
#include "planosz/hal.h"
#include "planosz/resource.h"
#include "planosz/skills.h"
#include "planosz/trace.h"

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

struct planosz_exec_result planosz_exec_run(const struct planosz_ir_plan *plan)
{
	const struct planosz_hal_ops *hal = planosz_backend_ops();
	int32_t out[PLANOSZ_MAX_STEPS] = { 0 };
	struct planosz_exec_result r = { PLANOSZ_OK, 0, -1 };
	const uint32_t t0 = hal->uptime_ms();

	planosz_res_reset();
	planosz_trace_emit(0, "run_start", "\"plan\":\"%s\",\"deadline_ms\":%u,\"steps\":%u", plan->plan_id,
			   (unsigned int)plan->deadline_ms, (unsigned int)plan->n_steps);

	for (int i = 0; i < plan->n_steps; i++) {
		const struct planosz_ir_step *s = &plan->steps[i];
		uint32_t now = hal->uptime_ms() - t0;

		if (now > plan->deadline_ms) {
			r.code = PLANOSZ_EXEC_TIMEOUT;
			r.failed_step = i;
			planosz_trace_emit(now, "deadline_miss", "\"deadline_ms\":%u,\"code\":\"EXEC_TIMEOUT\"",
					   (unsigned int)plan->deadline_ms);
			break;
		}
		if (s->when_step >= 0 && !cond_holds(s->when_op, out[s->when_step], s->when_value_milli)) {
			planosz_trace_emit(now, "step_skip", "\"step\":\"%s\",\"observed_milli\":%d", s->id,
					   (int)out[s->when_step]);
			continue;
		}
		if (planosz_res_acquire(s->excl_mask, s->shared_mask) != 0) {
			r.code = PLANOSZ_EXEC_SKILL_FAIL;
			r.failed_step = i;
			planosz_trace_emit(now, "step_error", "\"step\":\"%s\",\"code\":\"EXEC_SKILL_FAIL\","
					   "\"detail\":\"resource busy\"", s->id);
			break;
		}
		planosz_trace_emit(now, "step_start", "\"step\":\"%s\",\"skill\":%u", s->id, (unsigned int)s->skill_idx);

		enum planosz_code rc = planosz_skill_dispatch(s, &out[i]);
		uint32_t end = hal->uptime_ms() - t0;

		planosz_res_release(s->excl_mask, s->shared_mask);
		if (rc == PLANOSZ_OK && (end - now) > s->wcet_ms) {
			rc = PLANOSZ_EXEC_TIMEOUT;
		}
		if (rc != PLANOSZ_OK) {
			r.code = rc;
			r.failed_step = i;
			planosz_trace_emit(end, "step_error", "\"step\":\"%s\",\"code\":\"%s\"", s->id,
					   planosz_code_str(rc));
			break;
		}
		planosz_trace_emit(end, "step_end", "\"step\":\"%s\",\"status\":\"OK\",\"dur_ms\":%u,\"out_milli\":%d",
				   s->id, (unsigned int)(end - now), (int)out[i]);
	}

	r.total_ms = hal->uptime_ms() - t0;
	planosz_trace_emit(r.total_ms, "run_end", "\"plan\":\"%s\",\"status\":\"%s\",\"code\":\"%s\",\"total_ms\":%u",
			   plan->plan_id, r.code == PLANOSZ_OK ? "OK" : "FAIL", planosz_code_str(r.code),
			   (unsigned int)r.total_ms);
	return r;
}
