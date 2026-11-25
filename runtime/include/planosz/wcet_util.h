/* Helpers used by the generated per-skill WCET functions (skill_table.gen.c).
 * Python semantics are mirrored exactly (floor division / modulo) so the device and
 * planoszc agree bit-for-bit on every WCET; tests/host/test_wcet_agreement.py enforces it. */
#ifndef PLANOSZ_WCET_UTIL_H_
#define PLANOSZ_WCET_UTIL_H_

#include <stdint.h>

#define PLANOSZ_WCET_SATURATED (INT64_C(1) << 40) /* division by zero => "infinite" => reject */

static inline int64_t pz_fdiv(int64_t a, int64_t b)
{
	int64_t q;

	if (b == 0) {
		return PLANOSZ_WCET_SATURATED;
	}
	q = a / b;
	if ((a % b != 0) && ((a < 0) != (b < 0))) {
		q--;
	}
	return q;
}

static inline int64_t pz_fmod(int64_t a, int64_t b)
{
	int64_t r;

	if (b == 0) {
		return PLANOSZ_WCET_SATURATED;
	}
	r = a % b;
	if (r != 0 && ((r < 0) != (b < 0))) {
		r += b;
	}
	return r;
}

static inline int64_t pz_min(int64_t a, int64_t b) { return a < b ? a : b; }
static inline int64_t pz_max(int64_t a, int64_t b) { return a > b ? a : b; }

static inline uint32_t pz_u32(int64_t v)
{
	if (v < 0) {
		return 0;
	}
	return v > (int64_t)UINT32_MAX ? UINT32_MAX : (uint32_t)v;
}

#endif /* PLANOSZ_WCET_UTIL_H_ */
