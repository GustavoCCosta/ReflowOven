/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * RFO-T13, the part of it that does not need an oscilloscope.
 *
 * The bench test in RFO-T13 watches the real gate from power-on reset; that is
 * hardware work and this suite does not pretend to replace it. What it does
 * cover is the window the firmware is responsible for: from the moment the GPIO
 * controller exists until the control thread first runs. Before RFO-B06 nothing
 * claimed the pin in that window, so it sat as a high-impedance input for the
 * 100 ms K_THREAD_DEFINE delay, and an opto-coupled SSR with a pull-up on its
 * input reads high impedance as ON.
 *
 * The image deliberately contains heater.c and no control thread, so the only
 * thing that can have configured the pin by the time a test runs is the
 * SYS_INIT hook under test.
 */

#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/gpio/gpio_emul.h>
#include <zephyr/ztest.h>

#define SSR_NODE DT_ALIAS(reflow_ssr)

static const struct gpio_dt_spec ssr = GPIO_DT_SPEC_GET(SSR_NODE, gpios);

/*
 * ztest runs at application level, after every SYS_INIT level has completed and
 * before anything in this image drives the heater. Whatever the pin looks like
 * here is what it looked like for the whole 100 ms the control thread used to
 * spend sleeping.
 */
ZTEST(reflow_boot, test_ssr_is_an_output_before_any_thread_runs)
{
	gpio_flags_t flags = 0;

	zassert_true(gpio_is_ready_dt(&ssr), "emulated gpio not ready");

	zassert_ok(gpio_emul_flags_get(ssr.port, ssr.pin, &flags),
		   "could not read the gate's configuration");

	/*
	 * This is the assertion that fails without the patch: an unclaimed pin
	 * has no GPIO_OUTPUT bit, which on real hardware is the high-impedance
	 * state the SSR module misreads as ON.
	 */
	zassert_true((flags & GPIO_OUTPUT) != 0,
		     "the gate is not driven at boot (flags 0x%x): high impedance "
		     "until the control thread starts",
		     (unsigned int)flags);
}

ZTEST(reflow_boot, test_ssr_is_low_before_any_thread_runs)
{
	int val = gpio_emul_output_get(ssr.port, ssr.pin);

	zassert_true(val >= 0, "could not read the gate: %d", val);
	zassert_equal(val, 0, "the gate is high at boot with no loop running");
}

/*
 * Sanity check on THIS SUITE'S OWN fixture, and nothing more.
 *
 * `ssr` comes from DT_ALIAS(reflow_ssr) resolved in the devicetree of this test
 * application — tests/boot/boards/<platform>.overlay. So what the assertion
 * guards is that the simulated overlay declares GPIO_ACTIVE_HIGH, which is what
 * makes the "physically low" reading in the test above meaningful. It says
 * nothing about the three board overlays: flipping the polarity in the Pico or
 * ESP32 overlay — the only ones where the mistake energises something real — is
 * invisible from here.
 *
 * The guarantee for the real targets is the BUILD_ASSERT on DT_GPIO_FLAGS in
 * heater.c, which resolves against the devicetree actually being built and so
 * fails the build on every board. Keep both: this one stops the fixture from
 * drifting out from under the test above, that one stops the boards.
 */
ZTEST(reflow_boot, test_inactive_means_physically_low)
{
	zassert_equal(ssr.dt_flags & GPIO_ACTIVE_LOW, 0,
		      "the SSR is declared active-low; GPIO_OUTPUT_INACTIVE would "
		      "then drive the gate high at boot");
}

ZTEST_SUITE(reflow_boot, NULL, NULL, NULL, NULL, NULL);
