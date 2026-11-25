/* Zephyr shell front-end (CONFIG_PLANOSZ_SHELL):
 *   planosz status               runtime summary
 *   planosz skills               registered contracts (incl. device WCET model)
 *   planosz plans                registered + uploaded plans
 *   planosz admit <name>         admission decision only (NDJSON on the trace sink)
 *   planosz run <name>           admit, then execute if accepted
 *   planosz load begin           start an IR upload (clears the "uploaded" slot)
 *   planosz load data <base64>   append a chunk
 *   planosz load end             verify CRC/version and arm the "uploaded" plan
 * `planoszc compile <plan.yaml> --emit uart` prints exactly these lines. */
#include <zephyr/kernel.h>
#include <zephyr/shell/shell.h>
#include "planosz/planosz.h"

static int cmd_status(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);
	shell_print(sh, "PlanOS-Z runtime: ir_version=%d skills=%u workers=%d uptime_ms=%u uploaded=%s",
		    PLANOSZ_IR_VERSION, (unsigned int)planosz_registry_count(), PLANOSZ_EXEC_WORKERS,
		    (unsigned int)k_uptime_get_32(), planosz_upload_plan() ? "yes" : "no");
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
	for (size_t i = 0; i < planosz_ingress_count(); i++) {
		const char *name = planosz_ingress_name(i);
		const struct planosz_ir_plan *p = planosz_ingress_find(name);

		shell_print(sh, "%s (%s, %u steps)", name, p->plan_id, p->n_steps);
	}
	if (planosz_upload_plan() != NULL) {
		shell_print(sh, "%s (%s, %u steps)", PLANOSZ_UPLOADED_PLAN, planosz_upload_plan()->plan_id,
			    planosz_upload_plan()->n_steps);
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

static int cmd_load_begin(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);
	planosz_ingress_load_begin();
	shell_print(sh, "load: ready");
	return 0;
}

static int cmd_load_data(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	enum planosz_code rc = planosz_ingress_load_data(argv[1]);

	if (rc != PLANOSZ_OK) {
		shell_error(sh, "load: %s (chunk rejected)", planosz_code_str(rc));
		return -EINVAL;
	}
	return 0;
}

static int cmd_load_end(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);
	enum planosz_code rc = planosz_ingress_load_end();

	if (rc != PLANOSZ_OK) {
		shell_error(sh, "load: %s", planosz_code_str(rc));
		return -EINVAL;
	}
	shell_print(sh, "load: OK %s (%u bytes) -> planosz admit|run %s", planosz_upload_plan()->plan_id,
		    (unsigned int)planosz_upload_bytes(), PLANOSZ_UPLOADED_PLAN);
	return 0;
}

SHELL_STATIC_SUBCMD_SET_CREATE(planosz_load_cmds,
	SHELL_CMD_ARG(begin, NULL, "Start an IR upload", cmd_load_begin, 1, 0),
	SHELL_CMD_ARG(data, NULL, "<base64> Append a chunk", cmd_load_data, 2, 0),
	SHELL_CMD_ARG(end, NULL, "Verify CRC/version, arm the 'uploaded' plan", cmd_load_end, 1, 0),
	SHELL_SUBCMD_SET_END);

SHELL_STATIC_SUBCMD_SET_CREATE(planosz_cmds,
	SHELL_CMD_ARG(status, NULL, "Runtime summary", cmd_status, 1, 0),
	SHELL_CMD_ARG(skills, NULL, "List registered skill contracts", cmd_skills, 1, 0),
	SHELL_CMD_ARG(plans, NULL, "List plans (registered + uploaded)", cmd_plans, 1, 0),
	SHELL_CMD_ARG(admit, NULL, "<plan> Admission decision only", cmd_admit, 2, 0),
	SHELL_CMD_ARG(run, NULL, "<plan> Admit, then execute", cmd_run, 2, 0),
	SHELL_CMD(load, &planosz_load_cmds, "Upload an IR plan over the shell/UART", NULL),
	SHELL_SUBCMD_SET_END);
SHELL_CMD_REGISTER(planosz, &planosz_cmds, "PlanOS-Z job-pack runtime", NULL);
