/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * RFO-G06: the controller, exercised against an injectable thermocouple.
 *
 * Until this suite existed, controller.c and the sensor path had no coverage at
 * all, for a mechanical reason: on native_sim the emulated SPI bus has no
 * MAX6675 behind it, so reflow_temp_init() fails, the controller latches
 * FAULT_SENSOR and a run never starts. The image built here links
 * tests/controller/src/temp_fake.c instead of src/core/temp.c, so the sample is
 * whatever the test says it is, and every other file in the control path --
 * controller.c, heater.c, pid.c, profile.c, tempguard.c -- is the shipped one.
 *
 * Everything is observed the way the rest of the firmware observes it: the
 * telemetry channel out, reflow_cmd_post() in. The tests therefore also pin the
 * contract in app.h that makes the optional features removable.
 */

#include <errno.h>

#include <zephyr/kernel.h>
#include <zephyr/ztest.h>
#include <zephyr/zbus/zbus.h>

#include "app.h"
#include "heater.h"
#include "temp_fake.h"

#define ABS_MAX_MC (CONFIG_REFLOW_ABS_MAX_TEMP_C * 1000)

/* One sample period is 250 ms by default; give every wait several of them. */
#define SETTLE_MS 3000

typedef bool (*pred_fn)(const struct reflow_telemetry *t, int arg);

static bool wait_for_ms(pred_fn pred, int arg, struct reflow_telemetry *last,
			int64_t budget_ms)
{
	int64_t deadline = k_uptime_get() + budget_ms;

	while (true) {
		zassert_ok(zbus_chan_read(&reflow_telemetry_chan, last, K_MSEC(200)),
			   "telemetry channel unreadable");
		if (pred(last, arg)) {
			return true;
		}
		if (k_uptime_get() >= deadline) {
			return false;
		}
		k_msleep(10);
	}
}

static bool wait_for(pred_fn pred, int arg, struct reflow_telemetry *last)
{
	return wait_for_ms(pred, arg, last, SETTLE_MS);
}

static bool is_state(const struct reflow_telemetry *t, int arg)
{
	return t->state == (uint8_t)arg;
}

static bool is_fault(const struct reflow_telemetry *t, int arg)
{
	return t->state == REFLOW_STATE_FAULT && t->fault == (uint8_t)arg;
}

static bool temp_valid_is(const struct reflow_telemetry *t, int arg)
{
	return t->temp_valid == (uint8_t)arg;
}

static bool temp_reads(const struct reflow_telemetry *t, int arg)
{
	return t->temp_valid != 0U && t->temp_mc == (int32_t)arg;
}

static void post(uint8_t id, int32_t arg)
{
	struct reflow_cmd cmd = { .id = id, .arg = arg };

	zassert_ok(reflow_cmd_post(&cmd, K_MSEC(200)), "command queue full");
}

/* Block until the control thread has taken n more samples from the fake. */
static void wait_samples(uint32_t n)
{
	uint32_t start = reflow_temp_fake_reads();
	int64_t deadline = k_uptime_get() + SETTLE_MS;

	while (reflow_temp_fake_reads() - start < n) {
		zassert_true(k_uptime_get() < deadline,
			     "the control thread stopped sampling");
		k_msleep(10);
	}
}

/*
 * Take the oven from wherever the previous test left it back to a clean idle.
 *
 * The order matters. The healthy reading has to reach the controller BEFORE the
 * fault is cleared: the absolute backstop looks at the last raw sample, so
 * clearing first would have it re-trip on the value the previous test injected,
 * and the next test would run against an oven that is quietly back in FAULT.
 */
static void fresh_idle(void *unused)
{
	struct reflow_telemetry t;

	ARG_UNUSED(unused);

	reflow_temp_fake_reset();
	wait_samples(2);
	post(REFLOW_CMD_STOP, 0);
	post(REFLOW_CMD_CLEAR_FAULT, 0);

	zassert_true(wait_for(is_state, REFLOW_STATE_IDLE, &t),
		     "oven did not return to idle (state %s, fault %s)",
		     reflow_state_str(t.state), reflow_fault_str(t.fault));
	zassert_true(wait_for(temp_valid_is, 1, &t),
		     "no valid reading after resetting the fake sensor");
	zassert_false(reflow_heater_is_on(), "element still energised while idle");
}

/*
 * Criterion 1: the suite can pin the reading to an arbitrary value.
 *
 * 55 degC is a 30 degC step from the fake's 25 degC idle, deliberately under
 * REFLOW_SPIKE_MAX_STEP_MC: the value has to arrive unchanged. Anything larger
 * is a step the real filter suppresses, which is what the last test uses.
 */
