/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * "Has this client had its time?" - the one decision behind the send timeout,
 * as a pure function of an instant and the per-client state.
 *
 * Kept apart from httpd.c, and free of any Zephyr dependency, for the same
 * reason cmdparse.c, httpgate.c and profiles_json.c are: httpd.c is the shell
 * that talks to a socket, and the judgement it makes can be exercised with
 * nothing but integers (RFO-B07).
 *
 * The defect this serves: zsock_send() on a blocking socket, from the server's
 * only thread. A client that connects, asks for the 6 KB page and never reads
 * it fills the TCP send buffer and parks send_all() forever. The thread never
 * returns to zsock_poll(), so the event stream stops for EVERY client and the
 * remote Stop button dies - while the oven is at 245 degC. One browser tab on a
 * bad Wi-Fi is enough; nothing malicious is needed.
 *
 * What is measured here is time WITHOUT PROGRESS, not time on the connection:
 *
 *   - a slow client that keeps accepting bytes keeps resetting its clock and is
 *     never dropped, however long the transfer takes;
 *   - a client that stops accepting bytes is dropped after budget_ms;
 *   - an idle SSE stream is not sending anything, so its budget is never armed
 *     and this path can never close it. That distinction is the second item of
 *     the acceptance criteria and it is the whole reason the state is explicit
 *     instead of being a timestamp on the client struct.
 */

#ifndef REFLOW_SENDBUDGET_H_
#define REFLOW_SENDBUDGET_H_

#include <stdbool.h>
#include <stdint.h>

struct reflow_send_budget {
	int64_t since_ms;   /* when the current no-progress stretch began */
	int64_t budget_ms;  /* how long that stretch may last */
	bool armed;         /* false outside a send: nothing to expire */
};

/*
 * Begin a send. budget_ms <= 0 disables the deadline for this send, which is
 * how a caller opts out without a second code path.
 */
void reflow_send_budget_start(struct reflow_send_budget *b, int64_t now_ms,
			      int64_t budget_ms);

/* Bytes moved: the no-progress stretch starts over. */
void reflow_send_budget_progress(struct reflow_send_budget *b, int64_t now_ms);

/* End of the send, successful or not. An unarmed budget never expires. */
void reflow_send_budget_done(struct reflow_send_budget *b);

/*
 * True when this client has gone budget_ms without accepting a byte, and the
 * caller should give up on it and close.
 *
 * False whenever the budget is not armed, whatever now_ms is - an idle stream
 * cannot be closed by this path.
 */
bool reflow_send_budget_expired(const struct reflow_send_budget *b, int64_t now_ms);

#endif /* REFLOW_SENDBUDGET_H_ */
