/* Skill registry: static table generated from the IDL YAML files. */
#ifndef PLANOSZ_REGISTRY_H_
#define PLANOSZ_REGISTRY_H_

#include <stddef.h>
#include <stdint.h>

#define PLANOSZ_MAX_ARGS 4

enum planosz_safety_class { PLANOSZ_SAFE = 0, PLANOSZ_REQUIRES_CONFIRM = 1, PLANOSZ_HAZARDOUS = 2 };
enum planosz_power_class { PLANOSZ_ACTIVE_HIGH = 0, PLANOSZ_ACTIVE_LOW = 1, PLANOSZ_SLEEP = 2 };

struct planosz_arg_spec {
	const char *name;
	int64_t min; /* enums: 0..n-1 index; floats: milli-units */
	int64_t max;
};

struct planosz_skill_contract {
	const char *skill_id;
	uint16_t version;
	uint32_t perm_mask;
	uint8_t safety_class;
	uint8_t power_class;
	uint8_t n_args;
	struct planosz_arg_spec args[PLANOSZ_MAX_ARGS];
	const char *wcet_model; /* documentation only; WCET is baked into the IR by the host */
};

size_t planosz_registry_count(void);
const struct planosz_skill_contract *planosz_registry_get(unsigned int idx);
int planosz_registry_find(const char *skill_id); /* index or -1 */

#endif /* PLANOSZ_REGISTRY_H_ */
