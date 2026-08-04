/* SPDX-License-Identifier: Apache-2.0 */

#include <errno.h>

#include <zephyr/device.h>
#include <zephyr/drivers/sensor.h>
#include <zephyr/logging/log.h>

#include "temp.h"

LOG_MODULE_REGISTER(reflow_temp, CONFIG_REFLOW_LOG_LEVEL);

#define THERMOCOUPLE_NODE DT_ALIAS(reflow_thermocouple)

#if !DT_NODE_HAS_STATUS(THERMOCOUPLE_NODE, okay)
#error "Missing devicetree alias 'reflow-thermocouple' (see boards/*.overlay)"
#endif

static const struct device *const tc_dev = DEVICE_DT_GET(THERMOCOUPLE_NODE);

/* Plausibility window: outside it the reading is treated as a sensor fault. */
#define TEMP_MIN_MC (-20000)
#define TEMP_MAX_MC (400000)

static int32_t last_mc;
static bool have_last;

int reflow_temp_init(void)
{
	if (!device_is_ready(tc_dev)) {
		LOG_ERR("thermocouple %s not ready", tc_dev->name);
		return -ENODEV;
	}

	have_last = false;
	LOG_INF("thermocouple %s ready", tc_dev->name);
	return 0;
}

int reflow_temp_read(int32_t *temp_mc)
{
	struct sensor_value val;
	int32_t mc;
	int ret;

	ret = sensor_sample_fetch(tc_dev);
	if (ret != 0) {
		/* MAX6675 reports an open thermocouple as a fetch error. */
		LOG_ERR("sample_fetch: %d", ret);
		return ret;
	}

	ret = sensor_channel_get(tc_dev, SENSOR_CHAN_AMBIENT_TEMP, &val);
	if (ret == -ENOTSUP) {
		ret = sensor_channel_get(tc_dev, SENSOR_CHAN_DIE_TEMP, &val);
	}
	if (ret != 0) {
		LOG_ERR("channel_get: %d", ret);
		return ret;
	}

	mc = val.val1 * 1000 + val.val2 / 1000;

	if (mc < TEMP_MIN_MC || mc > TEMP_MAX_MC) {
		LOG_ERR("implausible reading %d mC", mc);
		return -ERANGE;
	}

	/*
	 * Single-sample spike rejection. The MAX6675 has 0.25 degC resolution
	 * and a ~220 ms conversion; a jump of more than 40 degC between
	 * consecutive samples is electrical noise, not physics.
	 */
	if (have_last && (mc - last_mc > 40000 || last_mc - mc > 40000)) {
		LOG_WRN("spike rejected: %d -> %d mC", last_mc, mc);
		mc = last_mc;
	}

	last_mc = mc;
	have_last = true;
	*temp_mc = mc;

	return 0;
}
