/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Unit tests for the two pieces of logic that decide whether a board gets
 * soldered or cooked: the PID and the profile state machine.
 * Run with:  west twister -T tests -p native_sim
 */

#include <zephyr/ztest.h>

#include "pid.h"
#include "profile.h"

/* ------------------------------------------------------------------ PID */

static const struct pid_cfg cfg = {
	.kp = 40.0f,
	.ki = 0.5f,
	.kd = 200.0f,
	.out_min = 0.0f,
	.out_max = 1000.0f,
	.i_min = -500.0f,
	.i_max = 500.0f,
	.d_alpha = 0.2f,
};

ZTEST(reflow_pid, test_proportional_only)
{
	struct pid_cfg p = cfg;
	struct pid_state st;
	float out;

	p.ki = 0.0f;
	p.kd = 0.0f;
	pid_reset(&st);

	out = pid_step(&p, &st, 100.0f, 90.0f, 0.25f, NULL);
	zassert_within(out, 400.0f, 0.01f, "expected kp*err = 400, got %f",
		       (double)out);
}

ZTEST(reflow_pid, test_output_is_clamped)
{
	struct pid_state st;
	float hot, cold;

	pid_reset(&st);
	cold = pid_step(&cfg, &st, 250.0f, 25.0f, 0.25f, NULL);
	zassert_equal(cold, 1000.0f, "large positive error must saturate high");

	pid_reset(&st);
	hot = pid_step(&cfg, &st, 25.0f, 250.0f, 0.25f, NULL);
	zassert_equal(hot, 0.0f, "negative error must saturate low");
}

ZTEST(reflow_pid, test_integral_does_not_wind_up)
{
	struct pid_state st;
	struct pid_terms terms;

	pid_reset(&st);

	/* 200 s of a huge error: the output is pinned at max the whole time. */
	for (int i = 0; i < 800; i++) {
		(void)pid_step(&cfg, &st, 250.0f, 25.0f, 0.25f, &terms);
	}
	zassert_true(terms.i <= cfg.i_max,
		     "integral %f exceeded the clamp", (double)terms.i);

	/*
	 * Now the measurement overshoots. With anti-windup the controller must
	 * back off within a couple of steps, not stay stuck at full power.
	 */
	for (int i = 0; i < 4; i++) {
		(void)pid_step(&cfg, &st, 250.0f, 265.0f, 0.25f, &terms);
	}
	zassert_true(terms.out < 1000.0f,
		     "controller stayed saturated after overshoot (out=%f)",
		     (double)terms.out);
}

ZTEST(reflow_pid, test_no_derivative_kick_on_setpoint_step)
{
	struct pid_cfg p = cfg;
	struct pid_state st;
	float a, b;

	p.ki = 0.0f;
	pid_reset(&st);

	/* Steady measurement, setpoint jumps: D must contribute nothing. */
	(void)pid_step(&p, &st, 30.0f, 30.0f, 0.25f, NULL);
	a = pid_step(&p, &st, 30.0f, 30.0f, 0.25f, NULL);
	b = pid_step(&p, &st, 230.0f, 30.0f, 0.25f, NULL);

	zassert_within(a, 0.0f, 0.01f, "no error, no output");
	zassert_equal(b, 1000.0f, "step should go to full power, got %f",
		      (double)b);
}

ZTEST_SUITE(reflow_pid, NULL, NULL, NULL, NULL, NULL);

/* -------------------------------------------------------------- profile */

static const struct reflow_profile test_prof = {
	.name = "test",
	.n_stages = 3,
	.stages = {
		{ "ramp", 100000, 10000, REFLOW_STAGE_RAMP },
		{ "hold", 100000, 10000, REFLOW_STAGE_SOAK },
		{ "cool",  50000, 20000, REFLOW_STAGE_COOL },
	},
	.abort_mc = 150000,
	.tol_mc = 2000,
	.grace_ms = 5000,
};

ZTEST(reflow_profile, test_builtin_table_is_sane)
{
	zassert_true(reflow_profile_count() > 0, "no built-in profiles");
	zassert_is_null(reflow_profile_get(reflow_profile_count()),
			"out of range index must return NULL");

	for (uint8_t i = 0; i < reflow_profile_count(); i++) {
		const struct reflow_profile *p = reflow_profile_get(i);

		zassert_true(p->n_stages > 0 && p->n_stages <= REFLOW_MAX_STAGES,
			     "profile %u has %u stages", i, p->n_stages);
		zassert_equal(p->stages[p->n_stages - 1].kind, REFLOW_STAGE_COOL,
			      "profile %u must end on a cooling stage", i);
		for (uint8_t s = 0; s < p->n_stages; s++) {
			zassert_true(p->stages[s].target_mc < p->abort_mc,
				     "profile %u stage %u targets at or above the abort level",
				     i, s);
		}
	}
}

