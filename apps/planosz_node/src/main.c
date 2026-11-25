/* PlanOS-Z node: registers demo plans, wires the NDJSON trace to the console and (optionally)
 * submits ACCEPT / REJECT demos at boot. Interact afterwards via `planosz ...`
 * (plans, admit, run, load begin|data|end for UART IR upload). */
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include "demo_plans.h"
#include "planosz/planosz.h"

#ifdef CONFIG_BOARD_NATIVE_SIM
#include <posix_board_if.h>
#ifdef CONFIG_SHELL
#include <zephyr/shell/shell.h>

/* `sim_exit`: lets scripted shell sessions (CI) end the simulation cleanly. */
static int cmd_sim_exit(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(sh);
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);
	posix_exit(0);
	return 0;
}
SHELL_CMD_REGISTER(sim_exit, NULL, "Exit the native_sim process", cmd_sim_exit);
#endif
#endif

static void console_sink(const char *line, void *ctx)
{
	ARG_UNUSED(ctx);
	printk("%s\n", line);
}

#ifdef CONFIG_APP_AUTODEMO
static void demo(const char *label, const char *name)
{
	printk("\n== %s (plan '%s') ==\n", label, name);
	enum planosz_code rc = planosz_ingress_submit(planosz_ingress_find(name), true);

	printk("RESULT: %s\n", planosz_code_str(rc));
}
#endif

int main(void)
{
	planosz_trace_set_sink(console_sink, NULL);
	if (demo_plans_register() != 0) {
		printk("failed to register demo plans\n");
		return 0;
	}
	printk("PlanOS-Z node up: %u skills, board=%s, executor workers=%d\n",
	       (unsigned int)planosz_registry_count(), CONFIG_BOARD, PLANOSZ_EXEC_WORKERS);

#ifdef CONFIG_APP_AUTODEMO
	demo("ACCEPT: parallel sense -> branch -> report", "accept");
	demo("REJECT: deadline infeasible", "deadline");
	demo("REJECT: exclusive resource conflict", "conflict");
	demo("ACCEPT: three sensor sweeps on 2 workers (overlap)", "parallel");
	demo("REJECT: IR lies about WCET (1 ms) - device recomputes", "deadline_forged");
	demo("REJECT: IR strips resource masks - device recomputes", "conflict_forged");
#endif

#ifdef CONFIG_APP_EXIT_AFTER_DEMO
	posix_exit(0);
#endif
	return 0;
}
