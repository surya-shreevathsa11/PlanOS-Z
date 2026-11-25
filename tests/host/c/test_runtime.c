/* Host unit tests for the C runtime core (admission, resource table, executor, trace).
 * Built with plain gcc by tests/host/test_c_runtime.py (no Zephyr needed).
 * The three PLAN_* headers are generated from examples/missions by `planoszc compile`,
 * so Python and C admission are checked against the SAME plans. */
#include <stdio.h>
#include <string.h>
#include "planosz/planosz.h"
#include "planosz/skill_table.gen.h"
#include "accept.h"
#include "deadline.h"
#include "conflict.h"

static int failures;
static uint32_t vclock;

#define CHECK(cond)                                                                        \
	do {                                                                               \
		if (!(cond)) {                                                             \
			printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);              \
			failures++;                                                        \
		}                                                                          \
	} while (0)

/* ---- host HAL with a virtual clock ---- */
static int h_sensor(uint8_t i, int32_t *milli)
{
	static const int32_t v[4] = { 48200, 31200, 27500, 24000 };

	vclock += 5;
	if (i >= 4) {
		return -1;
	}
	*milli = v[i];
	return 0;
}
static int h_out(uint8_t c, bool on) { (void)c; (void)on; return 0; }
static int h_delay(uint32_t ms) { vclock += ms; return 0; }
static int h_idle(uint32_t ms, bool deep) { (void)deep; vclock += ms; return 0; }
static int h_info(uint8_t f, char *b, size_t l) { (void)f; (void)l; b[0] = 0; return 0; }
static uint32_t h_uptime(void) { return vclock; }

static const struct planosz_hal_ops host_ops = { h_sensor, h_out, h_delay, h_idle, h_info, h_uptime };
const struct planosz_hal_ops *planosz_backend_ops(void) { return &host_ops; }

static void stdout_sink(const char *line, void *ctx) { (void)ctx; printf("TRACE %s\n", line); }

static const struct planosz_policy POLICY = { 0xffffffffu, false, false };

static struct planosz_ir_step mk(uint8_t skill, int32_t a0, int32_t a1, int32_t a2, int32_t a3)
{
	struct planosz_ir_step s = { .id = "s", .skill_idx = skill, .wcet_ms = 10, .when_step = -1,
				     .args = { a0, a1, a2, a3 } };
	return s;
}

static struct planosz_admission_result admit1(struct planosz_ir_step *s, uint32_t perms)
{
	struct planosz_ir_plan p = { "t", 1000, perms, 1, s };

	return planosz_admit(&p, &POLICY);
}

