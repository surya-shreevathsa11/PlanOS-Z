/* Ingress: how plans reach the device.
 *   - named, pre-compiled IR plans registered by the app (CI / demos): `planosz plans|admit|run`
 *   - UART/shell upload of the IR wire format into one ephemeral slot named "uploaded":
 *       planosz load begin | planosz load data <base64> ... | planosz load end
 *     then `planosz admit uploaded` / `planosz run uploaded`.
 * Every submission is serialised (one admit/run at a time). */
#ifndef PLANOSZ_INGRESS_H_
#define PLANOSZ_INGRESS_H_

#include <stdbool.h>
#include <stddef.h>
#include "planosz/admission.h"
#include "planosz/ir.h"

#define PLANOSZ_MAX_REGISTERED_PLANS 8
#define PLANOSZ_UPLOADED_PLAN "uploaded"

int planosz_ingress_register(const char *name, const struct planosz_ir_plan *plan);
/* Registered plans, plus the upload slot under PLANOSZ_UPLOADED_PLAN (NULL if empty). */
const struct planosz_ir_plan *planosz_ingress_find(const char *name);
size_t planosz_ingress_count(void);
const char *planosz_ingress_name(size_t idx);
/* Policy the device admits with (workers == executor worker count). */
const struct planosz_policy *planosz_ingress_policy(void);

/* Admit (always traced) and, if accepted, execute. Returns the final reason code. */
enum planosz_code planosz_ingress_submit(const struct planosz_ir_plan *plan, bool execute);

/* Upload slot (what the shell commands call). `end` emits a `load` trace record. */
void planosz_ingress_load_begin(void);
enum planosz_code planosz_ingress_load_data(const char *b64);
enum planosz_code planosz_ingress_load_end(void);

#endif /* PLANOSZ_INGRESS_H_ */
