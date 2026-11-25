/* Skill handlers: one function dispatching on the generated skill index. */
#ifndef PLANOSZ_SKILLS_H_
#define PLANOSZ_SKILLS_H_

#include "planosz/errors.h"
#include "planosz/ir.h"

/* On success *out_milli receives the step's primary output (floats as milli-units). */
enum planosz_code planosz_skill_dispatch(const struct planosz_ir_step *step, int32_t *out_milli);

#endif /* PLANOSZ_SKILLS_H_ */
