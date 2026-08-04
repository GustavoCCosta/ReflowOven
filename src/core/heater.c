/* SPDX-License-Identifier: Apache-2.0 */

#include <errno.h>

#include <zephyr/drivers/gpio.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include "heater.h"

LOG_MODULE_REGISTER(reflow_heater, CONFIG_REFLOW_LOG_LEVEL);

#define SSR_NODE DT_ALIAS(reflow_ssr)

#if !DT_NODE_EXISTS(SSR_NODE)
#error "Missing devicetree alias 'reflow-ssr' (see boards/*.overlay)"
#endif

static const struct gpio_dt_spec ssr = GPIO_DT_SPEC_GET(SSR_NODE, gpios);

#define WINDOW_MS    CONFIG_REFLOW_HEATER_WINDOW_MS
#define MIN_PULSE_MS CONFIG_REFLOW_HEATER_MIN_PULSE_MS
#define STALE_MS     CONFIG_REFLOW_HEATER_STALE_MS

static struct k_spinlock lock;
static uint16_t req_permille;
static uint32_t phase_ms;
static int64_t last_update;
static bool output_on;

static void drive(bool on)
{
	if (on == output_on) {
		return;
	}
	(void)gpio_pin_set_dt(&ssr, on ? 1 : 0);
	output_on = on;
}

int reflow_heater_init(void)
{
	int ret;

	if (!gpio_is_ready_dt(&ssr)) {
		LOG_ERR("SSR gpio not ready");
		return -ENODEV;
	}

	/* Inactive at reset: never energise the element before the loop runs. */
	ret = gpio_pin_configure_dt(&ssr, GPIO_OUTPUT_INACTIVE);
	if (ret != 0) {
		return ret;
	}

	output_on = false;
	req_permille = 0;
	phase_ms = 0;
	last_update = k_uptime_get();

	LOG_INF("SSR on %s pin %d, window %d ms", ssr.port->name, ssr.pin, WINDOW_MS);
	return 0;
}

void reflow_heater_set_duty(uint16_t permille)
{
	k_spinlock_key_t key = k_spin_lock(&lock);

	if (permille > 1000U) {
		permille = 1000U;
	}

	/*
	 * Snap duties that would produce a pulse shorter than the SSR can
	 * honour: an unrealisably short pulse is worse than no pulse.
	 */
	if ((uint32_t)permille * WINDOW_MS / 1000U < MIN_PULSE_MS) {
		permille = 0;
	} else if ((uint32_t)(1000U - permille) * WINDOW_MS / 1000U < MIN_PULSE_MS) {
		permille = 1000;
	}

	req_permille = permille;
	last_update = k_uptime_get();
	k_spin_unlock(&lock, key);
}

void reflow_heater_off(void)
{
	k_spinlock_key_t key = k_spin_lock(&lock);

	req_permille = 0;
	last_update = k_uptime_get();
	drive(false);
	k_spin_unlock(&lock, key);
}

void reflow_heater_tick(uint32_t dt_ms)
{
	k_spinlock_key_t key = k_spin_lock(&lock);
	uint32_t on_ms;

	if (k_uptime_get() - last_update > STALE_MS) {
		/* Control loop stopped feeding us: fail safe. */
		req_permille = 0;
		drive(false);
		k_spin_unlock(&lock, key);
		LOG_WRN("duty request stale, output forced off");
		return;
	}

	phase_ms += dt_ms;
	if (phase_ms >= WINDOW_MS) {
		phase_ms %= WINDOW_MS;
	}

	on_ms = (uint32_t)req_permille * WINDOW_MS / 1000U;
	drive(phase_ms < on_ms);

	k_spin_unlock(&lock, key);
}

uint16_t reflow_heater_duty(void)
{
	return req_permille;
}

bool reflow_heater_is_on(void)
{
	return output_on;
}
