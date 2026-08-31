/* SPDX-License-Identifier: Apache-2.0 */

#include "profile.h"

static const struct reflow_profile builtin[] = {
	{
		.name = "SAC305 lead-free",
		.n_stages = 5,
		.stages = {
			{ "preheat", 150000,  90000, REFLOW_STAGE_RAMP },
			{ "soak",    180000,  90000, REFLOW_STAGE_RAMP },
			{ "ramp",    245000,  60000, REFLOW_STAGE_RAMP },
			{ "peak",    245000,  30000, REFLOW_STAGE_SOAK },
			{ "cool",     50000, 180000, REFLOW_STAGE_COOL },
		},
		.abort_mc = 260000,
		.tol_mc = 5000,
		.grace_ms = 60000,
	},
	{
		.name = "Sn63Pb37 leaded",
		.n_stages = 5,
		.stages = {
			{ "preheat", 140000,  90000, REFLOW_STAGE_RAMP },
			{ "soak",    160000,  80000, REFLOW_STAGE_RAMP },
			{ "ramp",    215000,  50000, REFLOW_STAGE_RAMP },
			{ "peak",    215000,  25000, REFLOW_STAGE_SOAK },
			{ "cool",     50000, 180000, REFLOW_STAGE_COOL },
		},
		.abort_mc = 235000,
		.tol_mc = 5000,
		.grace_ms = 60000,
	},
	{
		.name = "bake / dry 120C",
		.n_stages = 3,
		.stages = {
			{ "ramp",  120000,  120000, REFLOW_STAGE_RAMP },
			{ "hold",  120000, 1800000, REFLOW_STAGE_SOAK },
			{ "cool",   50000,  300000, REFLOW_STAGE_COOL },
		},
		.abort_mc = 140000,
		.tol_mc = 3000,
		.grace_ms = 120000,
	},
	{
		.name = "Bake / Dry 200C",
		.n_stages = 4,
		.stages = {
			{ "preheat", 150000,  200000, REFLOW_STAGE_RAMP },
			{ "ramp",  200000,  400000, REFLOW_STAGE_RAMP },
			{ "hold",  200000, 3000000, REFLOW_STAGE_SOAK },
			{ "cool",     50000, 180000, REFLOW_STAGE_COOL },
		},
		.abort_mc = 250000,
		.tol_mc = 3000,
		.grace_ms = 120000,
	},
};

uint8_t reflow_profile_count(void)
{
	return (uint8_t)(sizeof(builtin) / sizeof(builtin[0]));
}

const struct reflow_profile *reflow_profile_get(uint8_t idx)
{
	if (idx >= reflow_profile_count()) {
		return NULL;
	}
	return &builtin[idx];
}

uint32_t reflow_profile_nominal_ms(const struct reflow_profile *prof)
{
	uint32_t total = 0;

	for (uint8_t i = 0; i < prof->n_stages; i++) {
		total += prof->stages[i].nominal_ms;
	}
	return total;
}

/*
 * The overrun a stage is allowed before reflow_run_tick() calls it a timeout.
 * Cooling is passive with the element off, so it gets its own, much larger
 * budget; see REFLOW_COOL_GRACE_MS in profile.h for why the two differ.
 */
static uint32_t stage_grace_ms(const struct reflow_stage *st,
			       const struct reflow_profile *prof)
{
	return st->kind == REFLOW_STAGE_COOL ? REFLOW_COOL_GRACE_MS : prof->grace_ms;
}

uint32_t reflow_profile_max_ms(const struct reflow_profile *prof)
{
	/*
	 * The longest a run can legally last: every stage takes its nominal
	 * time plus the whole grace period before reflow_run_tick() calls it a
	 * REFLOW_RUN_ERR_TIMEOUT. grace_ms is per profile, not per stage, so it
	 * counts once per stage -- except for a cooling stage, which is allowed
	 * REFLOW_COOL_GRACE_MS instead (RFO-B04). Reading the budget from the
	 * same helper reflow_run_tick() uses is what keeps this a bound rather
	 * than a second opinion.
	 *
	 * This is the bound a simulation or a bench harness should use as its
	 * cut-off, instead of a literal (RFO-G12: host_sim had 3600 s hardcoded
	 * and could never finish the 3780 s bake profile). Deriving it this way
	 * cannot hide a profile that fails to converge: the state machine
	 * declares the timeout itself at or before this point, so the caller
	 * still sees a failure rather than a completed run.
	 */
	uint32_t total = reflow_profile_nominal_ms(prof);

	for (uint8_t i = 0; i < prof->n_stages; i++) {
		total += stage_grace_ms(&prof->stages[i], prof);
	}
	return total;
}

