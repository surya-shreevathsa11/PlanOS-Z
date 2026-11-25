/* Ingress: how plans reach the device. v0.1 = named, pre-compiled IR plans selected from
 * the Zephyr shell (`planosz plans|admit|run`). UART/net blob upload is roadmap phase 3+. */
#ifndef PLANOSZ_INGRESS_H_
#define PLANOSZ_INGRESS_H_

#include <stdbool.h>
#include "planosz/ir.h"

#define PLANOSZ_MAX_REGISTERED_PLANS 8

int planosz_ingress_register(const char *name, const struct planosz_ir_plan *plan);
const struct planosz_ir_plan *planosz_ingress_find(const char *name);

/* Admit (always traced) and, if accepted, execute. Returns the final reason code. */
enum planosz_code planosz_ingress_submit(const struct planosz_ir_plan *plan, bool execute);

#endif /* PLANOSZ_INGRESS_H_ */
