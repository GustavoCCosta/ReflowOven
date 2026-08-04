/* SPDX-License-Identifier: Apache-2.0 */

#include "pid.h"

static float clampf(float v, float lo, float hi)
{
	if (v < lo) {
		return lo;
	}
	if (v > hi) {
		return hi;
	}
	return v;
}

void pid_reset(struct pid_state *st)
{
	st->integral = 0.0f;
	st->prev_meas = 0.0f;
	st->d_filt = 0.0f;
	st->has_prev = false;
}

float pid_step(const struct pid_cfg *cfg, struct pid_state *st, float sp,
	       float meas, float dt_s, struct pid_terms *terms)
{
	float err = sp - meas;
	float p, i, d, raw, out;

	if (dt_s <= 0.0f) {
		dt_s = 1e-3f;
	}

	p = cfg->kp * err;

	/* Derivative on measurement: d(err)/dt == -d(meas)/dt for constant sp. */
	if (st->has_prev) {
		float dmeas = (meas - st->prev_meas) / dt_s;
		float alpha = clampf(cfg->d_alpha, 0.0f, 1.0f);

		st->d_filt = st->d_filt + alpha * (dmeas - st->d_filt);
	} else {
		st->d_filt = 0.0f;
		st->has_prev = true;
	}
	st->prev_meas = meas;
	d = -cfg->kd * st->d_filt;

	/* Tentative integration, then conditional rollback (anti-windup). */
	i = clampf(st->integral + cfg->ki * err * dt_s, cfg->i_min, cfg->i_max);
	raw = p + i + d;
	out = clampf(raw, cfg->out_min, cfg->out_max);

	if (raw != out && ((raw > out && err > 0.0f) || (raw < out && err < 0.0f))) {
		/* Saturated and the error would push it further out: hold. */
		i = clampf(st->integral, cfg->i_min, cfg->i_max);
		raw = p + i + d;
		out = clampf(raw, cfg->out_min, cfg->out_max);
	}
	st->integral = i;

	if (terms != NULL) {
		terms->p = p;
		terms->i = i;
		terms->d = d;
		terms->out = out;
	}

	return out;
}
