/* On-device admission: same stages and reason codes as tools/planoszc/planoszc/admission.py
 *   1 structure -> 2 per-step contract checks -> 3 resource conflicts -> 4 deadline vs WCET
 * No heap, no RTOS calls: O(n^2) over at most 32 steps (plus a small max-clique search).
 *
 * WCET, resource masks and branch exclusivity are RECOMPUTED here from the generated skill
 * table; the IR's wcet_ms / excl_mask / shared_mask / mutex_mask are never used for a decision. */
#include "planosz/admission.h"
#include "planosz/registry.h"

static struct planosz_admission_result fail(enum planosz_code code, int step, int other)
{
	struct planosz_admission_result r = { code, step, other, 0, 0, 0, 0 };

	return r;
}

static int popcount32(uint32_t v)
{
	int n = 0;

	while (v) {
		v &= v - 1u;
		n++;
	}
	return n;
}

static int ctz32(uint32_t v)
{
	int n = 0;

	while (!(v & 1u)) {
		v >>= 1;
		n++;
	}
	return n;
}

static int complement_op(uint8_t op)
{
	switch (op) {
	case PLANOSZ_CMP_GT: return PLANOSZ_CMP_LE;
	case PLANOSZ_CMP_LE: return PLANOSZ_CMP_GT;
	case PLANOSZ_CMP_GE: return PLANOSZ_CMP_LT;
	case PLANOSZ_CMP_LT: return PLANOSZ_CMP_GE;
	case PLANOSZ_CMP_EQ: return PLANOSZ_CMP_NE;
	case PLANOSZ_CMP_NE: return PLANOSZ_CMP_EQ;
	default: return -1;
	}
}

bool planosz_steps_mutually_exclusive(const struct planosz_ir_step *a, const struct planosz_ir_step *b)
{
	return a->when_step >= 0 && a->when_step == b->when_step && a->when_value_milli == b->when_value_milli &&
	       complement_op(a->when_op) == (int)b->when_op;
}

/* Bron-Kerbosch with pivoting: size of the largest set of pairwise-concurrent steps. */
static void max_clique(int size, uint32_t p, uint32_t x, const uint32_t *adj, int *best)
{
	uint32_t cand;
	int pivot = -1;
	int pivot_cnt = -1;

	if (p == 0 && x == 0) {
		if (size > *best) {
			*best = size;
		}
		return;
	}
	if (size + popcount32(p) <= *best) {
		return; /* cannot beat the best clique found so far */
	}
	for (uint32_t m = p | x; m; m &= m - 1u) {
		int u = ctz32(m);
		int cnt = popcount32(p & adj[u]);

		if (cnt > pivot_cnt) {
			pivot_cnt = cnt;
			pivot = u;
		}
	}
	cand = p & ~adj[pivot];
	while (cand) {
		int v = ctz32(cand);
		uint32_t bit = 1u << v;

		max_clique(size + 1, p & adj[v], x & adj[v], adj, best);
		p &= ~bit;
		x |= bit;
		cand &= ~bit;
	}
}

