/* Skill registry: static table generated from the IDL YAML files.
 *
 * The generated table is the DEVICE's source of truth for timing and resources: each skill
 * carries a WCET function and resource-claim templates compiled from its IDL contract. The
 * admission controller and the executor derive WCET / resource masks from these - never from
 * numbers shipped inside an uploaded plan. */
#ifndef PLANOSZ_REGISTRY_H_
#define PLANOSZ_REGISTRY_H_

#include <stddef.h>
#include <stdint.h>

#define PLANOSZ_MAX_ARGS 4
#define PLANOSZ_MAX_CLAIMS 4
#define PLANOSZ_MAX_CLAIM_VARIANTS 8 /* a claim template `out_{channel}` expands to <= 8 resources */

enum planosz_safety_class { PLANOSZ_SAFE = 0, PLANOSZ_REQUIRES_CONFIRM = 1, PLANOSZ_HAZARDOUS = 2 };
enum planosz_power_class { PLANOSZ_ACTIVE_HIGH = 0, PLANOSZ_ACTIVE_LOW = 1, PLANOSZ_SLEEP = 2 };
enum planosz_claim_mode { PLANOSZ_CLAIM_SHARED = 0, PLANOSZ_CLAIM_EXCLUSIVE = 1 };

struct planosz_arg_spec {
	const char *name;
	int64_t min; /* enums: 0..n-1 index; floats: milli-units */
	int64_t max;
};

/* One resource claim of a skill. `arg < 0`: fixed resource res[0]. Otherwise the resource is
 * res[args[arg]] (the claim template referenced an enum argument, e.g. "out_{channel}").
 * `res[]` holds indices into planosz_res_names[] (the device's global resource bit space). */
struct planosz_claim {
	uint8_t mode; /* enum planosz_claim_mode */
	int8_t arg;
	uint8_t n_variants;
	uint8_t res[PLANOSZ_MAX_CLAIM_VARIANTS];
};

/* Worst-case execution time in ms for the resolved (already range-checked) args. */
typedef uint32_t (*planosz_wcet_fn)(const int32_t *args);

struct planosz_skill_contract {
	const char *skill_id;
	uint16_t version;
	uint32_t perm_mask;
	uint8_t safety_class;
	uint8_t power_class;
	uint8_t n_args;
	struct planosz_arg_spec args[PLANOSZ_MAX_ARGS];
	uint8_t n_claims;
	struct planosz_claim claims[PLANOSZ_MAX_CLAIMS];
	planosz_wcet_fn wcet_fn;
	const char *wcet_model; /* the IDL expression the wcet_fn was generated from (display only) */
};

/* What the device itself concludes about one step. */
struct planosz_derived {
	uint32_t wcet_ms;
	uint32_t excl_mask;   /* bits in the device resource space (planosz_res_names[]) */
	uint32_t shared_mask;
};

size_t planosz_registry_count(void);
const struct planosz_skill_contract *planosz_registry_get(unsigned int idx);
int planosz_registry_find(const char *skill_id); /* index or -1 */

/* Compute WCET + resource masks for `skill_idx` with `args` from the on-device contract.
 * Returns 0, or -1 if the skill is unknown or an enum arg selects a claim variant that
 * does not exist (args must already be range-checked by admission). */
int planosz_registry_derive(unsigned int skill_idx, const int32_t *args, struct planosz_derived *out);

#endif /* PLANOSZ_REGISTRY_H_ */
