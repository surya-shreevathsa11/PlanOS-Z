/* PlanOS-Z stable reason codes. Numeric values are part of the public API:
 * append only, never renumber. Mirrors tools/planoszc/planoszc/errors.py and
 * docs/error_codes.md (checked by tests/host/test_docs_sync.py). */
#ifndef PLANOSZ_ERRORS_H_
#define PLANOSZ_ERRORS_H_

enum planosz_code {
	PLANOSZ_OK = 0,
	PLANOSZ_REJECT_UNKNOWN_SKILL = 1,
	PLANOSZ_REJECT_BAD_ARGS = 2,
	PLANOSZ_REJECT_PERMISSION = 3,
	PLANOSZ_REJECT_RESOURCE_CONFLICT = 4,
	PLANOSZ_REJECT_DEADLINE_INFEASIBLE = 5,
	PLANOSZ_REJECT_UNSUPPORTED_IR = 6,
	PLANOSZ_REJECT_SAFETY_CLASS = 7,
	PLANOSZ_REJECT_BAD_PLAN = 8,
	PLANOSZ_EXEC_TIMEOUT = 9,
	PLANOSZ_EXEC_SKILL_FAIL = 10,
	PLANOSZ_EXEC_CANCELLED = 11,
	PLANOSZ_EXEC_BACKEND_UNAVAILABLE = 12,
	PLANOSZ_CODE_COUNT = 13
};

/* "REJECT_BAD_ARGS", "OK", ... (never NULL) */
const char *planosz_code_str(enum planosz_code code);

#endif /* PLANOSZ_ERRORS_H_ */
