/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Which profile the panel shows, as the encoder turns and telemetry arrives.
 * Pure, no Zephyr, tested in tests/logic/ - the shape RFO-B13 established for
 * ui/buttonmap.c, next to net/cmdparse.c and net/httpgate.c.
 *
 * Why it is separate from input_ui.c (RFO-B17): the index was a plain `static
 * uint8_t` written by two threads - the input callback and the telemetry
 * listener - AND the next index was derived from it. So a periodic frame
 * carrying a profile_idx the controller had not updated yet fed a stale value
 * back into the arithmetic, and the next detent reselected a profile the
 * operator had already passed. Inside a callback that is only reachable with
 * an image, a driver and real encoder events; here it is a state machine a
 * test walks end to end.
 */

#ifndef REFLOW_SELMAP_H_
#define REFLOW_SELMAP_H_

#include <stdbool.h>
#include <stdint.h>

/* No profile to select - reflow_sel_next() with no profiles at all. */
#define REFLOW_SEL_NONE (-1)

/*
 * How many disagreeing telemetry frames the panel tolerates before it gives
 * up on the command it posted and believes the oven again.
 *
 * It exists because SELECT_PROFILE CAN be refused: controller.c returns early
 * while RUNNING and for an index that does not resolve, and a refused command
 * never produces a frame carrying it. Without a bound, one refusal would leave
 * the panel ignoring telemetry until the next reset - trading this ticket's
 * defect for a worse one.
 *
 * Two frames, because a legitimate command is drained within one control
 * period (100 ms) while frames are 500 ms apart: one frame already in flight
 * when the command was posted is the normal case, two is slack, and three
 * means it is not coming.
 */
#define REFLOW_SEL_STALE_FRAMES 2

struct reflow_sel {
	/* What the panel shows. Always the UI's own value, never written
	 * straight from telemetry while a command is in flight. */
	uint8_t index;
	/* The index of the command in flight, valid only while `pending`. */
	uint8_t want;
	/* A SELECT_PROFILE was posted and accepted, and no frame has confirmed
	 * it yet. */
	bool pending;
	/* Frames that disagreed with `want` since it was posted. */
	uint8_t stale;
};

/*
 * Index 0 and nothing in flight. That is not an arbitrary zero: controller.c
 * also boots at profile_idx 0, so before the first frame the panel and the
 * oven agree by construction, and the first detent moves from the same place
 * the oven is.
 */
void reflow_sel_init(struct reflow_sel *s);

/*
 * The index `steps` detents away, over `count` profiles. Pure in the strict
 * sense - it does not touch `s` - so the caller can compute the index, try to
 * post the command, and only then commit.
 *
 * Returns REFLOW_SEL_NONE when there is nothing to select (count == 0).
 */
int reflow_sel_next(const struct reflow_sel *s, int32_t steps, uint8_t count);

/* A SELECT_PROFILE for `idx` was accepted by the command queue. */
void reflow_sel_commit(struct reflow_sel *s, uint8_t idx);

/* A telemetry frame arrived carrying `idx` as the oven's profile. */
void reflow_sel_telemetry(struct reflow_sel *s, uint8_t idx);

/* What the panel should show. */
uint8_t reflow_sel_index(const struct reflow_sel *s);

#endif /* REFLOW_SELMAP_H_ */
