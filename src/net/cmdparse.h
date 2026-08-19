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

enum reflow_cmd_parse reflow_cmd_parse(const char *query, struct reflow_cmd *cmd);

#endif /* REFLOW_NET_CMDPARSE_H_ */
