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
