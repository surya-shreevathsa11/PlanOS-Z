#include <stdarg.h>
#include <stdio.h>
#include "planosz/trace.h"

#define PLANOSZ_TRACE_LINE_MAX 640

static planosz_trace_sink_fn sink;
static void *sink_ctx;
static uint32_t seq;
static char line[PLANOSZ_TRACE_LINE_MAX]; /* static: keeps worker/shell stacks small; emission is serialised */

void planosz_trace_set_sink(planosz_trace_sink_fn fn, void *ctx)
{
	sink = fn;
	sink_ctx = ctx;
}

void planosz_trace_reset(void)
{
	seq = 0;
}

void planosz_trace_vemit(uint32_t t_ms, const char *type, const char *body_fmt, va_list ap)
{
	int n;

	if (sink == NULL) {
		return;
	}
	n = snprintf(line, sizeof(line), "{\"seq\":%u,\"type\":\"%s\",\"t_ms\":%u", (unsigned int)seq++, type,
		     (unsigned int)t_ms);
	if (n > 0 && body_fmt != NULL && body_fmt[0] != '\0' && (size_t)n < sizeof(line)) {
		line[n++] = ',';
		n += vsnprintf(line + n, sizeof(line) - (size_t)n, body_fmt, ap);
	}
	if (n < 0 || (size_t)n >= sizeof(line) - 2) {
		n = (int)sizeof(line) - 3; /* truncated: still terminate the object */
	}
	line[n] = '}';
	line[n + 1] = '\0';
	sink(line, sink_ctx);
}

void planosz_trace_emit(uint32_t t_ms, const char *type, const char *body_fmt, ...)
{
	va_list ap;

	va_start(ap, body_fmt);
	planosz_trace_vemit(t_ms, type, body_fmt, ap);
	va_end(ap);
}

void planosz_trace_admit(const struct planosz_ir_plan *plan, const struct planosz_admission_result *res,
			 const struct planosz_policy *policy)
{
	char extra[64] = "";
	unsigned int workers = policy ? policy->workers : 0;

	if (res->ir_wcet_mismatch) {
		snprintf(extra, sizeof(extra), ",\"ir_wcet_mismatch\":%u", (unsigned int)res->ir_wcet_mismatch);
	}
	if (res->code == PLANOSZ_OK) {
		planosz_trace_emit(0, "admit", "\"plan\":\"%s\",\"status\":\"ACCEPT\",\"code\":\"OK\",\"wcet_ms\":%u,"
				   "\"deadline_ms\":%u,\"workers\":%u,\"steps\":%u%s", plan->plan_id,
				   (unsigned int)res->wcet_ms, (unsigned int)plan->deadline_ms, workers,
				   (unsigned int)plan->n_steps, extra);
	} else {
		planosz_trace_emit(0, "admit", "\"plan\":\"%s\",\"status\":\"REJECT\",\"code\":\"%s\",\"step\":%d,"
				   "\"wcet_ms\":%u,\"deadline_ms\":%u,\"workers\":%u%s", plan->plan_id,
				   planosz_code_str(res->code), res->step, (unsigned int)res->wcet_ms,
				   (unsigned int)plan->deadline_ms, workers, extra);
	}
}
