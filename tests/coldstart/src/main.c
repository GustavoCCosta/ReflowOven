/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * RFO-B39: the oven that powered up with no thermocouple has to come back when
 * the probe is plugged in, without a reboot.
 *
 * This is its own image, not a test in tests/controller, for the same reason
 * tests/boot is its own image: the property is about the state the control
 * thread starts in, and the thread starts before any test body can influence
 * it. So the influence is a build-time value - REFLOW_TEMP_FAKE_INIT_FAILURES=1
 * in this directory's CMakeLists - and the whole image is built around a
 * front-end that is missing at boot and appears afterwards.
 *
 * The alternative, a runtime knob on the fake, is what RFO-G17 deleted: it
 * could never be set in time to matter and it advertised a capability it did
 * not have.
 *
 * The fake and every core file are shared with tests/controller, not copied.
 */

#include <zephyr/kernel.h>
#include <zephyr/ztest.h>
#include <zephyr/zbus/zbus.h>

#include "app.h"
#include "heater.h"
#include "temp_fake.h"

/*
 * The controller retries every CONFIG_REFLOW_SENSOR_RETRY_MS and then needs a
 * sampling period on top. Give the waits several of both.
 */
#define SETTLE_MS (CONFIG_REFLOW_SENSOR_RETRY_MS + 4 * CONFIG_REFLOW_SAMPLE_PERIOD_MS + 2000)

static bool wait_temp_valid(uint8_t want, struct reflow_telemetry *last)
{
	int64_t deadline = k_uptime_get() + SETTLE_MS;

	while (true) {
		zassert_ok(zbus_chan_read(&reflow_telemetry_chan, last, K_MSEC(200)),
			   "telemetry channel unreadable");
		if (last->temp_valid == want) {
			return true;
		}
		if (k_uptime_get() >= deadline) {
			return false;
		}
		k_msleep(10);
	}
}

static bool wait_state(uint8_t want, struct reflow_telemetry *last)
{
	int64_t deadline = k_uptime_get() + SETTLE_MS;

	while (true) {
		zassert_ok(zbus_chan_read(&reflow_telemetry_chan, last, K_MSEC(200)),
			   "telemetry channel unreadable");
		if (last->state == want) {
			return true;
		}
		if (k_uptime_get() >= deadline) {
			return false;
		}
		k_msleep(10);
	}
}

static void post(uint8_t id)
{
	struct reflow_cmd cmd = { .id = id, .arg = 0 };

	zassert_ok(reflow_cmd_post(&cmd, K_MSEC(200)), "command queue full");
}

/*
 * The whole defect, in one run: boot with no front-end, clear the fault, and
 * the oven must start sampling and accept a START - all without a reboot.
 *
 * Without the patch the controller evaluates `ready` exactly once. CLEAR_FAULT
 * still returns the state to IDLE, so the first half of this test passes
 * unchanged; what never happens is the sampling coming back, so temp_valid
 * stays 0 and START is refused for ever. That is the assertion that goes red.
 */
ZTEST(reflow_coldstart, test_forno_volta_quando_o_termopar_aparece)
{
	struct reflow_telemetry t = { 0 };

	/*
	 * The image booted with reflow_temp_init() failing once, so the thread
	 * latched FAULT_SENSOR before any test body ran. Assert that, because
	 * if it is not true this test is measuring nothing.
	 */
	zassert_true(wait_state(REFLOW_STATE_FAULT, &t),
		     "a imagem nao subiu com o front-end faltando (estado %s): "
		     "este teste deixou de exercitar o defeito",
		     reflow_state_str(t.state));
	zassert_equal(t.fault, REFLOW_FAULT_SENSOR,
		      "esperava FAULT_SENSOR no boot sem termopar, veio %s",
		      reflow_fault_str(t.fault));
	zassert_equal(t.temp_valid, 0, "leitura valida sem front-end");

	/* The operator plugs the probe in and clears. */
	post(REFLOW_CMD_CLEAR_FAULT);
	zassert_true(wait_state(REFLOW_STATE_IDLE, &t),
		     "a falta nao limpou (estado %s)", reflow_state_str(t.state));

	/* THE assertion: sampling comes back on its own, with no reboot. */
	zassert_true(wait_temp_valid(1, &t),
		     "o forno nunca voltou a amostrar depois que o front-end "
		     "apareceu: inoperante ate o reboot");

	/* And the oven is usable again, which is what the operator asked for. */
	post(REFLOW_CMD_START);
	zassert_true(wait_state(REFLOW_STATE_RUNNING, &t),
		     "START recusado depois da recuperacao (estado %s, falta %s)",
		     reflow_state_str(t.state), reflow_fault_str(t.fault));

	post(REFLOW_CMD_STOP);
}

/*
 * The safety half of the acceptance criteria, from the outside: while the
 * front-end was missing the element must never have been energised. The retry
 * only runs outside RUNNING, and a run cannot exist without a valid reading, so
 * there is no path from here to a live element - this asserts the observable
 * consequence rather than the reasoning.
 */
ZTEST(reflow_coldstart, test_o_elemento_fica_desligado_durante_a_recuperacao)
{
	struct reflow_telemetry t = { 0 };

	zassert_false(reflow_heater_is_on(),
		      "elemento energizado durante a recuperacao do front-end");

	post(REFLOW_CMD_STOP);
	post(REFLOW_CMD_CLEAR_FAULT);
	zassert_true(wait_temp_valid(1, &t), "front-end nao voltou");

	zassert_false(reflow_heater_is_on(),
		      "elemento energizado com o forno parado apos a recuperacao");
	zassert_equal(t.duty_permille, 0, "duty %u com o forno parado", t.duty_permille);
}

ZTEST_SUITE(reflow_coldstart, NULL, NULL, NULL, NULL, NULL);
