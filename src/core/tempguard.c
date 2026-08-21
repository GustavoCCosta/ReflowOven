/* SPDX-License-Identifier: Apache-2.0 */

#include "tempguard.h"

void reflow_spike_reset(struct reflow_spike *s)
{
	s->last_mc = 0;
	s->rejects = 0;
	s->have_last = false;
}

enum reflow_spike_result reflow_spike_filter(struct reflow_spike *s, int32_t raw_mc,
					     int32_t *out_mc)
{
	bool jumped;

	if (!s->have_last) {
		s->last_mc = raw_mc;
		s->have_last = true;
		s->rejects = 0;
		*out_mc = raw_mc;
		return REFLOW_SPIKE_ACCEPT;
	}

	/*
	 * Subtract in both directions instead of taking an absolute value:
	 * abs(INT32_MIN) is undefined, and this form has no such edge.
	 *
	 * It is NOT overflow-proof for arbitrary input — raw_mc - s->last_mc
	 * overflows for far-apart extremes, which is undefined behaviour. What
	 * makes it safe here is the caller: temp.c rejects anything outside
	 * -20..400 degC as FAULT_SENSOR before this function is reached, so the
	 * operands are always within 420000 of each other. A caller that drops
	 * that window has to clamp, or bring its own wider type.
	 */
	jumped = (raw_mc - s->last_mc > REFLOW_SPIKE_MAX_STEP_MC) ||
		 (s->last_mc - raw_mc > REFLOW_SPIKE_MAX_STEP_MC);

	if (!jumped) {
		s->rejects = 0;
		s->last_mc = raw_mc;
		*out_mc = raw_mc;
		return REFLOW_SPIKE_ACCEPT;
	}

	/*
	 * A jump this large is noise on one sample and physics on three. The
	 * counter is what stops the filter from suppressing a real step for
	 * ever: without it, the pre-patch code fed the rejected value back into
	 * last_mc and every following sample was measured against a reading
	 * that never moved again (RFO-B05).
	 */
	/* The MAX_REJECTS'th consecutive jump is believed; the ones before it are
	 * suppressed. Counting this way is what keeps the suppression window at
	 * MAX_REJECTS - 1 samples rather than MAX_REJECTS.
	 */
	if (s->rejects + 1 < REFLOW_SPIKE_MAX_REJECTS) {
		s->rejects++;
		/*
		 * last_mc is deliberately NOT updated here. It still holds the
		 * last believed sample, so the next comparison is against
		 * reality and not against a value this filter invented.
		 */
		*out_mc = s->last_mc;
		return REFLOW_SPIKE_REJECT;
	}

	s->rejects = 0;
	s->last_mc = raw_mc;
	*out_mc = raw_mc;
	return REFLOW_SPIKE_FORCED;
}

bool reflow_overtemp_tripped(int32_t filtered_mc, int32_t raw_mc, int32_t limit_mc)
{
	/*
	 * Either reading is enough. raw_mc is the one that matters while the
	 * spike rejector is suppressing a rise; filtered_mc still counts,
	 * because a reading latched high is not a reason to re-energise the
	 * element. The cost of this pair is that a single noise spike above the
	 * limit now latches FAULT_OVERTEMP instead of being filtered away —
	 * a stop that needs an explicit clear, which is the safe direction to
	 * be wrong in.
	 */
	return (raw_mc >= limit_mc) || (filtered_mc >= limit_mc);
}
