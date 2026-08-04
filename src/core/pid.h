/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * PID controller. Pure C99, no Zephyr dependencies, so it can be unit
 * tested on the host (see tests/logic).
 */

#ifndef REFLOW_PID_H_
#define REFLOW_PID_H_

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>

struct pid_cfg {
	float kp;
	float ki;
	float kd;
	/* Output clamp, in the caller's unit (here: 0..1000 permille of duty). */
	float out_min;
	float out_max;
	/* Integral term clamp (anti-windup). */
	float i_min;
	float i_max;
	/* Low pass on the derivative path, 0..1. 1.0 = no filtering. */
	float d_alpha;
};

struct pid_state {
	float integral;
	float prev_meas;
	float d_filt;
	bool has_prev;
};

struct pid_terms {
	float p;
	float i;
	float d;
	float out;
};

void pid_reset(struct pid_state *st);

/*
 * One control step.
 *
 * sp, meas: setpoint and measurement, any consistent unit (here milli-degC).
 * dt_s:     elapsed time in seconds, must be > 0.
 * terms:    optional, filled with the individual contributions (may be NULL).
 *
 * The derivative acts on the measurement (not on the error) to avoid a kick
 * when the setpoint jumps. Integration is skipped while the output is
 * saturated in the direction that would make saturation worse (conditional
 * integration).
 */
float pid_step(const struct pid_cfg *cfg, struct pid_state *st, float sp,
	       float meas, float dt_s, struct pid_terms *terms);

#endif /* REFLOW_PID_H_ */