ZTEST(reflow_controller, test_injected_reading_reaches_the_telemetry)
{
	struct reflow_telemetry t;

	reflow_temp_fake_set_raw_mc(55000);

	zassert_true(wait_for(temp_reads, 55000, &t),
		     "controller reported %d mC, not the injected 55000 mC",
		     t.temp_mc);
}

/*
 * Criterion 2, the SPI half: a failing transfer with the oven idle must not
 * latch a fault -- the element is already off, so there is nothing to protect
 * against -- but it must invalidate the reading, and START must then refuse.
 */
ZTEST(reflow_controller, test_spi_error_while_idle_only_refuses_to_start)
{
	struct reflow_telemetry t;

	reflow_temp_fake_fail(-EIO);

	zassert_true(wait_for(temp_valid_is, 0, &t),
		     "reading still marked valid after an SPI error");
	zassert_equal(t.state, REFLOW_STATE_IDLE, "SPI error latched a fault while idle");

	post(REFLOW_CMD_START, 0);
	k_msleep(1000);

	zassert_ok(zbus_chan_read(&reflow_telemetry_chan, &t, K_MSEC(200)), NULL);
	zassert_equal(t.state, REFLOW_STATE_IDLE,
		      "the oven started with no valid temperature (state %s)",
		      reflow_state_str(t.state));
	zassert_false(reflow_heater_is_on(), "element energised with no valid temperature");
}

/*
 * Criterion 2, the open-thermocouple half. The MAX6675 driver reports an open
 * junction as -ENOENT, and an oven powered up with nothing plugged in must not
 * need a "reflow clear" once the probe is connected.
 */
ZTEST(reflow_controller, test_open_thermocouple_while_idle_recovers_without_a_clear)
{
	struct reflow_telemetry t;

	reflow_temp_fake_fail(-ENOENT);
	zassert_true(wait_for(temp_valid_is, 0, &t), "open thermocouple went unnoticed");
	zassert_equal(t.fault, REFLOW_FAULT_NONE,
		      "an open thermocouple latched %s while idle",
		      reflow_fault_str(t.fault));

	reflow_temp_fake_fail(0);
	zassert_true(wait_for(temp_valid_is, 1, &t),
		     "reading did not come back after the probe was reconnected");
	zassert_equal(t.state, REFLOW_STATE_IDLE, NULL);
}

/*
 * Losing the sensor mid-run is the dangerous case: the element can be on and
 * the loop is blind. It has to latch and stay latched until an explicit clear.
 */
ZTEST(reflow_controller, test_sensor_loss_during_a_run_latches_and_cuts_the_element)
{
	struct reflow_telemetry t;

	post(REFLOW_CMD_START, 0);
	zassert_true(wait_for(is_state, REFLOW_STATE_RUNNING, &t), "the run did not start");

	reflow_temp_fake_fail(-EIO);

	zassert_true(wait_for(is_fault, REFLOW_FAULT_SENSOR, &t),
		     "losing the sensor mid-run left the oven in %s/%s",
		     reflow_state_str(t.state), reflow_fault_str(t.fault));
	zassert_equal(t.duty_permille, 0, "duty still %u after a sensor fault",
		      t.duty_permille);
	zassert_false(reflow_heater_is_on(), "element still energised after a sensor fault");

	/* The latch: START must bounce off it even once the sensor is back. */
	reflow_temp_fake_fail(0);
	post(REFLOW_CMD_START, 0);
	k_msleep(1000);

	zassert_ok(zbus_chan_read(&reflow_telemetry_chan, &t, K_MSEC(200)), NULL);
	zassert_equal(t.state, REFLOW_STATE_FAULT,
		      "the fault did not survive a START (state %s)",
		      reflow_state_str(t.state));
}

/*
 * Criterion 3, and the reason this suite is worth its weight: the RFO-B05
 * scenario, at the level where it actually bites.
 *
 * The injected step is far larger than REFLOW_SPIKE_MAX_STEP_MC, so the real
 * spike rejector in tempguard.c holds the reported temperature still for the
 * next couple of samples. The control loop therefore sees a cool oven while the
 * chip is reading 300 degC. Only the raw sample can trip the absolute cut-out,
 * and this asserts that it does -- with the filtered value still far below the
 * limit at the moment the fault is raised, which is what proves the backstop
 * did not get there through the filtered path.
 */
