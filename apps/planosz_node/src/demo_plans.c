/* Demo plans compiled from examples/missions/ by `planoszc compile --emit c`
 * (regenerate with `make app-plans`). Embedded plans keep CI deterministic; plans can also
 * be uploaded at runtime over the shell (`planosz load ...`, slot "uploaded").
 *
 * The REJECT plans are emitted with --force so the DEVICE proves it re-rejects what the host
 * would also reject. The *_forged plans additionally carry FALSIFIED host claims (WCET = 1 ms,
 * resource masks stripped): the device must still reject them because it recomputes both
 * from its own skill table instead of trusting the IR. */
#include "demo_plans.h"
#include "plan_accept.gen.h"
#include "plan_conflict.gen.h"
#include "plan_conflict_forged.gen.h"
#include "plan_deadline.gen.h"
#include "plan_deadline_forged.gen.h"
#include "plan_parallel.gen.h"
#include "planosz/planosz.h"

int demo_plans_register(void)
{
	int rc = planosz_ingress_register("accept", &PLAN_ACCEPT);

	rc |= planosz_ingress_register("deadline", &PLAN_DEADLINE);
	rc |= planosz_ingress_register("conflict", &PLAN_CONFLICT);
	rc |= planosz_ingress_register("parallel", &PLAN_PARALLEL);
	rc |= planosz_ingress_register("deadline_forged", &PLAN_DEADLINE_FORGED);
	rc |= planosz_ingress_register("conflict_forged", &PLAN_CONFLICT_FORGED);
	return rc;
}
