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

/*
 * Name buffer sizes. Fixed arrays, not pointers, and that is the whole point
 * of this header change: a struct with a pointer in it cannot be written to
 * flash. Persisting it as it was would store an ADDRESS, and the worst case is
 * not losing the name - it is serving a profile name read from arbitrary
 * memory on the web page after a reboot or a firmware update.
 *
 * Declared here rather than in Kconfig on purpose: profile.c is one of the
 * files that must build with no Zephyr at all (tests/logic/CMakeLists.txt and
 * tools/host_sim.c compile it as plain C99), so it never sees autoconf.h.
 * Same reason the absolute limit reaches reflow_profile_validate() as an
 * argument instead of as CONFIG_REFLOW_ABS_MAX_TEMP_C.
 *
 * The numbers come from the narrowest place a name is shown, which is the
 * 320x240 panel, measured rather than guessed: LVGL's Montserrat 14 (the
 * default font) has 158 glyphs with a mean advance of 10.06 px and a widest
 * advance of 17.5 px (parsed from the adv_w table in
 * lv_font_montserrat_14.c, which stores 1/16 px units).
 *
 *   - Profile name, 32: 31 visible characters at the mean advance is 312 px,
 *     so a typical name fills the 320 px line and nothing shorter wastes it.
 *     The longest built-in is "SAC305 lead-free" at 16, which leaves room for
 *     the editable names this slice exists to make possible.
 *   - Stage name, 12: the stage shares its line with the stage counter and the
 *     elapsed time, so it gets roughly a third of the width; 11 visible
 *     characters is 111 px at the mean advance. The longest built-in is
 *     "preheat" at 7.
 *
 * What does NOT bind these: the telemetry JSON. Its worst case with the sizes
 * above is 254 bytes against REFLOW_JSON_BUF_SZ of 384, and telemetry_json.h
 * carries a BUILD_ASSERT so that stays true if somebody grows a name.
 *
 * A name made entirely of the widest glyph still overflows the label - 18
 * characters is already 315 px. That is a display concern, not a data one:
 * lbl_profile has no long mode set today, so it clips rather than wraps. Left
 * alone deliberately; see the PR.
 */
#define REFLOW_PROFILE_NAME_LEN 32
#define REFLOW_STAGE_NAME_LEN   12

/*
 * How long a COOLING stage may overrun its nominal time before the run is
 * called a timeout. Deliberately separate from grace_ms, and two orders of
 * magnitude larger (RFO-B04).
 *
 * The two cases are not the same failure. A heating stage that overruns means
 * the oven is being driven and is not following the profile: the element is on,
 * the board is being cooked to an unknown temperature, and stopping quickly is
 * the whole point of grace_ms. A cooling stage that overruns means a passive
 * oven is cooling slower than the profile guessed. The heater has been off
 * since the stage began -- reflow_run_heater_allowed() forces it off, and the
 * absolute backstop still runs every tick -- so nothing is at risk, and the
 * old behaviour latched FAULT_TIMEOUT at the end of every otherwise perfect
 * run. Nominal 180 s plus 60 s of grace only closes if the oven's thermal time
 * constant is under ~112 s; a toaster oven with the door shut is 200-600 s.
 *
 * Why a bound at all instead of waiting forever: 30 minutes covers a passive
 * cool-down from 245 degC to 50 degC for a thermal time constant up to ~800 s,
 * which is past the far end of that range. Beyond it something is wrong that
 * the operator has to see -- a sensor reading stuck above the target, or an
 * element still delivering heat -- and a run that never ends would hide it.
 */
#define REFLOW_COOL_GRACE_MS 1800000U

enum reflow_stage_kind {
	/* Drive the setpoint linearly from the entry temperature to target. */
	REFLOW_STAGE_RAMP,
	/* Hold the setpoint at target. */
	REFLOW_STAGE_SOAK,
	/* Heater off, wait until the oven falls to target. */
	REFLOW_STAGE_COOL,
};

struct reflow_stage {
	char name[REFLOW_STAGE_NAME_LEN];
	int32_t target_mc;
	uint32_t nominal_ms;
	enum reflow_stage_kind kind;
};

