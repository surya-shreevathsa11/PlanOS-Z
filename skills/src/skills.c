/* Skill handlers. They talk to the world only through the HAL and never own the scheduler.
 * Argument order follows the IDL arg order (see generated skill_table.gen.c). */
#include <stdbool.h>
#include "planosz/hal.h"
#include "planosz/skill_table.gen.h"
#include "planosz/skills.h"

enum planosz_code planosz_skill_dispatch(const struct planosz_ir_step *s, int32_t *out)
{
	const struct planosz_hal_ops *hal = planosz_backend_ops();
	int rc = 0;

	*out = 0;
	switch (s->skill_idx) {
	case PLANOSZ_SKILL_SENSOR_READ: /* args: sensor, samples */
		rc = hal->sensor_read((uint8_t)s->args[0], out);
		break;
	case PLANOSZ_SKILL_ACTUATOR_PULSE: { /* args: channel, count, on_ms, off_ms */
		for (int32_t k = 0; k < s->args[1] && rc == 0; k++) {
			rc = hal->output_set((uint8_t)s->args[0], true);
			if (rc == 0) {
				rc = hal->delay_ms((uint32_t)s->args[2]);
			}
			if (rc == 0) {
				rc = hal->output_set((uint8_t)s->args[0], false);
			}
			if (rc == 0) {
				rc = hal->delay_ms((uint32_t)s->args[3]);
			}
		}
		*out = s->args[1] * 1000;
		break;
	}
	case PLANOSZ_SKILL_WAIT: /* args: duration_ms */
		rc = hal->delay_ms((uint32_t)s->args[0]);
		*out = s->args[0];
		break;
	case PLANOSZ_SKILL_POWER_IDLE: /* args: duration_ms, mode(0=light,1=deep) */
		rc = hal->idle_ms((uint32_t)s->args[0], s->args[1] == 1);
		*out = s->args[0];
		break;
	case PLANOSZ_SKILL_SYS_INFO: { /* args: field */
		char buf[32];

		rc = hal->sys_info((uint8_t)s->args[0], buf, sizeof(buf));
		break;
	}
	case PLANOSZ_SKILL_DIAG_REPORT: /* args: scope - the trace itself is the report sink */
		*out = s->args[0] == 0 ? 1 : 4;
		break;
	case PLANOSZ_SKILL_ACTUATOR_CONTACTOR: /* args: state, settle_ms */
		rc = hal->delay_ms((uint32_t)s->args[1]);
		*out = s->args[0];
		break;
	default:
		return PLANOSZ_EXEC_BACKEND_UNAVAILABLE;
	}
	return rc == 0 ? PLANOSZ_OK : PLANOSZ_EXEC_SKILL_FAIL;
}
