/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Reflow profile description and profile runner (stage state machine).
 * Pure C99, no Zephyr dependencies, unit tested on the host.
 *
 * All temperatures are in milli-degrees Celsius (mC) to stay integer-only.
 */

#ifndef REFLOW_PROFILE_H_
#define REFLOW_PROFILE_H_

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>

#define REFLOW_MAX_STAGES 8

enum reflow_stage_kind {
	/* Drive the setpoint linearly from the entry temperature to target. */
	REFLOW_STAGE_RAMP,
	/* Hold the setpoint at target. */
	REFLOW_STAGE_SOAK,
	/* Heater off, wait until the oven falls to target. */
	REFLOW_STAGE_COOL,
};

struct reflow_stage {
	const char *name;
	int32_t target_mc;
	uint32_t nominal_ms;
	enum reflow_stage_kind kind;
};

struct reflow_profile {
	const char *name;
	uint8_t n_stages;
	struct reflow_stage stages[REFLOW_MAX_STAGES];
	/* Hard limit: crossing it aborts the run, whatever the stage. */
	int32_t abort_mc;
	/* How close to target counts as "reached". */
	int32_t tol_mc;
	/* Extra time a stage may take beyond nominal_ms before it is a fault. */
	uint32_t grace_ms;
};

enum reflow_run_result {
	REFLOW_RUN_ACTIVE = 0,
	REFLOW_RUN_DONE,
	REFLOW_RUN_ERR_TIMEOUT,
	REFLOW_RUN_ERR_OVERTEMP,
};

struct reflow_run {
	uint8_t stage;
	uint32_t stage_ms;
	uint32_t total_ms;
	int32_t stage_start_mc;
	int32_t setpoint_mc;
	enum reflow_run_result result;
};

/* Reset the runner to the first stage, entering at temp_mc. */
void reflow_run_start(struct reflow_run *run, const struct reflow_profile *prof,
		      int32_t temp_mc);

/*
 * Advance the run by dt_ms using the latest measurement, recomputing
 * run->setpoint_mc. Returns run->result; once it is not ACTIVE the runner
 * latches and further calls are no-ops.
 */
enum reflow_run_result reflow_run_tick(struct reflow_run *run,
				       const struct reflow_profile *prof,
				       uint32_t dt_ms, int32_t temp_mc);

/* True when the heater must be forced off for the current stage. */
bool reflow_run_heater_allowed(const struct reflow_run *run,
			       const struct reflow_profile *prof);

/* Sum of nominal stage durations, for progress reporting. */
uint32_t reflow_profile_nominal_ms(const struct reflow_profile *prof);

/* Built-in profile table. */
uint8_t reflow_profile_count(void);
const struct reflow_profile *reflow_profile_get(uint8_t idx);

#endif /* REFLOW_PROFILE_H_ */
