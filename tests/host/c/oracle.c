/* Line-oriented oracle around the REAL runtime sources, driven by the Python tests:
 *   derive <skill> <a0> <a1> <a2> <a3>           -> D <wcet_ms> <excl_mask> <shared_mask>
 *   resources                                    -> R <bit> <name>   (device resource space)
 *   plan <deadline_ms> <perm_mask> <workers> <granted_mask> <allow_hazardous>
 *   step <skill> <deps> <confirm> <when_step> <when_op> <when_milli> <a0> <a1> <a2> <a3>
 *        <ir_wcet> <ir_excl> <ir_shared> <ir_mutex>
 *   admit                                        -> A <code> <step> <other> <wcet> <cp> <omega> <mismatch>
 *   wire <base64 chunk>... / wire-end            -> W <code> <reason> ; then P/S lines of the decoded plan
 * Differential tests compare these answers with planoszc's Python implementation. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "planosz/planosz.h"
#include "planosz/skill_table.gen.h"

/* the oracle never executes plans; the HAL is only needed to link */
static int h_sensor(uint8_t i, int32_t *m) { (void)i; *m = 0; return 0; }
static int h_out(uint8_t c, bool on) { (void)c; (void)on; return 0; }
static int h_delay(uint32_t ms) { (void)ms; return 0; }
static int h_idle(uint32_t ms, bool d) { (void)ms; (void)d; return 0; }
static int h_info(uint8_t f, char *b, size_t l) { (void)f; (void)l; b[0] = 0; return 0; }
static uint32_t h_up(void) { return 0; }
static const struct planosz_hal_ops ops = { h_sensor, h_out, h_delay, h_idle, h_info, h_up };
const struct planosz_hal_ops *planosz_backend_ops(void) { return &ops; }

static struct planosz_ir_step steps[PLANOSZ_MAX_STEPS];
static char ids[PLANOSZ_MAX_STEPS][8];
static struct planosz_ir_plan plan;
static struct planosz_policy policy;

int main(void)
{
	char cmd[32];

	while (scanf("%31s", cmd) == 1) {
		if (!strcmp(cmd, "derive")) {
			unsigned int sk;
			int32_t a[4];
			struct planosz_derived d;

			scanf("%u %d %d %d %d", &sk, &a[0], &a[1], &a[2], &a[3]);
			if (planosz_registry_derive(sk, a, &d) == 0) {
				printf("D %u %u %u\n", (unsigned int)d.wcet_ms, (unsigned int)d.excl_mask,
				       (unsigned int)d.shared_mask);
			} else {
				printf("D -1 0 0\n");
			}
		} else if (!strcmp(cmd, "resources")) {
			for (int i = 0; i < PLANOSZ_RES_COUNT; i++) {
				printf("R %d %s\n", i, planosz_res_names[i]);
			}
		} else if (!strcmp(cmd, "plan")) {
			unsigned int dl, perm, w, granted, haz;

			scanf("%u %u %u %u %u", &dl, &perm, &w, &granted, &haz);
			memset(steps, 0, sizeof(steps));
			plan = (struct planosz_ir_plan){ "oracle", dl, perm, 0, steps };
			policy = (struct planosz_policy){ granted, haz != 0, (uint8_t)w };
		} else if (!strcmp(cmd, "step")) {
			struct planosz_ir_step *s = &steps[plan.n_steps];
			unsigned int sk, deps, conf, wop;
			int ws, wv;

			scanf("%u %u %u %d %u %d %d %d %d %d %u %u %u %u", &sk, &deps, &conf, &ws, &wop, &wv, &s->args[0],
			      &s->args[1], &s->args[2], &s->args[3], &s->wcet_ms, &s->excl_mask, &s->shared_mask,
			      &s->mutex_mask);
			snprintf(ids[plan.n_steps], sizeof(ids[0]), "s%u", (unsigned int)plan.n_steps);
			s->id = ids[plan.n_steps];
			s->skill_idx = (uint8_t)sk;
			s->deps_mask = deps;
			s->confirm = (uint8_t)conf;
			s->when_step = (int8_t)ws;
			s->when_op = (uint8_t)wop;
			s->when_value_milli = wv;
			plan.n_steps++;
		} else if (!strcmp(cmd, "admit")) {
			struct planosz_admission_result r = planosz_admit(&plan, &policy);

			printf("A %s %d %d %u %u %u %u\n", planosz_code_str(r.code), r.step, r.other_step,
			       (unsigned int)r.wcet_ms, (unsigned int)r.critical_path_ms, (unsigned int)r.max_concurrency,
			       (unsigned int)r.ir_wcet_mismatch);
		} else if (!strcmp(cmd, "wire")) {
			static char chunk[512];
			const char *why = "";
			enum planosz_code rc = PLANOSZ_OK;

			planosz_upload_begin();
			while (scanf("%511s", chunk) == 1 && strcmp(chunk, "wire-end") != 0) {
				if (rc == PLANOSZ_OK) {
					rc = planosz_upload_data(chunk, &why);
				}
			}
			if (rc == PLANOSZ_OK) {
				rc = planosz_upload_end(&why);
			}
			printf("W %s %s\n", planosz_code_str(rc), why ? why : "");
			if (rc == PLANOSZ_OK) {
				const struct planosz_ir_plan *p = planosz_upload_plan();

				printf("P %s %u %u %u\n", p->plan_id, (unsigned int)p->deadline_ms, (unsigned int)p->perm_mask,
				       (unsigned int)p->n_steps);
				for (unsigned int i = 0; i < p->n_steps; i++) {
					const struct planosz_ir_step *s = &p->steps[i];

					printf("S %s %u %u %u %d %u %d %d %d %d %d %u\n", s->id, (unsigned int)s->skill_idx,
					       (unsigned int)s->deps_mask, (unsigned int)s->confirm, (int)s->when_step,
					       (unsigned int)s->when_op, (int)s->when_value_milli, (int)s->args[0], (int)s->args[1],
					       (int)s->args[2], (int)s->args[3], (unsigned int)s->wcet_ms);
				}
			}
		}
	}
	return 0;
}
