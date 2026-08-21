/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Guard rails on the temperature reading path: the spike rejector, and the
 * absolute over-temperature backstop.
 *
 * Kept apart from temp.c and controller.c, and free of any Zephyr dependency,
 * for the same reason pid.c and profile.c are: these two decisions are what
 * stands between a rejected sample and an element left energised. They are
 * pure functions of the samples fed in, so the whole behaviour can be
 * exercised with nothing but integers.
 */

#ifndef REFLOW_TEMPGUARD_H_
#define REFLOW_TEMPGUARD_H_

#include <stdbool.h>
#include <stdint.h>

/*
 * A jump larger than this between consecutive samples is electrical noise and
 * not physics: the MAX6675 has 0.25 degC resolution and a ~220 ms conversion.
 */
#define REFLOW_SPIKE_MAX_STEP_MC 40000

/*
 * How many consecutive samples the filter may suppress before it has to give
 * up and believe the sensor. The filter must never be able to hold the
 * reported temperature still indefinitely: a real step larger than
 * REFLOW_SPIKE_MAX_STEP_MC is indistinguishable from noise on one sample, but
 * not on three.
 */
#define REFLOW_SPIKE_MAX_REJECTS 3

struct reflow_spike {
	int32_t last_mc;
	uint8_t rejects;
	bool have_last;
};

enum reflow_spike_result {
	/* The sample was plausible and is passed through unchanged. */
	REFLOW_SPIKE_ACCEPT = 0,
	/* The sample looked like a spike; the previous value is reported. */
	REFLOW_SPIKE_REJECT,
	/*
	 * The jump persisted for REFLOW_SPIKE_MAX_REJECTS samples, so it is
	 * not noise. The new value is accepted and reported.
	 */
	REFLOW_SPIKE_FORCED,
};

/* Forget history. Call before the first sample of a session. */
void reflow_spike_reset(struct reflow_spike *s);

/*
 * Feed one raw sample. *out_mc receives the value the control loop should use:
 * raw_mc on ACCEPT and FORCED, the previous sample on REJECT.
 *
 * out_mc must not alias raw_mc's storage in a way the caller depends on: the
 * function writes *out_mc before returning and reads nothing after.
 */
enum reflow_spike_result reflow_spike_filter(struct reflow_spike *s, int32_t raw_mc,
					     int32_t *out_mc);

/*
 * The absolute over-temperature backstop. Deliberately fed BOTH readings: it
 * has to trip on the raw sample, or the spike rejector above could suppress a
 * real rise and blind the very cut-out that exists to catch it. A backstop
 * that shares an input filter with the loop it backs up is not a backstop.
 */
bool reflow_overtemp_tripped(int32_t filtered_mc, int32_t raw_mc, int32_t limit_mc);

#endif /* REFLOW_TEMPGUARD_H_ */