struct reflow_profile {
	char name[REFLOW_PROFILE_NAME_LEN];
	uint8_t n_stages;
	struct reflow_stage stages[REFLOW_MAX_STAGES];
	/* Hard limit: crossing it aborts the run, whatever the stage. */
	int32_t abort_mc;
	/* How close to target counts as "reached". */
	int32_t tol_mc;
	/*
	 * Extra time a HEATING stage may take beyond nominal_ms before it is a
	 * fault. A cooling stage gets REFLOW_COOL_GRACE_MS instead; see below.
	 */
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
 *
 * Two readings, for the same reason reflow_overtemp_tripped() takes two
 * (RFO-B34). temp_mc is the spike-filtered value and drives the setpoint and
 * the stage machine; raw_mc is the sample as it came off the chip, and the
 * profile abort level is judged against both. With only the filtered one there
 * is a window - up to REFLOW_SPIKE_MAX_REJECTS - 1 samples - in which a real
 * rise past prof->abort_mc but below the absolute cut-out is seen by neither
 * barrier: the profile one because it reads a suppressed value, the absolute
 * one because the temperature is genuinely below it.
 *
 * A caller with no spike filter passes the same value twice; that is the
 * pre-RFO-B34 behaviour and it stays correct.
 */
enum reflow_run_result reflow_run_tick(struct reflow_run *run,
				       const struct reflow_profile *prof,
				       uint32_t dt_ms, int32_t temp_mc, int32_t raw_mc);

/* True when the heater must be forced off for the current stage. */
bool reflow_run_heater_allowed(const struct reflow_run *run,
			       const struct reflow_profile *prof);

/* Sum of nominal stage durations, for progress reporting. */
uint32_t reflow_profile_nominal_ms(const struct reflow_profile *prof);

/*
 * Longest a run of this profile can legally last: nominal plus the grace period
 * of every stage. Past this the state machine has already declared
 * REFLOW_RUN_ERR_TIMEOUT, so it is the right cut-off for a simulation loop -
 * and the wrong thing to replace with a literal.
 */
uint32_t reflow_profile_max_ms(const struct reflow_profile *prof);

/*
 * Why a profile has to be checked at all, now that it can come from somewhere
 * other than the table below: every field here is read by the state machine
 * that decides whether the element is energised. A profile is not data, it is
 * a set of limits.
 */
enum reflow_profile_valid {
	REFLOW_PROFILE_OK = 0,
	/* No profile at all. */
	REFLOW_PROFILE_ERR_NULL,
	/*
	 * The name fills its buffer with no NUL in it. Every reader treats it
	 * as a C string - LOG_INF("%s"), snprintk, lv_label_set_text - so an
	 * unterminated name is a read past the struct, which is exactly the
	 * failure this slice exists to make impossible.
	 */
	REFLOW_PROFILE_ERR_NAME,
	/* Zero stages, or more than the array holds. */
	REFLOW_PROFILE_ERR_STAGES,
	/* Same NUL rule as the profile name, for one of the stages. */
	REFLOW_PROFILE_ERR_STAGE_NAME,
	/*
	 * A stage of zero nominal time. The runner would hand it a deadline
	 * already past, so the stage can only end as a timeout.
	 */
	REFLOW_PROFILE_ERR_STAGE_TIME,
	/*
	 * Tolerance at or below zero: nothing ever counts as having reached
	 * target, because the comparison is against an exact value.
	 */
	REFLOW_PROFILE_ERR_TOL,
	/*
	 * The abort level is at or below the hottest the profile asks for. The
	 * profile would abort itself by following its own plan.
	 */
	REFLOW_PROFILE_ERR_ABORT_LOW,
	/*
	 * The abort level is above the firmware's absolute cut-out. A profile
	 * cannot raise the hard limit by asking; it would be an abort level
	 * the oven can never reach, and a silently disabled barrier.
	 */
	REFLOW_PROFILE_ERR_ABORT_HIGH,
};

/*
 * Check a profile against its own coherence and against the firmware limit.
 *
 * abs_max_mc is CONFIG_REFLOW_ABS_MAX_TEMP_C in milli-degrees, passed in
 * because this file has no Zephyr. Pass INT32_MAX to check coherence only;
 * callers that can energise the element must not.
 */
enum reflow_profile_valid reflow_profile_validate(const struct reflow_profile *prof,
						  int32_t abs_max_mc);

/* Human-readable form of the above, for logs and shell output. */
const char *reflow_profile_valid_str(enum reflow_profile_valid v);

/* Built-in profile table. */
uint8_t reflow_profile_count(void);
const struct reflow_profile *reflow_profile_get(uint8_t idx);

#endif /* REFLOW_PROFILE_H_ */
