/* Host tests for the C runtime core: admission (incl. the device-owned-WCET trust boundary),
 * resource table, the PARALLEL executor (real pthreads via the OSAL), ingress and IR upload.
 * Built with plain gcc by tests/host/test_c_runtime.py (no Zephyr needed).
 *
 * The plan headers are generated from examples/missions by `planoszc compile`, so Python and
 * C admission are checked against the SAME plans; blobs.h holds IR wire uploads built by
 * planoszc.wire (good, corrupted, version-bumped and WCET-forged). */
#define _POSIX_C_SOURCE 200809L
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include "planosz/planosz.h"
#include "planosz/skill_table.gen.h"
#include "accept.h"
#include "blobs.h"
#include "conflict.h"
#include "deadline.h"
#include "parallel.h"

static int failures;

#define CHECK(cond)                                                                        \
	do {                                                                               \
		if (!(cond)) {                                                             \
			printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);              \
			failures++;                                                        \
		}                                                                          \
	} while (0)

/* ---- host HAL: real time (so parallel steps genuinely overlap), thread-safe ---- */
static pthread_mutex_t hal_lock = PTHREAD_MUTEX_INITIALIZER;
static struct timespec base;
static int adc_in_flight, adc_max_in_flight;
static int32_t inject_sensor_fail = -1;   /* sensor idx whose read fails */
static uint32_t inject_sensor_extra_ms;   /* extra latency per sensor read (watchdog tests) */

static void sleep_ms(uint32_t ms)
{
	struct timespec ts = { ms / 1000, (long)(ms % 1000) * 1000000L };

	nanosleep(&ts, NULL);
}

static uint32_t h_uptime(void)
{
	struct timespec t;

	clock_gettime(CLOCK_MONOTONIC, &t);
	return (uint32_t)((t.tv_sec - base.tv_sec) * 1000 + (t.tv_nsec - base.tv_nsec) / 1000000L);
}

static int h_sensor(uint8_t i, int32_t *milli)
{
	static const int32_t v[4] = { 48200, 31200, 27500, 24000 };

	pthread_mutex_lock(&hal_lock);
	if (++adc_in_flight > adc_max_in_flight) {
		adc_max_in_flight = adc_in_flight;
	}
	pthread_mutex_unlock(&hal_lock);
	sleep_ms(15 + inject_sensor_extra_ms);
	pthread_mutex_lock(&hal_lock);
	adc_in_flight--;
	pthread_mutex_unlock(&hal_lock);
	if (i >= 4 || (int32_t)i == inject_sensor_fail) {
		return -1;
	}
	*milli = v[i];
	return 0;
}
static int h_out(uint8_t c, bool on) { (void)c; (void)on; return 0; }
static int h_delay(uint32_t ms) { sleep_ms(ms); return 0; }
static int h_idle(uint32_t ms, bool deep) { (void)deep; sleep_ms(ms); return 0; }
static int h_info(uint8_t f, char *b, size_t l) { (void)f; (void)l; b[0] = 0; return 0; }

static const struct planosz_hal_ops host_ops = { h_sensor, h_out, h_delay, h_idle, h_info, h_uptime };
const struct planosz_hal_ops *planosz_backend_ops(void) { return &host_ops; }

/* ---- trace capture ---- */
#define MAX_LINES 128
static char lines[MAX_LINES][640];
static int n_lines;

static void capture_sink(const char *line, void *ctx)
{
	(void)ctx;
	if (n_lines < MAX_LINES) {
		strncpy(lines[n_lines++], line, sizeof(lines[0]) - 1);
	}
	printf("TRACE %s\n", line);
}

static void trace_clear(void)
{
	n_lines = 0;
	planosz_trace_reset();
}

/* index of the first captured line containing `needle` (at/after `from`), or -1 */
static int find_line(const char *needle, int from)
{
	for (int i = from; i < n_lines; i++) {
		if (strstr(lines[i], needle)) {
			return i;
		}
	}
	return -1;
}

static int count_lines(const char *needle)
{
	int c = 0;

	for (int i = 0; i < n_lines; i++) {
		c += strstr(lines[i], needle) != NULL;
	}
	return c;
}

static const struct planosz_policy POLICY2 = { 0xffffffffu, false, 2 };

