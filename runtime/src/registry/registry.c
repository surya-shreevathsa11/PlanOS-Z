#include <string.h>
#include "planosz/registry.h"
#include "planosz/skill_table.gen.h"

size_t planosz_registry_count(void)
{
	return PLANOSZ_SKILL_COUNT;
}

const struct planosz_skill_contract *planosz_registry_get(unsigned int idx)
{
	return idx < PLANOSZ_SKILL_COUNT ? &planosz_skill_table[idx] : NULL;
}

int planosz_registry_find(const char *skill_id)
{
	for (unsigned int i = 0; i < PLANOSZ_SKILL_COUNT; i++) {
		if (strcmp(planosz_skill_table[i].skill_id, skill_id) == 0) {
			return (int)i;
		}
	}
	return -1;
}

int planosz_registry_derive(unsigned int skill_idx, const int32_t *args, struct planosz_derived *out)
{
	const struct planosz_skill_contract *c = planosz_registry_get(skill_idx);

	if (c == NULL || args == NULL || out == NULL) {
		return -1;
	}
	out->wcet_ms = c->wcet_fn(args);
	out->excl_mask = 0;
	out->shared_mask = 0;
	for (unsigned int k = 0; k < c->n_claims; k++) {
		const struct planosz_claim *cl = &c->claims[k];
		unsigned int variant = 0;
		uint32_t bit;

		if (cl->arg >= 0) {
			if (args[cl->arg] < 0 || (uint32_t)args[cl->arg] >= cl->n_variants) {
				return -1;
			}
			variant = (unsigned int)args[cl->arg];
		}
		bit = 1u << cl->res[variant];
		if (cl->mode == PLANOSZ_CLAIM_EXCLUSIVE) {
			out->excl_mask |= bit;
		} else {
			out->shared_mask |= bit;
		}
	}
	return 0;
}
