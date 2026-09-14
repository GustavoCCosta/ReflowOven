/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * What the encoder's button commands, given the oven state and how long the
 * press lasted. Pure, no Zephyr, tested in tests/logic/ - the shape CLAUDE.md
 * reserves for "logic that decides whether the element comes on", next to
 * net/cmdparse.c and net/httpgate.c.
 *
 * Why it is separate from input_ui.c (RFO-B13): this decision is the operator's
 * stop path when there is no network. Inside an input-subsystem callback it
 * would only be exercisable with an image, a driver and events; here it is a
 * table a test walks end to end.
 */

#ifndef REFLOW_BUTTONMAP_H_
#define REFLOW_BUTTONMAP_H_

#include <stdbool.h>
#include <stdint.h>

/* No command to post. Distinct from every REFLOW_CMD_*, which are >= 0. */
#define REFLOW_BUTTON_NONE (-1)

/*
 * `state` is a REFLOW_STATE_*, valid only when `state_known`. Before the first
 * telemetry frame the oven state is unknown, and that distinction is what
 * decides safety - see the comment in the .c.
 *
 * Returns a REFLOW_CMD_* or REFLOW_BUTTON_NONE.
 */
int reflow_button_decide(bool state_known, uint8_t state, bool long_press);

#endif /* REFLOW_BUTTONMAP_H_ */