static struct planosz_ir_step mk(uint8_t skill, int32_t a0, int32_t a1, int32_t a2, int32_t a3)
{
	struct planosz_ir_step s = { .id = "s", .skill_idx = skill, .wcet_ms = 10, .when_step = -1,
				     .args = { a0, a1, a2, a3 } };
	return s;
}

static struct planosz_admission_result admit1(struct planosz_ir_step *s, uint32_t perms)
{
	struct planosz_ir_plan p = { "t", 1000, perms, 1, s };

	return planosz_admit(&p, &POLICY2);
}

/* mutable copy of a generated (const) plan so tests can falsify host claims */
static struct planosz_ir_step copy_steps[PLANOSZ_MAX_STEPS];
static struct planosz_ir_plan clone(const struct planosz_ir_plan *src)
{
	struct planosz_ir_plan p = *src;

	memcpy(copy_steps, src->steps, sizeof(src->steps[0]) * src->n_steps);
	p.steps = copy_steps;
	return p;
}

/* ACCEPT with a short warn pulse train (3 x 60 ms) so failure-path tests stay fast */
static struct planosz_ir_plan short_accept(void)
{
	struct planosz_ir_plan p = clone(&ACCEPT);

	copy_steps[2].args[1] = 3;
	copy_steps[2].args[2] = 30;
	copy_steps[2].args[3] = 30;
	return p;
}

static struct planosz_policy pol(unsigned int workers)
{
	struct planosz_policy p = { 0xffffffffu, false, (uint8_t)workers };

	return p;
}

static void test_admission_matches_python(void)
{
	struct planosz_admission_result a = planosz_admit(&ACCEPT, &POLICY2);
	struct planosz_admission_result d = planosz_admit(&DEADLINE, &POLICY2);
	struct planosz_admission_result c = planosz_admit(&CONFLICT, &POLICY2);

	printf("ADMIT accept %s wcet=%u\n", planosz_code_str(a.code), (unsigned int)a.wcet_ms);
	printf("ADMIT deadline %s wcet=%u\n", planosz_code_str(d.code), (unsigned int)d.wcet_ms);
	printf("ADMIT conflict %s\n", planosz_code_str(c.code));
	CHECK(a.code == PLANOSZ_OK && a.wcet_ms == 1180 && a.max_concurrency == 2 && a.ir_wcet_mismatch == 0);
	CHECK(d.code == PLANOSZ_REJECT_DEADLINE_INFEASIBLE && d.wcet_ms == 2594);
	CHECK(c.code == PLANOSZ_REJECT_RESOURCE_CONFLICT && c.step == 1 && c.other_step == 2);
}

static void test_timing_models(void)
{
	/* ACCEPT: step WCETs 60,120,1030,310,30. warn||rest are mutually exclusive branches => omega 2 */
	for (unsigned int w = 0; w <= 3; w++) {
		struct planosz_policy p = pol(w);
		struct planosz_admission_result r = planosz_admit(&ACCEPT, &p);

		CHECK(r.code == PLANOSZ_OK);
		CHECK(r.wcet_ms == (w == 1 ? 1550u : 1180u)); /* sequential = sum, otherwise critical path */
		CHECK(r.critical_path_ms == 1180);
	}
	{
		struct planosz_ir_plan tight = ACCEPT;
		struct planosz_policy seq = pol(1), par = pol(2);

		tight.deadline_ms = 1300; /* fits the critical path (1180) but not the sum (1550) */
		CHECK(planosz_admit(&tight, &par).code == PLANOSZ_OK);
		CHECK(planosz_admit(&tight, &seq).code == PLANOSZ_REJECT_DEADLINE_INFEASIBLE);
	}
	/* PARALLEL: three independent 200 ms sweeps + report(30): cp=230, sum=630, omega=3.
	 * 2 workers cannot run 3 at once => Graham bound 230 + ceil(400/2) = 430. */
	{
		struct planosz_admission_result r;
		struct planosz_policy p;

		p = pol(0); r = planosz_admit(&PARALLEL, &p);
		CHECK(r.wcet_ms == 230 && r.max_concurrency == 3);
		p = pol(1); r = planosz_admit(&PARALLEL, &p);
		CHECK(r.wcet_ms == 630 && r.code == PLANOSZ_REJECT_DEADLINE_INFEASIBLE);
		p = pol(2); r = planosz_admit(&PARALLEL, &p);
		CHECK(r.wcet_ms == 430 && r.code == PLANOSZ_OK);
		p = pol(3); r = planosz_admit(&PARALLEL, &p);
		CHECK(r.wcet_ms == 230 && r.code == PLANOSZ_OK);
		printf("ADMIT parallel_w2 %s wcet=%u\n", planosz_code_str(planosz_admit(&PARALLEL, &POLICY2).code),
		       (unsigned int)planosz_admit(&PARALLEL, &POLICY2).wcet_ms);
	}
}

