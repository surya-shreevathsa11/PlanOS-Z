/* Ingress (plan registry + submit path) and, with CONFIG_PLANOSZ_SHELL, Zephyr shell commands:
 *   planosz status            runtime summary
 *   planosz skills            registered contracts
 *   planosz plans             plans registered by the app
 *   planosz admit <name>      admission decision only (NDJSON on the trace sink)
 *   planosz run <name>        admit, then execute if accepted
 */
#include <string.h>
#include <zephyr/kernel.h>
#ifdef CONFIG_PLANOSZ_SHELL
#include <zephyr/shell/shell.h>
#endif
#include "planosz/planosz.h"
#include "planosz/skill_table.gen.h"

static struct {
	const char *name;
	const struct planosz_ir_plan *plan;
} plans[PLANOSZ_MAX_REGISTERED_PLANS];
static int n_plans;

static const struct planosz_policy device_policy = {
	.granted_perm_mask = 0xffffffffu,
	.allow_hazardous = false,
	.sequential_executor = !PLANOSZ_EXECUTOR_PARALLEL,
};

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
	return NULL;
}

enum planosz_code planosz_ingress_submit(const struct planosz_ir_plan *plan, bool execute)
{
	struct planosz_admission_result adm = planosz_admit(plan, &device_policy);

	planosz_trace_reset();
	planosz_trace_admit(plan, &adm); /* before any side effect */
	if (adm.code != PLANOSZ_OK || !execute) {
		return adm.code;
	}
	return planosz_exec_run(plan).code;
}

#ifdef CONFIG_PLANOSZ_SHELL
static int cmd_status(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);
	shell_print(sh, "PlanOS-Z runtime: ir_version=%d skills=%u plans=%d uptime_ms=%u", PLANOSZ_IR_VERSION,
		    (unsigned int)planosz_registry_count(), n_plans, (unsigned int)k_uptime_get_32());
	return 0;
}

static int cmd_skills(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);
	for (unsigned int i = 0; i < planosz_registry_count(); i++) {
		const struct planosz_skill_contract *c = planosz_registry_get(i);

		shell_print(sh, "%-18s v%u safety=%u args=%u wcet=\"%s\"", c->skill_id, c->version, c->safety_class,
			    c->n_args, c->wcet_model);
	}
	return 0;
}

static int cmd_plans(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);
	for (int i = 0; i < n_plans; i++) {
		shell_print(sh, "%s (%s, %u steps)", plans[i].name, plans[i].plan->plan_id, plans[i].plan->n_steps);
	}
	return 0;
}

static int submit_named(const struct shell *sh, const char *name, bool execute)
{
	const struct planosz_ir_plan *p = planosz_ingress_find(name);

	if (p == NULL) {
		shell_error(sh, "unknown plan '%s' (try: planosz plans)", name);
		return -ENOENT;
	}
	enum planosz_code rc = planosz_ingress_submit(p, execute);

	shell_print(sh, "%s %s", rc == PLANOSZ_OK ? "OK" : "FAILED", planosz_code_str(rc));
	return 0;
}

static int cmd_admit(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	return submit_named(sh, argv[1], false);
}

static int cmd_run(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	return submit_named(sh, argv[1], true);
}

SHELL_STATIC_SUBCMD_SET_CREATE(planosz_cmds,
	SHELL_CMD_ARG(status, NULL, "Runtime summary", cmd_status, 1, 0),
	SHELL_CMD_ARG(skills, NULL, "List registered skill contracts", cmd_skills, 1, 0),
	SHELL_CMD_ARG(plans, NULL, "List plans registered by the app", cmd_plans, 1, 0),
	SHELL_CMD_ARG(admit, NULL, "<plan> Admission decision only", cmd_admit, 2, 0),
	SHELL_CMD_ARG(run, NULL, "<plan> Admit, then execute", cmd_run, 2, 0),
	SHELL_SUBCMD_SET_END);
SHELL_CMD_REGISTER(planosz, &planosz_cmds, "PlanOS-Z job-pack runtime", NULL);
#endif /* CONFIG_PLANOSZ_SHELL */
