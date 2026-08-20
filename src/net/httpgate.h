/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Authorisation gate for POST /api/cmd — the one HTTP route that can energise
 * the heating element. Kept apart from httpd.c and free of any dependency on
 * sockets, Zephyr subsystems or global state, so the whole decision can be
 * exercised in native_sim with nothing but a string.
 *
 * The gate is a pure function of two inputs: the request exactly as it came off
 * the wire, and the token the build was configured with. It never blocks, never
 * allocates and never writes to its inputs.
 */

#ifndef REFLOW_NET_HTTPGATE_H_
#define REFLOW_NET_HTTPGATE_H_

enum reflow_gate {
	/* Not a command request, or a command request that checked out. */
	REFLOW_GATE_ALLOW = 0,
	/* Malformed, truncated or ambiguous: 400. */
	REFLOW_GATE_BAD_REQUEST,
	/* Token missing or wrong: 401. */
	REFLOW_GATE_UNAUTHORIZED,
	/* Origin does not match Host, or Host is not a literal IPv4: 403. */
	REFLOW_GATE_FORBIDDEN,
	/* /api/cmd reached with anything other than POST: 405. */
	REFLOW_GATE_METHOD_NOT_ALLOWED,
	/* Build has no token: remote control is off, 503. */
	REFLOW_GATE_DISABLED,
};

/*
 * request: the bytes received, NUL terminated, unmodified. Passing a request
 *          whose line has already been chopped up defeats the header checks.
 * token:   CONFIG_REFLOW_NET_TOKEN. An empty string disables remote commands.
 *
 * Returns REFLOW_GATE_ALLOW for every request that is not POST-to-/api/cmd:
 * telemetry is read-only and stays open. Everything else is a refusal.
 */
enum reflow_gate reflow_gate_check(const char *request, const char *token);

/* "401 Unauthorized" and friends. NULL for REFLOW_GATE_ALLOW. */
const char *reflow_gate_status(enum reflow_gate verdict);

#endif /* REFLOW_NET_HTTPGATE_H_ */
