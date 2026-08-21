/* SPDX-License-Identifier: Apache-2.0 */

#include <errno.h>

#include <zephyr/drivers/gpio.h>
#include <zephyr/init.h>
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

/*
 * Drive the gate low as early as the GPIO driver allows, without waiting for
 * the control thread.
 *
 * Why this exists (RFO-B06): the only thing that used to configure this pin was
 * reflow_heater_init(), reached from controller_thread, which K_THREAD_DEFINE
 * starts 100 ms after the kernel. Until then the pin was an input — high
 * impedance — and an opto-coupled SSR module with a pull-up on its input reads
 * that as ON. A board stuck in a reset loop would then heat continuously with
 * no loop running at all.
 *
 * Why POST_KERNEL and not PRE_KERNEL_1: the pin cannot be touched before the
 * GPIO controller exists, and on these targets it does not exist that early.
 * The RP2350 driver initialises at POST_KERNEL/CONFIG_GPIO_INIT_PRIORITY, the
 * native_sim emulator likewise; only the ESP32 driver is PRE_KERNEL_1, and even
 * there it sits at priority 40, so a PRE_KERNEL_1 hook at priority 0 would run
 * before it and find no device. POST_KERNEL at a numerically later priority is
 * the earliest level that works on every target, and it still completes before
 * the kernel starts any application thread.
 *
 * What this does NOT fix: the window from power-on reset until this runs. No
 * software can cover that — the pin is whatever the SoC's reset state makes it.
 * That window is why the hardware needs a pull-down on the gate; see the
 * "Safety" section of README.md.
 */
BUILD_ASSERT(CONFIG_KERNEL_INIT_PRIORITY_DEVICE > CONFIG_GPIO_INIT_PRIORITY,
	     "the SSR safe-init must run after the GPIO driver, not before it");

static int ssr_safe_init(void)
{
	int ret;

	if (!gpio_is_ready_dt(&ssr)) {
		LOG_ERR("SSR gpio not ready at boot; gate left in reset state");
		return -ENODEV;
	}

	ret = gpio_pin_configure_dt(&ssr, GPIO_OUTPUT_INACTIVE);
	if (ret != 0) {
		LOG_ERR("SSR gpio configure at boot: %d", ret);
	}

	return ret;
}

SYS_INIT(ssr_safe_init, POST_KERNEL, CONFIG_KERNEL_INIT_PRIORITY_DEVICE);

int reflow_heater_init(void)
{
	int ret;

	if (!gpio_is_ready_dt(&ssr)) {
		LOG_ERR("SSR gpio not ready");
		return -ENODEV;
	}

	/*
	 * Redundant with ssr_safe_init() above, deliberately: this is also the
	 * path that reports a GPIO failure to the controller, which turns it
	 * into a latched fault instead of a silent boot message.
	 */
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