int main(void)
{
	/* --- the same three missions Python decides on --- */
	struct planosz_admission_result a = planosz_admit(&ACCEPT, &POLICY);
	struct planosz_admission_result d = planosz_admit(&DEADLINE, &POLICY);
	struct planosz_admission_result c = planosz_admit(&CONFLICT, &POLICY);

	printf("ADMIT accept %s wcet=%u\n", planosz_code_str(a.code), (unsigned int)a.wcet_ms);
	printf("ADMIT deadline %s wcet=%u\n", planosz_code_str(d.code), (unsigned int)d.wcet_ms);
	printf("ADMIT conflict %s\n", planosz_code_str(c.code));
	CHECK(a.code == PLANOSZ_OK && a.wcet_ms == 1180);
	CHECK(d.code == PLANOSZ_REJECT_DEADLINE_INFEASIBLE && d.wcet_ms == 2594);
	CHECK(c.code == PLANOSZ_REJECT_RESOURCE_CONFLICT && c.step == 1 && c.other_step == 2);

	/* --- timing model must match the executor: sequential = sum of WCETs, parallel = critical path --- */
	{
		const struct planosz_policy seq = { 0xffffffffu, false, true };
		struct planosz_admission_result sa = planosz_admit(&ACCEPT, &seq);
		struct planosz_ir_plan tight = ACCEPT;

		printf("ADMIT accept_sequential %s wcet=%u\n", planosz_code_str(sa.code), (unsigned int)sa.wcet_ms);
		CHECK(sa.code == PLANOSZ_OK && sa.wcet_ms == 60 + 120 + 1030 + 310 + 30); /* 1550 */
		tight.deadline_ms = 1300; /* fits the critical path (1180) but not the sum (1550) */
		CHECK(planosz_admit(&tight, &POLICY).code == PLANOSZ_OK);
		CHECK(planosz_admit(&tight, &seq).code == PLANOSZ_REJECT_DEADLINE_INFEASIBLE);
	}

	/* --- hand-built negative cases --- */
	struct planosz_ir_step s;

	s = mk(99, 0, 0, 0, 0);
	CHECK(admit1(&s, 0xff).code == PLANOSZ_REJECT_UNKNOWN_SKILL);
	s = mk(PLANOSZ_SKILL_SENSOR_READ, 9, 1, 0, 0); /* sensor enum index out of range */
	CHECK(admit1(&s, PLANOSZ_PERM_READ_SENSOR).code == PLANOSZ_REJECT_BAD_ARGS);
	s = mk(PLANOSZ_SKILL_SENSOR_READ, 0, 1, 0, 0);
	CHECK(admit1(&s, 0).code == PLANOSZ_REJECT_PERMISSION);
	CHECK(admit1(&s, PLANOSZ_PERM_READ_SENSOR).code == PLANOSZ_OK);
	s = mk(PLANOSZ_SKILL_ACTUATOR_CONTACTOR, 0, 200, 0, 0);
	CHECK(admit1(&s, PLANOSZ_PERM_ACTUATE | PLANOSZ_PERM_HV_CONTROL).code == PLANOSZ_REJECT_SAFETY_CLASS);
	{
		struct planosz_policy pol = { 0xffffffffu, true, false };
		struct planosz_ir_plan p = { "t", 1000, PLANOSZ_PERM_ACTUATE | PLANOSZ_PERM_HV_CONTROL, 1, &s };

		CHECK(planosz_admit(&p, &pol).code == PLANOSZ_OK);
	}
	{
		struct planosz_ir_step bad[2] = { mk(PLANOSZ_SKILL_WAIT, 10, 0, 0, 0), mk(PLANOSZ_SKILL_WAIT, 10, 0, 0, 0) };
		struct planosz_ir_plan p = { "t", 1000, 0, 2, bad };

		bad[0].deps_mask = 0x2; /* forward edge => not topological / cyclic */
		CHECK(planosz_admit(&p, &POLICY).code == PLANOSZ_REJECT_BAD_PLAN);
		bad[0].deps_mask = 0;
		CHECK(planosz_admit(&p, &POLICY).code == PLANOSZ_OK);
		p.deadline_ms = 0;
		CHECK(planosz_admit(&p, &POLICY).code == PLANOSZ_REJECT_BAD_PLAN);
	}
	CHECK(planosz_admit(NULL, &POLICY).code == PLANOSZ_REJECT_BAD_PLAN);

	/* --- resource table --- */
	planosz_res_reset();
	CHECK(planosz_res_acquire(0x1, 0x2) == 0);
	CHECK(planosz_res_acquire(0x1, 0) == -1); /* exclusive vs held exclusive */
	CHECK(planosz_res_acquire(0, 0x2) == 0);  /* shared vs shared ok */
	CHECK(planosz_res_acquire(0x2, 0) == -1); /* exclusive vs shared */
	planosz_res_release(0x1, 0x2);
	CHECK(planosz_res_acquire(0x1, 0) == 0);

	/* --- executor on the accepted mission (cool=31.2 => warn runs, rest skipped) --- */
	planosz_trace_set_sink(stdout_sink, NULL);
	planosz_trace_reset();
	vclock = 0;
	planosz_trace_admit(&ACCEPT, &a);
	struct planosz_exec_result r = planosz_exec_run(&ACCEPT);

	CHECK(r.code == PLANOSZ_OK);
	CHECK(r.total_ms <= ACCEPT.deadline_ms);
	CHECK(r.total_ms >= 1000); /* the warn pulse train really ran */

	CHECK(strcmp(planosz_code_str(PLANOSZ_REJECT_BAD_ARGS), "REJECT_BAD_ARGS") == 0);
	CHECK(planosz_registry_find("sys.info") >= 0 && planosz_registry_find("nope") == -1);
	CHECK(planosz_registry_count() == PLANOSZ_SKILL_COUNT);

	printf(failures ? "RESULT FAIL (%d)\n" : "RESULT PASS\n", failures);
	return failures ? 1 : 0;
}