/* THE trust-boundary tests: falsified host claims must not change a single decision. */
static void test_tampered_ir_is_still_rejected(void)
{
	struct planosz_ir_plan p;
	struct planosz_admission_result r;

	/* 1. understated WCET (every step claims 1 ms) on the infeasible plan */
	p = clone(&DEADLINE);
	for (int i = 0; i < p.n_steps; i++) {
		copy_steps[i].wcet_ms = 1;
	}
	r = planosz_admit(&p, &POLICY2);
	CHECK(r.code == PLANOSZ_REJECT_DEADLINE_INFEASIBLE && r.wcet_ms == 2594);
	CHECK(r.ir_wcet_mismatch == p.n_steps); /* and the lie is reported */

	/* 2. WCET zero / absurdly small / absurdly large */
	p = clone(&DEADLINE);
	for (int i = 0; i < p.n_steps; i++) {
		copy_steps[i].wcet_ms = 0;
	}
	CHECK(planosz_admit(&p, &POLICY2).code == PLANOSZ_REJECT_DEADLINE_INFEASIBLE);
	p = clone(&ACCEPT);
	for (int i = 0; i < p.n_steps; i++) {
		copy_steps[i].wcet_ms = 0xffffffffu; /* overstating must not turn an ACCEPT into a REJECT either */
	}
	r = planosz_admit(&p, &POLICY2);
	CHECK(r.code == PLANOSZ_OK && r.wcet_ms == 1180);

	/* 3. stripped resource masks on the conflicting plan */
	p = clone(&CONFLICT);
	for (int i = 0; i < p.n_steps; i++) {
		copy_steps[i].excl_mask = copy_steps[i].shared_mask = 0;
	}
	r = planosz_admit(&p, &POLICY2);
	CHECK(r.code == PLANOSZ_REJECT_RESOURCE_CONFLICT && r.step == 1 && r.other_step == 2);

	/* 4. forged mutex_mask ("these branches are exclusive") must not hide a real conflict */
	p = clone(&CONFLICT);
	for (int i = 0; i < p.n_steps; i++) {
		copy_steps[i].mutex_mask = 0xffffffffu;
		copy_steps[i].excl_mask = copy_steps[i].shared_mask = 0;
	}
	CHECK(planosz_admit(&p, &POLICY2).code == PLANOSZ_REJECT_RESOURCE_CONFLICT);

	/* 5. the combination: everything falsified at once */
	p = clone(&DEADLINE);
	for (int i = 0; i < p.n_steps; i++) {
		copy_steps[i].wcet_ms = 1;
		copy_steps[i].excl_mask = copy_steps[i].shared_mask = copy_steps[i].mutex_mask = 0;
	}
	CHECK(planosz_admit(&p, &POLICY2).code == PLANOSZ_REJECT_DEADLINE_INFEASIBLE);

	/* 6. claiming extra resources / mutexes in the IR cannot change an ACCEPT either */
	p = clone(&ACCEPT);
	for (int i = 0; i < p.n_steps; i++) {
		copy_steps[i].excl_mask = 0xffffffffu;
		copy_steps[i].mutex_mask = 0;
	}
	CHECK(planosz_admit(&p, &POLICY2).code == PLANOSZ_OK);

	/* 7. complementary `when` conditions on a step pair are the ONLY way to be exclusive: removing
	 *    them (the IR's own conditions) from an exclusive pair turns the plan into a conflict. */
	p = clone(&ACCEPT);
	copy_steps[3].when_step = -1; /* rest no longer guarded: warn and rest can overlap on power_domain */
	CHECK(planosz_admit(&p, &POLICY2).code == PLANOSZ_REJECT_RESOURCE_CONFLICT);
}

