/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Host-side closed-loop check of the control logic. Not part of the firmware
 * build: it links the same pid.c and profile.c against a first-order oven
 * model so the profile and the default gains can be exercised without
 * hardware.
 *
 *   cc -std=c99 -Wall -Wextra -O2 -o host_sim tools/host_sim.c \
 *      src/core/pid.c src/core/profile.c -Isrc/core -lm
 *   ./host_sim            # summary
 *   ./host_sim -v         # per-second trace
 */

#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "pid.h"
#include "profile.h"

#define DT_S      0.25
#define AMBIENT_C 25.0

/*
 * Oven model, overridable with KHEAT / KLOSS / TAU so a sluggish oven can be
 * checked too: full-power heating rate (degC/s), Newtonian loss coefficient
 * (1/s), and the thermocouple + thermal mass lag (s).
 */
static double k_heat = 4.5;
static double k_loss = 0.010;
static double tau_sense = 4.0;

/* Defaults mirror the Kconfig values; override with KP/KI/KD/ICLAMP/DALPHA. */
static struct pid_cfg cfg = {
	.kp = 60.0f, .ki = 2.0f, .kd = 250.0f,
	.out_min = 0.0f, .out_max = 1000.0f,
	.i_min = -1000.0f, .i_max = 1000.0f,
	.d_alpha = 0.2f,
};

static void load_gains(void)
{
	const char *e;

	if ((e = getenv("KP")) != NULL) {
		cfg.kp = (float)atof(e);
	}
	if ((e = getenv("KI")) != NULL) {
		cfg.ki = (float)atof(e);
	}
	if ((e = getenv("KD")) != NULL) {
		cfg.kd = (float)atof(e);
	}
	if ((e = getenv("ICLAMP")) != NULL) {
		cfg.i_max = (float)atof(e);
		cfg.i_min = -cfg.i_max;
	}
	if ((e = getenv("DALPHA")) != NULL) {
		cfg.d_alpha = (float)atof(e);
	}
	if ((e = getenv("KHEAT")) != NULL) {
		k_heat = atof(e);
	}
	if ((e = getenv("KLOSS")) != NULL) {
		k_loss = atof(e);
	}
	if ((e = getenv("TAU")) != NULL) {
		tau_sense = atof(e);
	}
}

struct result {
	const char *name;
	enum reflow_run_result res;
	double peak_c;
	double rms_err_c;
	double duration_s;
	double peak_sp_err_c;
};

static struct result simulate(const struct reflow_profile *prof, bool verbose)
{
	struct reflow_run run;
	struct pid_state pid;
	double oven = AMBIENT_C, sensed = AMBIENT_C;
	double sum_sq = 0.0, t = 0.0, peak = AMBIENT_C, peak_err = 0.0;
	long n = 0;
	struct result r = { .name = prof->name, .res = REFLOW_RUN_ACTIVE };
	uint8_t last_stage = 0xff;

	pid_reset(&pid);
	reflow_run_start(&run, prof, (int32_t)(sensed * 1000));

	/*
	 * The cut-off comes from the profile, not from a literal: a run may
	 * legally last nominal + grace for every stage, and a profile longer
	 * than a hardcoded ceiling could never finish (RFO-G12). This does not
	 * mask a profile that fails to converge - reflow_run_tick() returns
	 * REFLOW_RUN_ERR_TIMEOUT at or before this bound, so the run is still
	 * reported as a failure, just the honest one.
	 */
	const double limit_s = reflow_profile_max_ms(prof) / 1000.0;

	while (r.res == REFLOW_RUN_ACTIVE && t < limit_s) {
		double duty = 0.0;
		double sp;

		r.res = reflow_run_tick(&run, prof, (uint32_t)(DT_S * 1000),
					(int32_t)(sensed * 1000));
		if (r.res != REFLOW_RUN_ACTIVE) {
			break;
		}
		sp = run.setpoint_mc / 1000.0;

		if (reflow_run_heater_allowed(&run, prof)) {
			duty = pid_step(&cfg, &pid, (float)sp, (float)sensed,
					(float)DT_S, NULL);
			double err = sp - sensed;

			sum_sq += err * err;
			n++;
			if (fabs(err) > peak_err) {
				peak_err = fabs(err);
			}
		}

		/* Oven core, then the lagged sensor reading. */
		oven += (k_heat * duty / 1000.0 - k_loss * (oven - AMBIENT_C)) * DT_S;
		sensed += (oven - sensed) * (DT_S / tau_sense);
		if (sensed > peak) {
			peak = sensed;
		}
		t += DT_S;

		if (verbose && run.stage != last_stage) {
			printf("  t=%6.1fs  -> stage %u (%s)\n", t, run.stage,
			       prof->stages[run.stage].name);
			last_stage = run.stage;
		}
	}

	r.peak_c = peak;
	r.rms_err_c = n ? sqrt(sum_sq / (double)n) : 0.0;
	r.duration_s = t;
	r.peak_sp_err_c = peak_err;
	return r;
}

static const char *res_str(enum reflow_run_result r)
{
	switch (r) {
	case REFLOW_RUN_ACTIVE:        return "STILL RUNNING";
	case REFLOW_RUN_DONE:          return "done";
	case REFLOW_RUN_ERR_TIMEOUT:   return "TIMEOUT";
	case REFLOW_RUN_ERR_OVERTEMP:  return "OVERTEMP";
	default:                       return "?";
	}
}

int main(int argc, char **argv)
{
	bool verbose = (argc > 1 && strcmp(argv[1], "-v") == 0);
	int failures = 0;

	load_gains();
	printf("gains: kp=%.1f ki=%.2f kd=%.1f iclamp=%.0f dalpha=%.2f\n",
	       (double)cfg.kp, (double)cfg.ki, (double)cfg.kd,
	       (double)cfg.i_max, (double)cfg.d_alpha);

	printf("oven model: %.1f degC/s at full power, loss %.3f /s, sensor tau %.1f s\n\n",
	       k_heat, k_loss, tau_sense);

	for (uint8_t i = 0; i < reflow_profile_count(); i++) {
		const struct reflow_profile *prof = reflow_profile_get(i);
		struct result r;

		if (verbose) {
			printf("[%u] %s\n", i, prof->name);
		}
		r = simulate(prof, verbose);

		printf("%-22s %-13s peak %6.1f C  rms err %5.2f C  "
		       "max err %5.2f C  %5.0f s\n",
		       r.name, res_str(r.res), r.peak_c, r.rms_err_c,
		       r.peak_sp_err_c, r.duration_s);

		if (r.res != REFLOW_RUN_DONE) {
			printf("  FAIL: run did not complete\n");
			failures++;
		}
		if (r.peak_c * 1000.0 >= prof->abort_mc) {
			printf("  FAIL: peak crossed the abort level\n");
			failures++;
		}
		if (r.rms_err_c > 8.0) {
			printf("  FAIL: tracking error too large\n");
			failures++;
		}
	}

	printf("\n%s\n", failures ? "FAILURES PRESENT" : "all profiles tracked OK");
	return failures ? 1 : 0;
}