ZTEST(reflow_controller, test_a_stuck_reading_does_not_blind_the_absolute_cut_out)
{
	struct reflow_telemetry t;

	post(REFLOW_CMD_START, 0);
	zassert_true(wait_for(is_state, REFLOW_STATE_RUNNING, &t), "the run did not start");

	reflow_temp_fake_set_raw_mc(300000);

	zassert_true(wait_for(is_fault, REFLOW_FAULT_OVERTEMP, &t),
		     "the oven read 300000 mC and stayed in %s/%s: the spike filter "
		     "suppressed the rise and the cut-out never saw it",
		     reflow_state_str(t.state), reflow_fault_str(t.fault));
	zassert_true(t.temp_mc < ABS_MAX_MC,
		     "the filtered reading (%d mC) had already passed the limit, so "
		     "this says nothing about the raw path", t.temp_mc);
	zassert_false(reflow_heater_is_on(), "element still energised after over-temperature");
}

/*
 * RFO-T12: the absolute cut-out is a backstop, not a step of a run.
 *
 * The neighbouring test above proves the raw path trips it, but it does so
 * with the oven RUNNING, so it says nothing about an idle oven. That
 * distinction is not academic: an oven can be hot with no run in progress --
 * the run has just ended, or a STOP came in while the chamber was at peak, or
 * the element is stuck closed by a welded SSR contact and nothing in software
 * ever asked for heat. In every one of those the reading above
 * CONFIG_REFLOW_ABS_MAX_TEMP_C is the only warning available, and the guard in
 * controller.c is deliberately written as "any state except FAULT" rather than
 * "while running".
 *
 * So: no START anywhere in this test. The oven sits idle, the sample goes one
 * degree past the limit, and the fault has to latch anyway.
 *
 * The step from the fake's 25 degC idle is far larger than
 * REFLOW_SPIKE_MAX_STEP_MC, so the real filter in tempguard.c holds the
 * reported value still and the trip can only come from the raw sample -- the
 * same asymmetry the previous test relies on, which is why the filtered value
 * is asserted to still be below the limit here too.
 */
ZTEST(reflow_controller, test_absolute_cut_out_trips_with_the_oven_idle)
{
	struct reflow_telemetry t;

	zassert_ok(zbus_chan_read(&reflow_telemetry_chan, &t, K_MSEC(200)), NULL);
	zassert_equal(t.state, REFLOW_STATE_IDLE,
		      "this test only means something from idle, and the oven is %s",
		      reflow_state_str(t.state));

	reflow_temp_fake_set_raw_mc(ABS_MAX_MC + 1000);

	zassert_true(wait_for(is_fault, REFLOW_FAULT_OVERTEMP, &t),
		     "the oven read %d mC while idle and stayed in %s/%s: the "
		     "backstop only fires during a run",
		     ABS_MAX_MC + 1000, reflow_state_str(t.state),
		     reflow_fault_str(t.fault));
	zassert_true(t.temp_mc < ABS_MAX_MC,
		     "the filtered reading (%d mC) had already passed the limit, so "
		     "this says nothing about the raw path", t.temp_mc);
	zassert_false(reflow_heater_is_on(),
		      "element still energised after over-temperature while idle");

	/*
	 * And it is a latch, not a warning: the oven stays refused until an
	 * explicit clear, which is what the README promises for all four
	 * faults. The reading is left where it is on purpose -- a START
	 * accepted here would be a run beginning above the hard limit.
	 */
	post(REFLOW_CMD_START, 0);
	k_msleep(1000);

	zassert_ok(zbus_chan_read(&reflow_telemetry_chan, &t, K_MSEC(200)), NULL);
	zassert_equal(t.state, REFLOW_STATE_FAULT,
		      "START was accepted with the over-temperature fault latched "
		      "(state %s)", reflow_state_str(t.state));
	zassert_false(reflow_heater_is_on(), "element energised from a latched fault");
}

/* A cleared fault has to give the oven back, not brick it until reboot. */
ZTEST(reflow_controller, test_a_cleared_fault_allows_a_new_run)
{
	struct reflow_telemetry t;

	post(REFLOW_CMD_START, 0);
	zassert_true(wait_for(is_state, REFLOW_STATE_RUNNING, &t), "the run did not start");

	reflow_temp_fake_fail(-EIO);
	zassert_true(wait_for(is_fault, REFLOW_FAULT_SENSOR, &t), "no fault to clear");

	reflow_temp_fake_fail(0);
	post(REFLOW_CMD_CLEAR_FAULT, 0);
	zassert_true(wait_for(is_state, REFLOW_STATE_IDLE, &t), "the fault would not clear");
	zassert_true(wait_for(temp_valid_is, 1, &t), "no reading after clearing");

	post(REFLOW_CMD_START, 0);
	zassert_true(wait_for(is_state, REFLOW_STATE_RUNNING, &t),
		     "the oven refused to run after a cleared fault (state %s, fault %s)",
		     reflow_state_str(t.state), reflow_fault_str(t.fault));
}