static void test_negative_cases(void)
{
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
		struct planosz_policy hz = { 0xffffffffu, true, 2 };
		struct planosz_ir_plan p = { "t", 1000, PLANOSZ_PERM_ACTUATE | PLANOSZ_PERM_HV_CONTROL, 1, &s };

		CHECK(planosz_admit(&p, &hz).code == PLANOSZ_OK);
	}
	{
		struct planosz_ir_step bad[2] = { mk(PLANOSZ_SKILL_WAIT, 10, 0, 0, 0), mk(PLANOSZ_SKILL_WAIT, 10, 0, 0, 0) };
		struct planosz_ir_plan p = { "t", 1000, 0, 2, bad };

		bad[0].deps_mask = 0x2; /* forward edge => not topological / cyclic */
		CHECK(planosz_admit(&p, &POLICY2).code == PLANOSZ_REJECT_BAD_PLAN);
		bad[0].deps_mask = 0;
		CHECK(planosz_admit(&p, &POLICY2).code == PLANOSZ_OK);
		bad[1].when_step = 5; /* condition on a step that is not an ancestor */
		CHECK(planosz_admit(&p, &POLICY2).code == PLANOSZ_REJECT_BAD_PLAN);
		bad[1].when_step = -3;
		CHECK(planosz_admit(&p, &POLICY2).code == PLANOSZ_REJECT_BAD_PLAN);
		bad[1].when_step = -1;
		bad[1].id = NULL;
		CHECK(planosz_admit(&p, &POLICY2).code == PLANOSZ_REJECT_BAD_PLAN);
		bad[1].id = "s";
		p.deadline_ms = 0;
		CHECK(planosz_admit(&p, &POLICY2).code == PLANOSZ_REJECT_BAD_PLAN);
	}
	CHECK(planosz_admit(NULL, &POLICY2).code == PLANOSZ_REJECT_BAD_PLAN);
}

static void test_device_derivation(void)
{
	struct planosz_derived d;
	int32_t pulse[4] = { 0, 5, 100, 100 }; /* warn_lamp x5, 100/100 ms */
	int32_t sens[4] = { 1, 4, 0, 0 };      /* coolant_proxy, 4 samples */
	int32_t idle[4] = { 300, 1, 0, 0 };    /* 300 ms, deep */

	CHECK(planosz_registry_derive(PLANOSZ_SKILL_ACTUATOR_PULSE, pulse, &d) == 0);
	CHECK(d.wcet_ms == 5u * (100 + 100 + 2) + 20);
	CHECK(planosz_res_names[__builtin_ctz(d.excl_mask)][4] == 'w'); /* out_warn_lamp */
	CHECK(__builtin_popcount(d.excl_mask) == 1 && __builtin_popcount(d.shared_mask) == 1);
	pulse[0] = 2; /* a different channel is a different resource */
	{
		struct planosz_derived d2;

		CHECK(planosz_registry_derive(PLANOSZ_SKILL_ACTUATOR_PULSE, pulse, &d2) == 0);
		CHECK(d2.excl_mask != d.excl_mask && d2.shared_mask == d.shared_mask);
	}
	CHECK(planosz_registry_derive(PLANOSZ_SKILL_SENSOR_READ, sens, &d) == 0 && d.wcet_ms == 40 + 20 * 4 &&
	      d.excl_mask == 0 && __builtin_popcount(d.shared_mask) == 2);
	CHECK(planosz_registry_derive(PLANOSZ_SKILL_POWER_IDLE, idle, &d) == 0 && d.wcet_ms == 340 && d.shared_mask == 0);
	pulse[0] = 3; /* claim variant outside the enum */
	CHECK(planosz_registry_derive(PLANOSZ_SKILL_ACTUATOR_PULSE, pulse, &d) == -1);
	CHECK(planosz_registry_derive(99, pulse, &d) == -1);
}