struct planosz_admission_result planosz_admit(const struct planosz_ir_plan *plan,
					      const struct planosz_policy *policy)
{
	uint32_t anc[PLANOSZ_MAX_STEPS];
	uint64_t finish[PLANOSZ_MAX_STEPS];
	uint32_t adj[PLANOSZ_MAX_STEPS];
	struct planosz_derived dv[PLANOSZ_MAX_STEPS];
	struct planosz_admission_result res = { PLANOSZ_OK, -1, -1, 0, 0, 0, 0 };

	/* ---- stage 1: structure ---- */
	if (plan == NULL || plan->steps == NULL || plan->n_steps == 0 ||
	    plan->n_steps > PLANOSZ_MAX_STEPS || plan->deadline_ms == 0) {
		return fail(PLANOSZ_REJECT_BAD_PLAN, -1, -1);
	}
	const int n = plan->n_steps;

	for (int i = 0; i < n; i++) {
		const struct planosz_ir_step *s = &plan->steps[i];

		if (s->id == NULL) {
			return fail(PLANOSZ_REJECT_BAD_PLAN, i, -1);
		}
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
		if (s->when_step < -1 || s->when_step >= i || (s->when_step >= 0 && s->when_op > PLANOSZ_CMP_NE)) {
			return fail(PLANOSZ_REJECT_BAD_PLAN, i, -1);
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

	/* ---- device-owned timing + resources: derived from the skill table, not from the IR ---- */
	for (int i = 0; i < n; i++) {
		const struct planosz_ir_step *s = &plan->steps[i];

		if (planosz_registry_derive(s->skill_idx, s->args, &dv[i]) != 0) {
			return fail(PLANOSZ_REJECT_BAD_ARGS, i, -1);
		}
		if (s->wcet_ms != dv[i].wcet_ms && res.ir_wcet_mismatch < UINT8_MAX) {
			res.ir_wcet_mismatch++;
		}
	}

	/* ---- stage 3: resource conflicts between steps with no ordering ---- */
	for (int i = 0; i < n; i++) {
		for (int j = i + 1; j < n; j++) {
			if (anc[j] & (1u << i)) {
				continue; /* j depends on i: can never overlap */
			}
			if (planosz_steps_mutually_exclusive(&plan->steps[i], &plan->steps[j])) {
				continue; /* provably exclusive branches (device-derived) */
			}
			if ((dv[i].excl_mask & (dv[j].excl_mask | dv[j].shared_mask)) ||
			    (dv[j].excl_mask & (dv[i].excl_mask | dv[i].shared_mask))) {
				struct planosz_admission_result r = fail(PLANOSZ_REJECT_RESOURCE_CONFLICT, i, j);

				r.ir_wcet_mismatch = res.ir_wcet_mismatch;
				return r;
			}
		}
	}

	/* ---- stage 4: WCET vs deadline. All conditional branches are counted (conservative). ---- */
	uint64_t cp = 0;
	uint64_t sum = 0;

	for (int i = 0; i < n; i++) {
		uint64_t start = 0;

		for (int j = 0; j < i; j++) {
			if ((plan->steps[i].deps_mask & (1u << j)) && finish[j] > start) {
				start = finish[j];
			}
		}
		finish[i] = start + dv[i].wcet_ms;
		if (finish[i] > cp) {
			cp = finish[i];
		}
		sum += dv[i].wcet_ms;
	}

	/* most steps that can ever run at the same time: largest pairwise-concurrent set */
	int omega = 0;

	for (int i = 0; i < n; i++) {
		adj[i] = 0;
		for (int j = 0; j < n; j++) {
			int lo = i < j ? i : j;
			int hi = i < j ? j : i;

			if (i == j || (anc[hi] & (1u << lo)) ||
			    planosz_steps_mutually_exclusive(&plan->steps[lo], &plan->steps[hi])) {
				continue;
			}
			adj[i] |= 1u << j;
		}
	}
	max_clique(0, n >= 32 ? 0xffffffffu : ((1u << n) - 1u), 0, adj, &omega);

	const uint64_t w = policy ? policy->workers : 0;
	uint64_t bound;

	if (w == 0 || (w > 1 && (uint64_t)omega <= w)) {
		bound = cp; /* every ready step always finds a free worker */
	} else if (w == 1) {
		bound = sum;
	} else {
		uint64_t graham = cp + (sum - cp + w - 1) / w;

		bound = graham < sum ? graham : sum;
	}
	res.wcet_ms = bound > UINT32_MAX ? UINT32_MAX : (uint32_t)bound;
	res.critical_path_ms = cp > UINT32_MAX ? UINT32_MAX : (uint32_t)cp;
	res.max_concurrency = (uint8_t)omega;
	if (bound > plan->deadline_ms) {
		res.code = PLANOSZ_REJECT_DEADLINE_INFEASIBLE;
	}
	return res;
}
