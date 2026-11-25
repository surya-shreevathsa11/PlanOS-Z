/* ESP32 backend (stub-with-teeth: real GPIO outputs, simulated sensors).
 *  - Outputs: real GPIO through devicetree aliases `warn-lamp`, `cooling-fan`, `buzzer`
 *    (see apps/planosz_node/boards/esp32_devkitc_procpu.overlay).
 *  - Sensors: SIMULATED values until real ADC/I2C parts are wired (roadmap phase 8).
 * The runtime core is untouched: only this file differs from the native_sim backend. */
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/kernel.h>
#include <stdio.h>
#include "planosz/hal.h"

#define OUT_SPEC(alias) GPIO_DT_SPEC_GET(DT_ALIAS(alias), gpios)

static const struct gpio_dt_spec outputs[3] = {
	OUT_SPEC(warn_lamp),
	OUT_SPEC(cooling_fan),
	OUT_SPEC(buzzer),
};
#define SIM_ADC_CONVERSION_MS 15 /* simulated conversion time per sample until a real ADC is wired */

static const int32_t sensor_milli[4] = { 48200, 31200, 27500, 24000 };
static bool outputs_ready;

static int init_outputs(void)
{
	for (size_t i = 0; i < ARRAY_SIZE(outputs); i++) {
		if (!gpio_is_ready_dt(&outputs[i]) ||
		    gpio_pin_configure_dt(&outputs[i], GPIO_OUTPUT_INACTIVE) != 0) {
			return -ENODEV;
		}
	}
	outputs_ready = true;
	return 0;
}

static int esp_sensor_read(uint8_t idx, int32_t *milli)
{
	if (idx >= ARRAY_SIZE(sensor_milli)) {
		return -EINVAL;
	}
	k_msleep(SIM_ADC_CONVERSION_MS);
	*milli = sensor_milli[idx]; /* TODO(phase 8): ADC / I2C driver */
	return 0;
}

static int esp_output_set(uint8_t ch, bool on)
{
	if (!outputs_ready && init_outputs() != 0) {
		return -ENODEV;
	}
	if (ch >= ARRAY_SIZE(outputs)) {
		return -EINVAL;
	}
	return gpio_pin_set_dt(&outputs[ch], on);
}

static int esp_delay_ms(uint32_t ms)
{
	k_msleep(ms);
	return 0;
}

static int esp_idle_ms(uint32_t ms, bool deep)
{
	ARG_UNUSED(deep);
	k_msleep(ms); /* TODO(phase 8): PM light/deep sleep */
	return 0;
}

static int esp_sys_info(uint8_t field, char *buf, size_t len)
{
	switch (field) {
	case 0: snprintf(buf, len, "%ums", (unsigned int)k_uptime_get_32()); break;
	case 1: snprintf(buf, len, "planosz-1.0.0"); break;
	case 2: snprintf(buf, len, "esp32_devkitc"); break;
	default: snprintf(buf, len, "n/a"); break;
	}
	return 0;
}

static uint32_t esp_uptime_ms(void)
{
	return k_uptime_get_32();
}

static const struct planosz_hal_ops ops = {
	.sensor_read = esp_sensor_read,
	.output_set = esp_output_set,
	.delay_ms = esp_delay_ms,
	.idle_ms = esp_idle_ms,
	.sys_info = esp_sys_info,
	.uptime_ms = esp_uptime_ms,
};

const struct planosz_hal_ops *planosz_backend_ops(void)
{
	return &ops;
}