/*
 * RFO-T10: a stage overrun has to become a latched FAULT_TIMEOUT with the
 * element cut, not just a return code.
 *
 * tests/logic already pins the profile machine's half of this
 * (test_grace_period_expiry_is_a_fault: reflow_run_tick() returns
 * REFLOW_RUN_ERR_TIMEOUT and keeps returning it). What had no coverage is
 * controller.c translating that into REFLOW_FAULT_TIMEOUT, dropping the duty and
 * refusing to run again until an explicit clear -- one of the four latches the
 * README promises.
 *
 * The sensor stays healthy and cold at the fake's 25 degC idle for the whole
 * run, so the "preheat" stage of profile 0 never reaches its 150 degC target and
 * overruns nominal_ms (90 s) plus grace_ms (60 s).
 *
 * That is 150 s of the oven's own clock, and it cannot be shortened from the test
 * side: run->stage_ms accumulates elapsed time, so no sample-period setting makes
 * it cheaper, and every other built-in profile's first heating stage is longer.
 * It costs far less than 150 s of real time, though: the simulated platforms
 * fast-forward their clock while every thread is sleeping, which is nearly all of
 * this test. Measured on qemu_x86, the whole suite is 39 s of wall clock with the
 * build included, so the suite's existing timeout still fits.
 */
ZTEST(reflow_controller, test_heating_stage_overrun_latches_fault_timeout)
{
	const struct reflow_profile *p = reflow_profile_get(0);
	const struct reflow_stage *st;
	struct reflow_telemetry t;
	int64_t started, took;

	zassert_not_null(p, "profile 0 does not exist");
	st = &p->stages[0];
	zassert_true(st->kind != REFLOW_STAGE_COOL,
		     "this test needs a HEATING first stage: a cooling one gets a "
		     "much larger grace budget and would measure something else");

	post(REFLOW_CMD_START, 0);
	zassert_true(wait_for(is_state, REFLOW_STATE_RUNNING, &t), "the run did not start");
	started = k_uptime_get();

	/*
	 * Budget: nominal + grace, plus SETTLE_MS of slack for the control
	 * period and the telemetry poll.
	 */
	zassert_true(wait_for_ms(is_fault, REFLOW_FAULT_TIMEOUT, &t,
				 (int64_t)st->nominal_ms + p->grace_ms + SETTLE_MS),
		     "a cold oven never reached FAULT_TIMEOUT: left in %s/%s",
		     reflow_state_str(t.state), reflow_fault_str(t.fault));

	took = k_uptime_get() - started;

	/*
	 * Not early, either. Without this the test would pass on a FAULT_TIMEOUT
	 * that arrived for the wrong reason -- and a timeout raised before the
	 * stage's own budget is spent is a firmware that gives up on an oven
	 * that is merely slow.
	 */
	zassert_true(took >= (int64_t)st->nominal_ms,
		     "FAULT_TIMEOUT came after %d ms, before the stage's nominal "
		     "%u ms was even spent", (int)took, st->nominal_ms);

	zassert_equal(t.duty_permille, 0, "duty still %u after a timeout fault",
		      t.duty_permille);
	zassert_false(reflow_heater_is_on(), "element still energised after a timeout fault");

	/* The latch: a healthy sensor is not enough, it takes an explicit clear. */
	post(REFLOW_CMD_START, 0);
	k_msleep(1000);
	zassert_ok(zbus_chan_read(&reflow_telemetry_chan, &t, K_MSEC(200)), NULL);
	zassert_equal(t.state, REFLOW_STATE_FAULT,
		      "START was accepted while FAULT_TIMEOUT was latched (state %s)",
		      reflow_state_str(t.state));
	zassert_false(reflow_heater_is_on(), "element energised by a refused START");

	post(REFLOW_CMD_CLEAR_FAULT, 0);
	zassert_true(wait_for(is_state, REFLOW_STATE_IDLE, &t), "the fault would not clear");

	post(REFLOW_CMD_START, 0);
	zassert_true(wait_for(is_state, REFLOW_STATE_RUNNING, &t),
		     "the oven refused to run after the timeout fault was cleared "
		     "(state %s, fault %s)",
		     reflow_state_str(t.state), reflow_fault_str(t.fault));
}

/*
 * Leave the oven stopped, so no test ends with a run still going. fresh_idle()
 * already normalises the state before each test, so today this changes nothing --
 * it exists so that adding a shared teardown later does not silently create a
 * dependency on test order.
 */
static void stop_the_oven(void *unused)
{
	ARG_UNUSED(unused);
	post(REFLOW_CMD_STOP, 0);
}

ZTEST_SUITE(reflow_controller, NULL, NULL, fresh_idle, stop_the_oven, NULL);
