/* Narrow backend HAL. The runtime core only ever calls through this table, so
 * native_sim and ESP32 differ by exactly one file each (backends/<name>/). */
#ifndef PLANOSZ_HAL_H_
#define PLANOSZ_HAL_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct planosz_hal_ops {
	/* sensor_idx follows the sensor.read enum order; value in milli-units */
	int (*sensor_read)(uint8_t sensor_idx, int32_t *milli);
	/* channel_idx follows the actuator.pulse enum order */
	int (*output_set)(uint8_t channel_idx, bool on);
	int (*delay_ms)(uint32_t ms);
	int (*idle_ms)(uint32_t ms, bool deep);
	int (*sys_info)(uint8_t field, char *buf, size_t len);
	uint32_t (*uptime_ms)(void);
};

/* Implemented once per backend (backends/native_sim, backends/esp32, tests/host). */
const struct planosz_hal_ops *planosz_backend_ops(void);

#endif /* PLANOSZ_HAL_H_ */
