/* SPDX-License-Identifier: Apache-2.0 */

#include <errno.h>

#include <zephyr/device.h>
#include <zephyr/drivers/sensor.h>
#include <zephyr/logging/log.h>

#include "temp.h"
#include "tempguard.h"

LOG_MODULE_REGISTER(reflow_temp, CONFIG_REFLOW_LOG_LEVEL);

#define THERMOCOUPLE_NODE DT_ALIAS(reflow_thermocouple)

#if !DT_NODE_HAS_STATUS_OKAY(THERMOCOUPLE_NODE)
#error "Missing devicetree alias 'reflow-thermocouple' (see boards/*.overlay)"
#endif

static const struct device *const tc_dev = DEVICE_DT_GET(THERMOCOUPLE_NODE);

/* Plausibility window: outside it the reading is treated as a sensor fault. */
#define TEMP_MIN_MC (-20000)
#define TEMP_MAX_MC (400000)

static struct reflow_spike spike;
/* Latches while reads are failing, so the error is logged once per outage. */
static bool err_logged;

int reflow_temp_init(void)
{
	if (!device_is_ready(tc_dev)) {
		LOG_ERR("thermocouple %s not ready", tc_dev->name);
		return -ENODEV;
	}

	reflow_spike_reset(&spike);
	err_logged = false;
	LOG_INF("thermocouple %s ready", tc_dev->name);
	return 0;
}

int reflow_temp_read(int32_t *temp_mc, int32_t *raw_mc)
{
	struct sensor_value val;
	int32_t mc, raw;
	int ret;

	ret = sensor_sample_fetch(tc_dev);
	if (ret != 0) {
		/*
		 * The MAX6675 reports an open thermocouple as -ENOENT here (it
		 * has a dedicated detection bit). Log the first failure only:
		 * at 4 reads per second a permanent open circuit would other-
		 * wise bury the console.
		 */
		if (!err_logged) {
			err_logged = true;
			LOG_ERR("sample_fetch: %d%s", ret,
				ret == -ENOENT ? " (thermocouple open)" : "");
		}
		return ret;
	}

	ret = sensor_channel_get(tc_dev, SENSOR_CHAN_AMBIENT_TEMP, &val);
	if (ret == -ENOTSUP) {
		ret = sensor_channel_get(tc_dev, SENSOR_CHAN_DIE_TEMP, &val);
	}
	if (ret != 0) {
		if (!err_logged) {
			err_logged = true;
			LOG_ERR("channel_get: %d", ret);
		}
		return ret;
	}

	mc = val.val1 * 1000 + val.val2 / 1000;

	if (mc < TEMP_MIN_MC || mc > TEMP_MAX_MC) {
		if (!err_logged) {
			err_logged = true;
			LOG_ERR("implausible reading %d mC", mc);
		}
		return -ERANGE;
	}

	/*
	 * Bounded spike rejection: see tempguard.c. The filter may lag a real
	 * step, it may not outlast one, and the raw sample leaves this function
	 * too, so the over-temperature backstop is not fed through it.
	 */
	raw = mc;
	switch (reflow_spike_filter(&spike, raw, &mc)) {
	case REFLOW_SPIKE_REJECT:
		LOG_WRN("spike rejected: %d -> %d mC", mc, raw);
		break;
	case REFLOW_SPIKE_FORCED:
		LOG_WRN("step to %d mC persisted for %d samples; believing the sensor",
			raw, REFLOW_SPIKE_MAX_REJECTS);
		break;
	case REFLOW_SPIKE_ACCEPT:
		break;
	}

	if (err_logged) {
		err_logged = false;
		LOG_INF("thermocouple reading restored");
	}

	*temp_mc = mc;
	if (raw_mc != NULL) {
		*raw_mc = raw;
	}

	return 0;
}
