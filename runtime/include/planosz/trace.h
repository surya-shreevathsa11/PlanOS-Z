/* NDJSON trace: every admit/start/end/error is one JSON object per line.
 * Sink is pluggable: Zephyr app -> printk/log, host tests -> stdout. */
#ifndef PLANOSZ_TRACE_H_
#define PLANOSZ_TRACE_H_

#include <stdint.h>
#include "planosz/admission.h"

typedef void (*planosz_trace_sink_fn)(const char *line, void *ctx);

void planosz_trace_set_sink(planosz_trace_sink_fn fn, void *ctx);
void planosz_trace_reset(void);

/* body: printf-style JSON members WITHOUT braces, e.g. "\"step\":\"%s\"" */
void planosz_trace_emit(uint32_t t_ms, const char *type, const char *body_fmt, ...)
	__attribute__((format(printf, 3, 4)));

void planosz_trace_admit(const struct planosz_ir_plan *plan, const struct planosz_admission_result *res);

#endif /* PLANOSZ_TRACE_H_ */
