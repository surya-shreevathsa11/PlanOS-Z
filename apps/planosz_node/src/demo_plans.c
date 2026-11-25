/* Demo plans compiled from examples/missions/ by `planoszc compile --emit c`
 * (regenerate with `make app-plans`). The two REJECT plans are emitted with --force so the
 * DEVICE proves it re-rejects what the host would also reject: never trust the host blindly. */
#include "demo_plans.h"
#include "plan_accept.gen.h"
#include "plan_conflict.gen.h"
#include "plan_deadline.gen.h"
#include "planosz/planosz.h"

int demo_plans_register(void)
{
	int rc = planosz_ingress_register("accept", &PLAN_ACCEPT);

	rc |= planosz_ingress_register("deadline", &PLAN_DEADLINE);
	rc |= planosz_ingress_register("conflict", &PLAN_CONFLICT);
	return rc;
}