ZTEST(reflow_profile, test_ramp_setpoint_is_interpolated)
{
	struct reflow_run run;

	reflow_run_start(&run, &test_prof, 20000);
	zassert_equal(run.setpoint_mc, 20000, "ramp must start at the entry temp");

	/* Half the ramp: setpoint halfway between 20 and 100 degC. */
	(void)reflow_run_tick(&run, &test_prof, 5000, 20000);
	zassert_within(run.setpoint_mc, 60000, 500,
		       "expected ~60000 mC, got %d", run.setpoint_mc);

	/* Setpoint never overshoots the stage target. */
	(void)reflow_run_tick(&run, &test_prof, 4000, 20000);
	zassert_true(run.setpoint_mc <= 100000, "setpoint %d above target",
		     run.setpoint_mc);
}

ZTEST(reflow_profile, test_stage_advances_only_when_hot_enough)
{
	struct reflow_run run;

	reflow_run_start(&run, &test_prof, 20000);

	/* Time is up but the oven is cold: stay in stage 0. */
	(void)reflow_run_tick(&run, &test_prof, 10000, 20000);
	zassert_equal(run.stage, 0, "advanced with the oven still cold");

	/* Target reached: move on, and the new stage starts from here. */
	(void)reflow_run_tick(&run, &test_prof, 100, 99000);
	zassert_equal(run.stage, 1, "should have entered the soak");
	zassert_equal(run.stage_ms, 0, "stage timer must restart");
	zassert_equal(run.stage_start_mc, 99000, "entry temperature not recorded");
}

ZTEST(reflow_profile, test_grace_period_expiry_is_a_fault)
{
	struct reflow_run run;
	enum reflow_run_result res = REFLOW_RUN_ACTIVE;

	reflow_run_start(&run, &test_prof, 20000);

	/* Oven never heats: nominal 10 s + 5 s grace, then fault. */
	for (int i = 0; i < 200 && res == REFLOW_RUN_ACTIVE; i++) {
		res = reflow_run_tick(&run, &test_prof, 250, 21000);
	}
	zassert_equal(res, REFLOW_RUN_ERR_TIMEOUT, "expected a timeout fault");
	zassert_equal(reflow_run_tick(&run, &test_prof, 250, 21000),
		      REFLOW_RUN_ERR_TIMEOUT, "fault must latch");
}

ZTEST(reflow_profile, test_overtemp_aborts)
{
	struct reflow_run run;

	reflow_run_start(&run, &test_prof, 20000);
	zassert_equal(reflow_run_tick(&run, &test_prof, 100, 151000),
		      REFLOW_RUN_ERR_OVERTEMP, "abort level not enforced");
}

ZTEST(reflow_profile, test_heater_is_off_while_cooling)
{
	struct reflow_run run;

	reflow_run_start(&run, &test_prof, 20000);
	zassert_true(reflow_run_heater_allowed(&run, &test_prof),
		     "heater should be allowed on the ramp");

	run.stage = 2; /* cooling stage */
	zassert_false(reflow_run_heater_allowed(&run, &test_prof),
		      "heater must be forced off while cooling");
}

ZTEST(reflow_profile, test_full_run_completes)
{
	struct reflow_run run;
	int32_t temp = 20000;
	enum reflow_run_result res = REFLOW_RUN_ACTIVE;

	reflow_run_start(&run, &test_prof, temp);

	for (int i = 0; i < 2000 && res == REFLOW_RUN_ACTIVE; i++) {
		/* Crude oven: follows the setpoint, cools when the heater is off. */
		if (reflow_run_heater_allowed(&run, &test_prof)) {
			temp += (run.setpoint_mc - temp) / 5;
		} else {
			temp -= 1500;
		}
		res = reflow_run_tick(&run, &test_prof, 250, temp);
	}

	zassert_equal(res, REFLOW_RUN_DONE, "run did not finish (res=%d, stage=%u)",
		      res, run.stage);
	zassert_true(temp <= 50000, "finished above the cool-down target: %d", temp);
}

ZTEST(reflow_profile, test_nominal_duration)
{
	zassert_equal(reflow_profile_nominal_ms(&test_prof), 40000,
		      "nominal duration mismatch");
}

ZTEST_SUITE(reflow_profile, NULL, NULL, NULL, NULL, NULL);
