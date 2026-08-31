/* SPDX-License-Identifier: Apache-2.0 */

#include "sendbudget.h"

void reflow_send_budget_start(struct reflow_send_budget *b, int64_t now_ms,
			      int64_t budget_ms)
{
	b->since_ms = now_ms;
	b->budget_ms = budget_ms;
	b->armed = budget_ms > 0;
}

void reflow_send_budget_progress(struct reflow_send_budget *b, int64_t now_ms)
{
	b->since_ms = now_ms;
}

void reflow_send_budget_done(struct reflow_send_budget *b)
{
	b->armed = false;
}

bool reflow_send_budget_expired(const struct reflow_send_budget *b, int64_t now_ms)
{
	if (!b->armed) {
		return false;
	}

	/*
	 * Strictly greater, so a budget of N ms means "N ms of silence is still
	 * allowed". Subtracting instead of comparing now_ms against a stored
	 * deadline keeps this correct if the caller's clock is ever restarted
	 * behind us: a negative elapsed reads as "no time has passed", which
	 * errs towards keeping the client, and a client kept one extra round is
	 * a slow page load - a client dropped wrongly is a browser that loses
	 * the oven's telemetry.
	 */
	return (now_ms - b->since_ms) > b->budget_ms;
}
