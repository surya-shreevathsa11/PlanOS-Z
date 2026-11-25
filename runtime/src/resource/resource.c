#include "planosz/resource.h"

static uint32_t excl_held;
static uint32_t shared_held;

void planosz_res_reset(void)
{
	excl_held = 0;
	shared_held = 0;
}

int planosz_res_acquire(uint32_t excl_mask, uint32_t shared_mask)
{
	if ((excl_mask & (excl_held | shared_held)) || (shared_mask & excl_held)) {
		return -1;
	}
	excl_held |= excl_mask;
	shared_held |= shared_mask; /* v0.1: shared claims are not ref-counted (sequential executor) */
	return 0;
}

void planosz_res_release(uint32_t excl_mask, uint32_t shared_mask)
{
	excl_held &= ~excl_mask;
	shared_held &= ~shared_mask;
}
