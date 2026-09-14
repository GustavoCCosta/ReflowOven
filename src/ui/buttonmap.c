/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * RFO-B13. A long press was an unconditional CLEAR_FAULT, and handle_cmd()
 * discards CLEAR_FAULT outside FAULT with no log and no change on screen. So
 * the operator who held the button during a run - meaning STOP - watched the
 * oven keep heating with nothing telling them it had not been obeyed. Third
 * ticket of that family (RFO-B19, RFO-B41), and this one is in the only control
 * that exists when there is no network.
 */

#include "buttonmap.h"

#include "../core/app.h"

int reflow_button_decide(bool state_known, uint8_t state, bool long_press)
{
	/*
	 * State not known yet - before the first telemetry frame - may only
	 * produce the command that cannot energise anything. CLEAR_FAULT here
	 * is the opposite of failing safe: it is the action that REARMS the
	 * oven, taken by default over a state nobody has read. START is no
	 * better: if the oven is idle, it begins a run from a panel that has
	 * not shown anything yet.
	 *
	 * STOP is the only one that cannot turn the element on. At worst it is
	 * a no-op.
	 *
	 * The window lasts until the first publish -
	 * CONFIG_REFLOW_PUBLISH_PERIOD_MS, 500 ms by default - and its price is
	 * that a press in that instant does not start the run. The operator
	 * presses again.
	 */
	if (!state_known) {
		return REFLOW_CMD_STOP;
	}

	switch (state) {
	case REFLOW_STATE_RUNNING:
		/*
		 * The defect in the title. With a run in progress ANY press is
		 * a request to stop: it is what the operator wants, and it is
		 * the only command whose refusal cannot leave the element on.
		 */
		return REFLOW_CMD_STOP;

	case REFLOW_STATE_FAULT:
		if (long_press) {
			/* The only legitimate use of a long press, and today's. */
			return REFLOW_CMD_CLEAR_FAULT;
		}
		/*
		 * A short press in FAULT still posts START, and that is
		 * DELIBERATE (RFO-B13). Not because START is useful there -
		 * handle_cmd() refuses it - but because it is the only command
		 * whose refusal is EXPLAINED: "start refused: clear the fault
		 * first" (controller.c). STOP outside RUNNING is a silent
		 * no-op, so swapping it for STOP would erase the one line that
		 * tells the operator what to do next.
		 *
		 * Fixing handle_cmd()'s silence for a discarded command is a
		 * real defect of the same family, but it belongs to the core
		 * and reaches further than this button - out of scope here by
		 * the ticket's own decision.
		 */
		return REFLOW_CMD_START;

	case REFLOW_STATE_IDLE:
	case REFLOW_STATE_DONE:
		/*
		 * Idle or finished: a long press does the SAME as a short one,
		 * and starts. Between this and posting nothing, starting won,
		 * because posting nothing reproduces exactly the defect this
		 * ticket exists to remove - the operator acts and nothing
		 * happens, with no feedback at all.
		 *
		 * The residual risk, said out loud: a button pressed by
		 * accident starts a run. That already holds for a short press,
		 * so it is not introduced here - what changes is the long press
		 * no longer being a surprising exception.
		 */
		return REFLOW_CMD_START;

	default:
		/*
		 * A state this table does not know. Separate from IDLE/DONE on
		 * purpose (RFO-B13 review): folded in with them, an unknown
		 * value answered START - the command that energises - while the
		 * !state_known branch above answers STOP for the very same
		 * ignorance. Same not-knowing, opposite decisions.
		 *
		 * Unreachable while the enum has four values and controller.c
		 * is the only publisher. It stops being unreachable the day the
		 * enum GROWS: a PREHEAT, a COOLING, added by someone who will
		 * not remember there is a button table in src/ui/. On that day
		 * this returns the command that cannot turn the element on,
		 * instead of the one that can.
		 */
		return REFLOW_CMD_STOP;
	}
}