static void test_resource_table(void)
{
	planosz_res_reset();
	CHECK(planosz_res_acquire(0x1, 0x2) == 0);
	CHECK(planosz_res_acquire(0x1, 0) == -1); /* exclusive vs held exclusive */
	CHECK(planosz_res_acquire(0, 0x2) == 0);  /* shared vs shared ok */
	CHECK(planosz_res_acquire(0x2, 0) == -1); /* exclusive vs shared */
	planosz_res_release(0x1, 0x2);            /* first holder leaves; the second shared holder stays */
	CHECK(planosz_res_acquire(0x2, 0) == -1); /* ...so a refcounted shared claim still blocks exclusive */
	planosz_res_release(0, 0x2);
	CHECK(planosz_res_acquire(0x2, 0) == 0);
	CHECK(planosz_res_acquire(0x4, 0x4) == -1); /* same resource claimed in both modes */
}

static unsigned int run_trace(const struct planosz_ir_plan *p, unsigned int workers, struct planosz_exec_result *out)
{
	trace_clear();
	*out = planosz_exec_run_workers(p, workers);
	return out->total_ms;
}

static void test_executor_branching_and_trace(void)
{
	struct planosz_exec_result r;

	run_trace(&ACCEPT, 2, &r);
	CHECK(r.code == PLANOSZ_OK && r.total_ms >= 1000 && r.total_ms <= ACCEPT.deadline_ms);
	CHECK(find_line("\"type\":\"run_start\"", 0) == 0);
	CHECK(find_line("\"type\":\"run_end\"", 0) == n_lines - 1);
	CHECK(count_lines("\"type\":\"step_skip\",\"t_ms\"") == 1 && find_line("\"step\":\"rest\",\"observed_milli\":31200", 0) > 0);
	CHECK(find_line("\"step\":\"warn\",\"status\":\"OK\"", 0) > 0);
	/* trace carries what replay needs: device-derived wcet, needs and claims */
	CHECK(find_line("\"step\":\"warn\",\"action\":\"actuator.pulse\"", 0) > 0);
	CHECK(find_line("\"wcet_ms\":1030,\"needs\":[\"volt\",\"cool\"],\"claims\":[\"out_warn_lamp:exclusive\",\"power_domain:shared\"]", 0) > 0);
	CHECK(r.peak_concurrency == 2);
}

static void test_executor_parallel_overlap(void)
{
	struct planosz_exec_result par, seq, three;
	uint32_t t_par, t_seq;

	adc_max_in_flight = 0;
	t_par = run_trace(&PARALLEL, 2, &par);
	CHECK(par.code == PLANOSZ_OK && par.peak_concurrency == 2 && adc_max_in_flight == 2);
	CHECK(par.total_ms <= PARALLEL.deadline_ms);
	/* both leading sweeps start before either finishes: genuine overlap in the trace */
	{
		int s0 = find_line("\"step_start\"", 0), s1 = find_line("\"step_start\"", s0 + 1);
		int e0 = find_line("\"type\":\"step_end\"", 0);

		CHECK(s0 >= 0 && s1 > s0 && e0 > s1);
	}
	adc_max_in_flight = 0;
	t_seq = run_trace(&PARALLEL, 1, &seq);
	CHECK(seq.code == PLANOSZ_OK && seq.peak_concurrency == 1 && adc_max_in_flight == 1);
	printf("TIMING parallel2=%ums sequential=%ums\n", t_par, t_seq);
	CHECK(t_par * 100 < t_seq * 85); /* 3 sweeps: ~2 sweeps long vs ~3 sweeps long */
	adc_max_in_flight = 0;
	run_trace(&PARALLEL, 3, &three);
	CHECK(three.code == PLANOSZ_OK && three.peak_concurrency == 3 && adc_max_in_flight == 3);
	CHECK(three.total_ms < par.total_ms + 10 && three.total_ms * 100 < t_seq * 55);
}

