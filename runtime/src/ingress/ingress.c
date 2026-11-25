/* Ingress core: plan registry, upload slot glue and the serialised admit -> execute path.
 * Portable (host-tested with the pthread OSAL); the Zephyr shell commands are in ingress_shell.c. */
#include <errno.h>
#include <string.h>
#include "planosz/ingress.h"
#include "planosz/ir_wire.h"
#include "planosz/osal.h"
#include "planosz/planosz.h"

static struct {
	const char *name;
	const struct planosz_ir_plan *plan;
} plans[PLANOSZ_MAX_REGISTERED_PLANS];
static int n_plans;

static struct planosz_mutex lock;
static int lock_ready;

static const struct planosz_policy device_policy = {
	.granted_perm_mask = 0xffffffffu,
	.allow_hazardous = false,
	.workers = PLANOSZ_EXEC_WORKERS, /* must equal the executor's worker count */
};

static void lock_take(void)
{
	if (!lock_ready) { /* first use happens during single-threaded boot */
		planosz_mutex_init(&lock);
		lock_ready = 1;
	}
	planosz_mutex_lock(&lock);
}

static void lock_give(void)
{
	planosz_mutex_unlock(&lock);
}

int planosz_ingress_register(const char *name, const struct planosz_ir_plan *plan)
{
	if (n_plans >= PLANOSZ_MAX_REGISTERED_PLANS) {
		return -ENOMEM;
	}
	plans[n_plans].name = name;
	plans[n_plans].plan = plan;
	n_plans++;
	return 0;
}

const struct planosz_ir_plan *planosz_ingress_find(const char *name)
{
	for (int i = 0; i < n_plans; i++) {
		if (strcmp(plans[i].name, name) == 0) {
			return plans[i].plan;
		}
	}
	return strcmp(name, PLANOSZ_UPLOADED_PLAN) == 0 ? planosz_upload_plan() : NULL;
}

size_t planosz_ingress_count(void)
{
	return (size_t)n_plans;
}

const char *planosz_ingress_name(size_t idx)
{
	return idx < (size_t)n_plans ? plans[idx].name : NULL;
}

const struct planosz_policy *planosz_ingress_policy(void)
{
	return &device_policy;
}

enum planosz_code planosz_ingress_submit(const struct planosz_ir_plan *plan, bool execute)
{
	struct planosz_admission_result adm;
	enum planosz_code rc;

	lock_take();
	adm = planosz_admit(plan, &device_policy);
	planosz_trace_reset();
	planosz_trace_admit(plan, &adm, &device_policy); /* before any side effect */
	rc = adm.code;
	if (adm.code == PLANOSZ_OK && execute) {
		rc = planosz_exec_run(plan).code;
	}
	lock_give();
	return rc;
}

void planosz_ingress_load_begin(void)
{
	lock_take();
	planosz_upload_begin();
	lock_give();
}

enum planosz_code planosz_ingress_load_data(const char *b64)
{
	enum planosz_code rc;

	lock_take();
	rc = planosz_upload_data(b64, NULL);
	lock_give();
	return rc;
}

enum planosz_code planosz_ingress_load_end(void)
{
	const char *why = "";
	enum planosz_code rc;

	lock_take();
	rc = planosz_upload_end(&why);
	planosz_trace_reset();
	if (rc == PLANOSZ_OK) {
		planosz_trace_emit(0, "load", "\"plan\":\"%s\",\"status\":\"OK\",\"code\":\"OK\",\"bytes\":%u",
				   planosz_upload_plan()->plan_id, (unsigned int)planosz_upload_bytes());
	} else {
		planosz_trace_emit(0, "load", "\"plan\":\"-\",\"status\":\"REJECT\",\"code\":\"%s\",\"detail\":\"%s\","
				   "\"bytes\":%u", planosz_code_str(rc), why, (unsigned int)planosz_upload_bytes());
	}
	lock_give();
	return rc;
}
