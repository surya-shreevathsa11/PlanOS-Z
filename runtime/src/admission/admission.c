/* On-device admission: same stages and reason codes as tools/planoszc/planoszc/admission.py
 *   1 structure -> 2 per-step contract checks -> 3 resource conflicts -> 4 deadline vs WCET
 * No heap, no RTOS calls: O(n^2) over at most 32 steps. */
#include "planosz/admission.h"
#include "planosz/registry.h"

static struct planosz_admission_result fail(enum planosz_code code, int step, int other)
{
	struct planosz_admission_result r = { code, step, other, 0 };

	return r;
}

struct planosz_admission_result planosz_admit(const struct planosz_ir_plan *plan,
					      const struct planosz_policy *policy)
{
	uint32_t anc[PLANOSZ_MAX_STEPS];
	uint32_t finish[PLANOSZ_MAX_STEPS];
	struct planosz_admission_result res = { PLANOSZ_OK, -1, -1, 0 };

	/* ---- stage 1: structure ---- */
	if (plan == NULL || plan->steps == NULL || plan->n_steps == 0 ||
	    plan->n_steps > PLANOSZ_MAX_STEPS || plan->deadline_ms == 0) {
		return fail(PLANOSZ_REJECT_BAD_PLAN, -1, -1);
	}
	const int n = plan->n_steps;

	for (int i = 0; i < n; i++) {
		const struct planosz_ir_step *s = &plan->steps[i];
		/* deps must point at earlier steps => topological order, acyclic by construction */
		if (s->deps_mask & ~((1u << i) - 1u)) {
			return fail(PLANOSZ_REJECT_BAD_PLAN, i, -1);
		}
		anc[i] = s->deps_mask;
		for (int j = 0; j < i; j++) {
			if (s->deps_mask & (1u << j)) {
				anc[i] |= anc[j];
			}
		}
		if (s->when_step >= 0 && !(anc[i] & (1u << s->when_step))) {
			return fail(PLANOSZ_REJECT_BAD_PLAN, i, -1);
		}
	}

	/* ---- stage 2: per-step contract checks ---- */
	if (policy && (plan->perm_mask & ~policy->granted_perm_mask)) {
		return fail(PLANOSZ_REJECT_PERMISSION, -1, -1);
	}
	for (int i = 0; i < n; i++) {
		const struct planosz_ir_step *s = &plan->steps[i];
		const struct planosz_skill_contract *c = planosz_registry_get(s->skill_idx);

		if (c == NULL) {
			return fail(PLANOSZ_REJECT_UNKNOWN_SKILL, i, -1);
		}
		for (unsigned int a = 0; a < c->n_args; a++) {
			if (s->args[a] < c->args[a].min || s->args[a] > c->args[a].max) {
				return fail(PLANOSZ_REJECT_BAD_ARGS, i, -1);
			}
		}
		if (c->perm_mask & ~plan->perm_mask) {
			return fail(PLANOSZ_REJECT_PERMISSION, i, -1);
		}
		if (c->safety_class == PLANOSZ_HAZARDOUS && !(policy && policy->allow_hazardous)) {
			return fail(PLANOSZ_REJECT_SAFETY_CLASS, i, -1);
		}
		if (c->safety_class == PLANOSZ_REQUIRES_CONFIRM && !s->confirm) {
			return fail(PLANOSZ_REJECT_SAFETY_CLASS, i, -1);
		}
	}

	/* ---- stage 3: resource conflicts between steps with no ordering ---- */
	for (int i = 0; i < n; i++) {
		for (int j = i + 1; j < n; j++) {
			const struct planosz_ir_step *a = &plan->steps[i];
			const struct planosz_ir_step *b = &plan->steps[j];

			if (anc[j] & (1u << i)) {
				continue; /* j depends on i: can never overlap */
			}
			if (a->mutex_mask & (1u << j)) {
				continue; /* provably exclusive branches */
			}
			if ((a->excl_mask & (b->excl_mask | b->shared_mask)) ||
			    (b->excl_mask & (a->excl_mask | a->shared_mask))) {
				return fail(PLANOSZ_REJECT_RESOURCE_CONFLICT, i, j);
			}
		}
	}

	/* ---- stage 4: WCET vs deadline. Parallel executor: critical path; sequential: sum.
	 * All conditional branches are counted (conservative). ---- */
	uint32_t worst = 0;

	for (int i = 0; i < n; i++) {
		uint32_t start = 0;

		for (int j = 0; j < i; j++) {
			if ((plan->steps[i].deps_mask & (1u << j)) && finish[j] > start) {
				start = finish[j];
			}
		}
		if (policy && policy->sequential_executor) {
			start = worst; /* nothing overlaps: start after everything before it */
		}
		finish[i] = start + plan->steps[i].wcet_ms;
		if (finish[i] > worst) {
			worst = finish[i];
		}
	}
	res.wcet_ms = worst;
	if (worst > plan->deadline_ms) {
		res.code = PLANOSZ_REJECT_DEADLINE_INFEASIBLE;
	}
	return res;
}
