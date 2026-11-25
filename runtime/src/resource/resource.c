/* Runtime resource lock table. Admission guarantees no unordered conflicting steps, so a
 * failed acquire here means reality disagrees with the plan; the executor then aborts.
 * Shared claims are reference-counted (two parallel sensor reads share adc0). Not thread-safe
 * on its own: the executor calls it with its run lock held. */
#include "planosz/resource.h"

static uint32_t excl_held;
static uint8_t shared_count[32];

static uint32_t shared_held(void)
{
	uint32_t m = 0;

	for (unsigned int i = 0; i < 32; i++) {
		if (shared_count[i]) {
			m |= 1u << i;
		}
	}
	return m;
}

void planosz_res_reset(void)
{
	excl_held = 0;
	for (unsigned int i = 0; i < 32; i++) {
		shared_count[i] = 0;
	}
}

int planosz_res_acquire(uint32_t excl_mask, uint32_t shared_mask)
{
	if ((excl_mask & shared_mask) != 0) {
		return -1; /* same resource claimed in both modes by one step */
	}
	if ((excl_mask & (excl_held | shared_held())) || (shared_mask & excl_held)) {
		return -1;
	}
	excl_held |= excl_mask;
	for (unsigned int i = 0; i < 32; i++) {
		if (shared_mask & (1u << i)) {
			shared_count[i]++;
		}
	}
	return 0;
}

void planosz_res_release(uint32_t excl_mask, uint32_t shared_mask)
{
	excl_held &= ~excl_mask;
	for (unsigned int i = 0; i < 32; i++) {
		if ((shared_mask & (1u << i)) && shared_count[i] > 0) {
			shared_count[i]--;
		}
	}
}