static void test_executor_watchdogs_and_failures(void)
{
	struct planosz_exec_result r;
	struct planosz_ir_plan p;

	/* per-step watchdog: a sensor read that stalls 100 ms per sample vs a 60 ms budget is reported at ~60 ms,
	 * nothing downstream starts, and the run ends once the stalled skill returns */
	inject_sensor_extra_ms = 100;
	run_trace(&ACCEPT, 2, &r);
	inject_sensor_extra_ms = 0;
	CHECK(r.code == PLANOSZ_EXEC_TIMEOUT && r.total_ms >= 400);
	CHECK(find_line("\"code\":\"EXEC_TIMEOUT\",\"detail\":\"exceeded wcet_ms=", 0) > 0);
	CHECK(find_line("\"step\":\"warn\",\"action\"", 0) < 0);
	CHECK(find_line("\"type\":\"step_cancel\"", 0) > 0); /* the sibling sweep is reported cancelled */
	{
		int e = find_line("\"type\":\"step_error\"", 0);
		unsigned int t_ms = 0;

		CHECK(e > 0 && sscanf(strstr(lines[e], "\"t_ms\":") + 7, "%u", &t_ms) == 1 && t_ms < 200);
	}
	CHECK(find_line("\"type\":\"run_end\",\"t_ms\":", 0) == n_lines - 1 && strstr(lines[n_lines - 1], "\"status\":\"FAIL\""));

	/* deadline watchdog: shrink the deadline AFTER admission so the plan overruns it */
	p = short_accept();
	p.deadline_ms = 100;
	run_trace(&p, 2, &r);
	CHECK(r.code == PLANOSZ_EXEC_TIMEOUT);
	CHECK(find_line("\"type\":\"deadline_miss\"", 0) > 0 && find_line("\"type\":\"step_cancel\",", 0) > 0);

	/* skill failure aborts the run */
	inject_sensor_fail = 1;
	run_trace(&ACCEPT, 2, &r);
	inject_sensor_fail = -1;
	CHECK(r.code == PLANOSZ_EXEC_SKILL_FAIL && find_line("\"step\":\"warn\",\"action\"", 0) < 0);

	/* runtime resource guard: the conflicting plan run WITHOUT admission collides on out_warn_lamp
	 * with 2 workers - even though its IR claims no resources (executor uses derived masks) ... */
	p = clone(&CONFLICT);
	for (int i = 0; i < p.n_steps; i++) {
		copy_steps[i].excl_mask = copy_steps[i].shared_mask = 0;
	}
	for (int i = 1; i <= 2; i++) { /* two short pulse trains on the same lamp */
		copy_steps[i].args[1] = 1;
		copy_steps[i].args[2] = 20;
		copy_steps[i].args[3] = 20;
	}
	run_trace(&p, 2, &r);
	CHECK(r.code == PLANOSZ_EXEC_SKILL_FAIL && find_line("resource busy", 0) > 0);
	/* ... and is harmless when forced to one worker */
	run_trace(&p, 1, &r);
	CHECK(r.code == PLANOSZ_OK);

	/* the executor's per-step budget is the DEVICE wcet: an IR claiming wcet_ms=1 must not trip it */
	p = short_accept();
	for (int i = 0; i < p.n_steps; i++) {
		copy_steps[i].wcet_ms = 1;
	}
	run_trace(&p, 2, &r);
	CHECK(r.code == PLANOSZ_OK);
}

