/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * RFO-B44, the half that can assert on the gate.
 *
 * The other half is the reflow.fatal.panic scenario next to this one, which
 * fires a real k_panic() and is judged by the console harness. It has to be a
 * separate image for two reasons: ztest's own k_sys_fatal_error_handler is a
 * strong symbol that collides with fatal.c, and a genuine fatal error halts the
 * CPU, so nothing in that image can run an assertion afterwards. Between the
 * two: this suite proves the cut drives the gate low from the states that
 * matter, that one proves the handler is really reached when the CPU faults.
 *
 * Every test here starts from the gate HIGH. A test that started with the gate
 * low would pass on an empty function.
 */

#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/gpio/gpio_emul.h>
#include <zephyr/kernel.h>
#include <zephyr/ztest.h>

#include "heater.h"

#define SSR_NODE DT_ALIAS(reflow_ssr)

static const struct gpio_dt_spec ssr = GPIO_DT_SPEC_GET(SSR_NODE, gpios);

/* Drive the gate high through the real path: full duty, then one window tick. */
static void energise(void)
{
	zassert_ok(reflow_heater_init(), "heater init failed");

	reflow_heater_set_duty(1000);
	reflow_heater_tick(0);

	zassert_equal(gpio_emul_output_get(ssr.port, ssr.pin), 1,
		      "fixture is broken: the gate is not high before the cut, so "
		      "this test would pass on an empty function");
}

ZTEST(reflow_fatal, test_emergency_cut_drives_the_gate_low)
{
	energise();

	reflow_heater_emergency_off();

	zassert_equal(gpio_emul_output_get(ssr.port, ssr.pin), 0,
		      "the gate is still high after the emergency cut");
}

/*
 * The cut has to be idempotent: the fatal path can be re-entered (a fault while
 * handling a fault), and a second call must not undo the first.
 */
ZTEST(reflow_fatal, test_emergency_cut_is_idempotent)
{
	energise();

	reflow_heater_emergency_off();
	reflow_heater_emergency_off();

	zassert_equal(gpio_emul_output_get(ssr.port, ssr.pin), 0,
		      "the gate came back after a second cut");
	zassert_false(reflow_heater_is_on(), "the module still reports the output on");
}

/*
 * Same fixture check as tests/boot: the "physically low" readings above only
 * mean anything while this suite's own overlay declares GPIO_ACTIVE_HIGH. The
 * guarantee for the real boards is the BUILD_ASSERT in heater.c.
 */
ZTEST(reflow_fatal, test_inactive_means_physically_low)
{
	zassert_equal(ssr.dt_flags & GPIO_ACTIVE_LOW, 0,
		      "the SSR is declared active-low in this fixture; a low gate "
		      "would then mean the element is ON");
}

ZTEST_SUITE(reflow_fatal, NULL, NULL, NULL, NULL, NULL);
