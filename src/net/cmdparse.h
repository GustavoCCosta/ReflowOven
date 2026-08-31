/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Command dispatch for POST /api/cmd, split out of httpd.c so that it can be
 * exercised without a network stack. Pure logic: no sockets, no Zephyr
 * subsystems, no global state.
 */

#ifndef REFLOW_NET_CMDPARSE_H_
#define REFLOW_NET_CMDPARSE_H_

#include "../core/app.h"

enum reflow_cmd_parse {
	/* cmd is valid: post it and answer 204. */
	REFLOW_CMD_PARSE_OK = 0,
	/* Nothing is executed; answer 400. */
	REFLOW_CMD_PARSE_REJECT,
	/*
	 * The request is malformed or ambiguous, but a stop was among the
	 * possible readings. cmd holds REFLOW_CMD_STOP: post it, then answer
	 * 400. Stopping an oven that was already idle costs nothing; ignoring
	 * a stop the operator meant to send does not.
	 */
	REFLOW_CMD_PARSE_REJECT_STOP,
};

/*
 * Parse the query of POST /api/cmd.
 *
 * A profile index outside 0..reflow_profile_count()-1 is REJECTed, not
 * clamped and not passed on (RFO-B10). It used to travel to the core, where
 * reflow_profile_get((uint8_t)cmd->arg) truncated it: arg=256 became profile 0
 * - the lead-free profile that goes to 245 degC - and the API answered 204, so
 * a board meant for the 120 degC bake ran the soldering profile with the UI
 * showing the request as accepted.
 */
enum reflow_cmd_parse reflow_cmd_parse(const char *query, struct reflow_cmd *cmd);

#endif /* REFLOW_NET_CMDPARSE_H_ */