static void test_ingress_and_upload(void)
{
	planosz_trace_set_sink(capture_sink, NULL);
	CHECK(planosz_ingress_find("uploaded") == NULL);
	CHECK(planosz_ingress_policy()->workers == PLANOSZ_EXEC_WORKERS);

	/* registered plan: submit = admit + run */
	planosz_ingress_register("parallel", &PARALLEL);
	trace_clear();
	CHECK(planosz_ingress_submit(planosz_ingress_find("parallel"), true) == PLANOSZ_OK);
	CHECK(find_line("\"type\":\"admit\"", 0) == 0 && find_line("\"workers\":2", 0) == 0);
	trace_clear();
	CHECK(planosz_ingress_submit(&DEADLINE, true) == PLANOSZ_REJECT_DEADLINE_INFEASIBLE);
	CHECK(n_lines == 1); /* rejected => admit record only: no side effects */

	/* good upload -> uploaded plan admits and runs */
	for (size_t b = 0; b < N_GOOD; b++) {
		planosz_ingress_load_begin();
		for (const char *const *c = GOOD[b].chunks; *c; c++) {
			CHECK(planosz_ingress_load_data(*c) == PLANOSZ_OK);
		}
		trace_clear();
		CHECK(planosz_ingress_load_end() == PLANOSZ_OK);
		CHECK(find_line("\"type\":\"load\"", 0) == 0 && find_line("\"status\":\"OK\"", 0) == 0);
		CHECK(planosz_ingress_find("uploaded") != NULL);
		CHECK(strcmp(planosz_ingress_find("uploaded")->plan_id, GOOD[b].plan_id) == 0);
		trace_clear();
		CHECK(planosz_ingress_submit(planosz_ingress_find("uploaded"), true) == GOOD[b].expect);
	}

	/* corrupted / unsupported uploads are REJECT_UNSUPPORTED_IR and leave the slot EMPTY */
	for (size_t b = 0; b < N_BAD; b++) {
		enum planosz_code rc = PLANOSZ_OK;

		planosz_ingress_load_begin();
		for (const char *const *c = BAD[b].chunks; *c && rc == PLANOSZ_OK; c++) {
			rc = planosz_ingress_load_data(*c);
		}
		trace_clear();
		if (rc == PLANOSZ_OK) {
			rc = planosz_ingress_load_end();
		}
		printf("BADLOAD %s -> %s\n", BAD[b].why, planosz_code_str(rc));
		CHECK(rc == PLANOSZ_REJECT_UNSUPPORTED_IR);
		CHECK(planosz_ingress_find("uploaded") == NULL);
	}
	trace_clear();
	planosz_ingress_load_begin();
	(void)planosz_ingress_load_data("!!!!");
	CHECK(planosz_ingress_load_end() == PLANOSZ_REJECT_UNSUPPORTED_IR);
	CHECK(find_line("\"code\":\"REJECT_UNSUPPORTED_IR\"", 0) == 0);

	/* a plan whose IR lies about WCET over the wire is still rejected by the device */
	for (size_t b = 0; b < N_FORGED; b++) {
		planosz_ingress_load_begin();
		for (const char *const *c = FORGED[b].chunks; *c; c++) {
			CHECK(planosz_ingress_load_data(*c) == PLANOSZ_OK);
		}
		CHECK(planosz_ingress_load_end() == PLANOSZ_OK); /* wire-valid ... */
		trace_clear();
		CHECK(planosz_ingress_submit(planosz_ingress_find("uploaded"), true) == FORGED[b].expect); /* ... admission decides */
		CHECK(n_lines == 1 && find_line("\"ir_wcet_mismatch\":", 0) >= 0);
	}
}

static void test_wire_primitives(void)
{
	uint8_t out[8];
	size_t n = 0;

	CHECK(planosz_crc16((const uint8_t *)"123456789", 9, 0xFFFFu) == 0x29B1); /* CRC-16/CCITT-FALSE check value */
	CHECK(planosz_base64_decode("TWFu", 4, out, sizeof(out), &n) == 0 && n == 3 && memcmp(out, "Man", 3) == 0);
	CHECK(planosz_base64_decode("TWE=", 4, out, sizeof(out), &n) == 0 && n == 2);
	CHECK(planosz_base64_decode("TQ==", 4, out, sizeof(out), &n) == 0 && n == 1);
	CHECK(planosz_base64_decode("TQ=", 3, out, sizeof(out), &n) == -1);
	CHECK(planosz_base64_decode("T=Q=", 4, out, sizeof(out), &n) == -1);
	CHECK(planosz_base64_decode("TQ==TQ==", 8, out, sizeof(out), &n) == -1); /* padding mid-stream */
	CHECK(planosz_base64_decode("TW#u", 4, out, sizeof(out), &n) == -1);
	CHECK(planosz_base64_decode("TWFuTWFu", 8, out, 4, &n) == -1); /* overflow */
}

int main(void)
{
	setvbuf(stdout, NULL, _IOLBF, 0);
	clock_gettime(CLOCK_MONOTONIC, &base);
	planosz_trace_set_sink(capture_sink, NULL);

	test_admission_matches_python();
	test_timing_models();
	test_tampered_ir_is_still_rejected();
	test_negative_cases();
	test_device_derivation();
	test_resource_table();
	test_wire_primitives();
	test_executor_branching_and_trace();
	test_executor_parallel_overlap();
	test_executor_watchdogs_and_failures();
	test_ingress_and_upload();

	CHECK(strcmp(planosz_code_str(PLANOSZ_REJECT_BAD_ARGS), "REJECT_BAD_ARGS") == 0);
	CHECK(planosz_registry_find("sys.info") >= 0 && planosz_registry_find("nope") == -1);
	CHECK(planosz_registry_count() == PLANOSZ_SKILL_COUNT);

	printf(failures ? "RESULT FAIL (%d)\n" : "RESULT PASS\n", failures);
	return failures ? 1 : 0;
}