bool reflow_run_heater_allowed(const struct reflow_run *run,
			       const struct reflow_profile *prof)
{
	if (run->result != REFLOW_RUN_ACTIVE || run->stage >= prof->n_stages) {
		return false;
	}
	return prof->stages[run->stage].kind != REFLOW_STAGE_COOL;
}

static int32_t stage_setpoint(const struct reflow_run *run,
			      const struct reflow_stage *st)
{
	int64_t span, elapsed, delta;

	switch (st->kind) {
	case REFLOW_STAGE_RAMP:
		if (st->nominal_ms == 0U) {
			return st->target_mc;
		}
		elapsed = run->stage_ms > st->nominal_ms ? st->nominal_ms : run->stage_ms;
		span = (int64_t)st->target_mc - (int64_t)run->stage_start_mc;
		delta = (span * elapsed) / (int64_t)st->nominal_ms;
		return (int32_t)((int64_t)run->stage_start_mc + delta);
	case REFLOW_STAGE_SOAK:
	case REFLOW_STAGE_COOL:
	default:
		return st->target_mc;
	}
}

void reflow_run_start(struct reflow_run *run, const struct reflow_profile *prof,
		      int32_t temp_mc)
{
	run->stage = 0;
	run->stage_ms = 0;
	run->total_ms = 0;
	run->stage_start_mc = temp_mc;
	run->result = REFLOW_RUN_ACTIVE;
	run->setpoint_mc = prof->n_stages > 0 ?
		stage_setpoint(run, &prof->stages[0]) : temp_mc;
}

static bool stage_complete(const struct reflow_stage *st,
			   const struct reflow_profile *prof,
			   const struct reflow_run *run, int32_t temp_mc)
{
	if (run->stage_ms < st->nominal_ms) {
		return false;
	}

	/* Time is up; also require the thermal goal to be met. */
	if (st->kind == REFLOW_STAGE_COOL) {
		return temp_mc <= st->target_mc;
	}
	return temp_mc >= st->target_mc - prof->tol_mc;
}

enum reflow_run_result reflow_run_tick(struct reflow_run *run,
				       const struct reflow_profile *prof,
				       uint32_t dt_ms, int32_t temp_mc)
{
	const struct reflow_stage *st;

	if (run->result != REFLOW_RUN_ACTIVE) {
		return run->result;
	}

	if (prof->n_stages == 0U || run->stage >= prof->n_stages) {
		run->result = REFLOW_RUN_DONE;
		return run->result;
	}

	if (temp_mc >= prof->abort_mc) {
		run->result = REFLOW_RUN_ERR_OVERTEMP;
		return run->result;
	}

	run->stage_ms += dt_ms;
	run->total_ms += dt_ms;
	st = &prof->stages[run->stage];

	if (stage_complete(st, prof, run, temp_mc)) {
		run->stage++;
		if (run->stage >= prof->n_stages) {
			run->result = REFLOW_RUN_DONE;
			run->setpoint_mc = temp_mc;
			return run->result;
		}
		run->stage_ms = 0;
		run->stage_start_mc = temp_mc;
		st = &prof->stages[run->stage];
	} else if (run->stage_ms > st->nominal_ms + stage_grace_ms(st, prof)) {
		/* Oven cannot follow the profile: stop instead of cooking on. */
		run->result = REFLOW_RUN_ERR_TIMEOUT;
		return run->result;
	}

	run->setpoint_mc = stage_setpoint(run, st);
	return run->result;
}
