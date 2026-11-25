/* PlanOS-Z IR v1: what `planoszc compile` hands to the device. Steps are stored in
 * topological order; dependencies and resources are bit-masks (max 32 each). */
#ifndef PLANOSZ_IR_H_
#define PLANOSZ_IR_H_

#include <stdbool.h>
#include <stdint.h>
#include "planosz/registry.h"

#define PLANOSZ_IR_VERSION 1
#define PLANOSZ_MAX_STEPS 32

enum planosz_cmp { PLANOSZ_CMP_GT, PLANOSZ_CMP_GE, PLANOSZ_CMP_LT, PLANOSZ_CMP_LE, PLANOSZ_CMP_EQ, PLANOSZ_CMP_NE };

struct planosz_ir_step {
	const char *id;
	uint8_t skill_idx;
	uint32_t wcet_ms;     /* worst-case, computed by the host from the contract */
	uint32_t deps_mask;   /* steps that must complete (or be skipped) first */
	uint32_t excl_mask;   /* resources claimed exclusively */
	uint32_t shared_mask; /* resources claimed in shared mode */
	uint32_t mutex_mask;  /* steps that can never run together with this one */
	uint8_t confirm;      /* plan author explicitly confirmed this step */
	int32_t args[PLANOSZ_MAX_ARGS]; /* resolved: enum = index, float = milli-units */
	int8_t when_step;     /* -1 = unconditional */
	uint8_t when_op;
	int32_t when_value_milli;
};

struct planosz_ir_plan {
	const char *plan_id;
	uint32_t deadline_ms;
	uint32_t perm_mask;
	uint8_t n_steps;
	const struct planosz_ir_step *steps;
};

#endif /* PLANOSZ_IR_H_ */
