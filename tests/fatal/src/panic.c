/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * RFO-B44, the half that fires a REAL fatal error.
 *
 * Not a ztest, and it cannot be one. ztest's own k_sys_fatal_error_handler
 * (CONFIG_ZTEST_FATAL_HOOK) is a strong symbol that would collide at link with
 * the one in src/core/fatal.c, and a genuine fatal error ends in
 * `Halting system` - the CPU stops, so no assertion can run afterwards inside
 * this image. What is left is the console, and that is enough for the one thing
 * this scenario has to establish: that the handler in fatal.c is the one the
 * kernel really reaches when a thread panics, and that it cuts.
 *
 * The gate is driven HIGH first, through the real path, and the level is printed
 * before the fault. A run that does not print `gate before the fault: 1` proves
 * nothing about what came after, so the harness requires that line too.
 *
 * The scenario is `ignore_faults: true` in testcase.yaml: the fatal error is the
 * subject, not an accident.
 */

#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/gpio/gpio_emul.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>

#include "heater.h"

#define SSR_NODE DT_ALIAS(reflow_ssr)

static const struct gpio_dt_spec ssr = GPIO_DT_SPEC_GET(SSR_NODE, gpios);

int main(void)
{
	int ret = reflow_heater_init();

	if (ret != 0) {
		printk("reflow-fatal: heater init failed: %d\n", ret);
		return 0;
	}

	/* The real path to a high gate: full duty, then one window tick. */
	reflow_heater_set_duty(1000);
	reflow_heater_tick(0);

	printk("reflow-fatal: gate before the fault: %d\n",
	       gpio_emul_output_get(ssr.port, ssr.pin));

	/*
	 * k_panic() and not a wild pointer: the ticket is about the DESTINATION
	 * of any fatal error, not about one cause, and a deliberate panic is the
	 * one cause that behaves identically on every platform this suite runs
	 * on. What it exercises is z_fatal_error() -> k_sys_fatal_error_handler,
	 * which is the same entry the CPU exception of RFO-B43 takes.
	 */
	k_panic();

	printk("reflow-fatal: still running after k_panic\n");
	return 0;
}
