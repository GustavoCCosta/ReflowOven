/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * RFO-B17. Three detents in ~30 ms posted three SELECT_PROFILE commands. Before
 * the controller drained them, the periodic publish carried the profile_idx it
 * still had, the telemetry listener wrote that into the same `selected` the
 * input callback reads, and the next detent counted from a value the operator
 * had already moved past. The selection stuck, or walked backwards, and the
 * operator could start the wrong profile.
 *
 * The fix is not a lock. Serialising the two writers would make the read
 * well-defined and keep the defect: the stale value would still be fed back
 * into the arithmetic, just without a race. What changes here is WHERE the next
 * index comes from - the panel's own value - and WHEN telemetry is allowed to
 * replace it.
 */

#include "selmap.h"

void reflow_sel_init(struct reflow_sel *s)
{
	s->index = 0U;
	s->want = 0U;
	s->pending = false;
	s->stale = 0U;
}

int reflow_sel_next(const struct reflow_sel *s, int32_t steps, uint8_t count)
{
	int64_t next;

	if (count == 0U) {
		return REFLOW_SEL_NONE;
	}

	/*
	 * In 64 bits on purpose. `steps` is whatever the input subsystem
	 * reports, and int32_t arithmetic on INT32_MIN overflows - undefined
	 * behaviour in the one place whose whole job is to be deterministic.
	 * The result is reduced before it comes back to 32 bits.
	 */
	next = ((int64_t)s->index + (int64_t)steps) % (int64_t)count;
	if (next < 0) {
		next += count;
	}

	return (int)next;
}

void reflow_sel_commit(struct reflow_sel *s, uint8_t idx)
{
	s->index = idx;
	s->want = idx;
	s->pending = true;
	s->stale = 0U;
}

void reflow_sel_telemetry(struct reflow_sel *s, uint8_t idx)
{
	if (!s->pending) {
		/*
		 * Nothing in flight, so the oven is the authority. This is the
		 * path that carries a selection made somewhere else - the HTTP
		 * route, the shell - onto the panel, and dropping it would make
		 * the panel lie about which profile would run.
		 */
		s->index = idx;
		return;
	}

	if (idx == s->want) {
		/* The oven caught up. Back to believing telemetry. */
		s->index = idx;
		s->want = idx;
		s->pending = false;
		s->stale = 0U;
		return;
	}

	/*
	 * The frame disagrees with what was posted. This is the defect's own
	 * frame: it is carrying the index from BEFORE the command, and writing
	 * it into the panel is what made the next detent count from a profile
	 * the operator had already passed. So it is ignored - but not for ever.
	 *
	 * SELECT_PROFILE can be refused (RUNNING, or an index that does not
	 * resolve), and a refused command never produces a confirming frame.
	 * After REFLOW_SEL_STALE_FRAMES the panel accepts that the command is
	 * not coming back and believes the oven again, because a panel showing
	 * a profile the oven does not have is the worse of the two failures:
	 * the operator would press start on it.
	 */
	s->stale++;
	if (s->stale >= REFLOW_SEL_STALE_FRAMES) {
		s->index = idx;
		s->want = idx;
		s->pending = false;
		s->stale = 0U;
	}
}

uint8_t reflow_sel_index(const struct reflow_sel *s)
{
	return s->index;
}
