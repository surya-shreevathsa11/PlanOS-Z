/* native_sim backend: deterministic simulated sensors/outputs, Zephyr kernel time.
 * Thread-safe: the parallel executor calls it from several worker threads at once. */
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <stdio.h>
#include "planosz/hal.h"

/* One simulated ADC conversion; sensor.read does `samples` of them (WCET model: 40 + 20 * samples). */
#define SIM_ADC_CONVERSION_MS 15

static const int32_t sensor_milli[4] = { 48200, 31200, 27500, 24000 }; /* pack_voltage, coolant_proxy, ... */
static const char *const channel_name[3] = { "warn_lamp", "cooling_fan", "buzzer" };

static int ns_sensor_read(uint8_t idx, int32_t *milli)
{
	if (idx >= ARRAY_SIZE(sensor_milli)) {
		return -EINVAL;
	}
	k_msleep(SIM_ADC_CONVERSION_MS);
	*milli = sensor_milli[idx];
	return 0;
}

static int ns_output_set(uint8_t ch, bool on)
{
	if (ch >= ARRAY_SIZE(channel_name)) {
		return -EINVAL;
	}
	printk("[sim] %s -> %s\n", channel_name[ch], on ? "ON" : "off");
	return 0;
}

static int ns_delay_ms(uint32_t ms)
{
	k_msleep(ms);
	return 0;
}

static int ns_idle_ms(uint32_t ms, bool deep)
{
	ARG_UNUSED(deep);
	k_msleep(ms);
	return 0;
}

static int ns_sys_info(uint8_t field, char *buf, size_t len)
{
	switch (field) {
	case 0: snprintf(buf, len, "%ums", (unsigned int)k_uptime_get_32()); break;
	case 1: snprintf(buf, len, "planosz-1.0.0"); break;
	case 2: snprintf(buf, len, "native_sim"); break;
	default: snprintf(buf, len, "n/a"); break;
	}
	return 0;
}

static uint32_t ns_uptime_ms(void)
{
	return k_uptime_get_32();
}

static const struct planosz_hal_ops ops = {
	.sensor_read = ns_sensor_read,
	.output_set = ns_output_set,
	.delay_ms = ns_delay_ms,
	.idle_ms = ns_idle_ms,
	.sys_info = ns_sys_info,
	.uptime_ms = ns_uptime_ms,
};

const struct planosz_hal_ops *planosz_backend_ops(void)
{
	return &ops;
}
