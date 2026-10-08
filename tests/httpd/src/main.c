/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * RFO-G42. First test that exercises src/net/httpd.c itself.
 *
 * The scenario is RFO-B07: a client that asks for the page and stops reading
 * it. On a blocking socket, from the server's only thread, zsock_send() parks
 * there for ever - the thread never returns to zsock_poll(), the event stream
 * stops for EVERY client, and the remote Stop button dies while the oven is at
 * 245 degC. One browser tab on a bad Wi-Fi is enough.
 *
 * Why the file is included rather than linked: send_all() is static, and it is
 * where the fix lives. Including httpd.c into this translation unit is what
 * lets the test call the real function instead of a copy of it - the lesson of
 * RFO-B24, where a test re-implemented what it claimed to measure. The
 * CMakeLists therefore does NOT list httpd.c in target_sources.
 *
 * Order matters in the three includes below and is not incidental:
 *
 *   1. <zephyr/net/socket.h> brings the real declarations in.
 *   2. socket_fake.h then redirects the NAMES.
 *   3. httpd.c is compiled with its call sites pointing at the fake, and with
 *      no other change - byte for byte the file that is flashed.
 */

#include <zephyr/net/socket.h>

#include "socket_fake.h"

#include <zephyr/kernel.h>
#include <zephyr/ztest.h>

#include "httpd.c"

/*
 * A body big enough that no peer swallows it in one call, and recognisably
 * bigger than one slice of a trickling one.
 */
#define BODY_LEN 64
static char body[BODY_LEN];

/*
 * The bound that says "the server thread came back". It is not a performance
 * assertion: SEND_BUDGET_MS is the contract, and three times it is generous
 * slack for a simulated platform under CI load. What it rules out is the only
 * failure that matters here - never coming back at all.
 */
#define RETURN_BOUND_MS (SEND_BUDGET_MS * 3)

static void setup_body(void)
{
	for (int i = 0; i < BODY_LEN; i++) {
		body[i] = 'x';
	}
}

static void *suite_setup(void)
{
	setup_body();
	return NULL;
}

static void scenario_reset(void *f)
{
	ARG_UNUSED(f);
	reflow_fake_reset();
}

/*
 * THE test of this ticket. A peer that never accepts a byte must cost the
 * server a bounded amount of time and then be given up on.
 *
 * Both halves are asserted, and the second is the one that would be missed:
 * returning is not enough if it took for ever to do it, and giving up is not
 * enough if the server kept the socket.
 */
ZTEST(reflow_httpd, test_a_client_that_stops_reading_does_not_park_the_server)
{
	int fd = reflow_fake_open(REFLOW_FAKE_PEER_STALLED);
	int64_t t0, elapsed;
	int ret;

	zassert_true(fd >= 0, "fixture: no fake descriptor available");

	t0 = k_uptime_get();
	ret = send_all(fd, body, BODY_LEN);
	elapsed = k_uptime_get() - t0;

	zassert_not_equal(ret, 0,
			  "send_all() reported success to a peer that accepted "
			  "zero bytes");

	zassert_true(elapsed < RETURN_BOUND_MS,
		     "send_all() held the server thread for %lld ms against a "
		     "budget of %d ms. This is RFO-B07: the thread never gets "
		     "back to zsock_poll(), so the event stream stops for every "
		     "client and the remote Stop button is gone",
		     elapsed, SEND_BUDGET_MS);

	zassert_equal(reflow_fake_accepted(fd), 0U,
		      "fixture: the stalled peer accepted %zu bytes",
		      reflow_fake_accepted(fd));
}

/*
 * The other half of RFO-B07, and the half that separates a real fix from a
 * timer on the connection: a peer that reads slowly but KEEPS reading must
 * never be dropped, however long the transfer takes.
 *
 * At one byte per slice, this transfer lasts BODY_LEN * 100 ms - far past the
 * budget - and it has to succeed. A fix that counted time on the connection
 * instead of time without progress would pass the test above and fail here.
 */
ZTEST(reflow_httpd, test_a_slow_but_progressing_client_is_never_dropped)
{
	int fd = reflow_fake_open(REFLOW_FAKE_PEER_TRICKLE);
	int64_t t0, elapsed;
	int ret;

	zassert_true(fd >= 0, "fixture: no fake descriptor available");

	t0 = k_uptime_get();
	ret = send_all(fd, body, BODY_LEN);
	elapsed = k_uptime_get() - t0;

	zassert_equal(ret, 0,
		      "send_all() gave up on a peer that was still accepting "
		      "bytes: the budget is measuring time on the connection "
		      "instead of time without progress (RFO-B07)");

	zassert_equal(reflow_fake_accepted(fd), (size_t)BODY_LEN,
		      "the peer accepted %zu of %d bytes",
		      reflow_fake_accepted(fd), BODY_LEN);

	zassert_true(elapsed > SEND_BUDGET_MS,
		     "fixture: the transfer took %lld ms, which is not longer "
		     "than the %d ms budget - this scenario only proves "
		     "something if it outlives the deadline",
		     elapsed, SEND_BUDGET_MS);
}

/* The healthy client, so the two above are not the only shapes measured. */
ZTEST(reflow_httpd, test_a_fast_client_gets_everything)
{
	int fd = reflow_fake_open(REFLOW_FAKE_PEER_FAST);

	zassert_true(fd >= 0, "fixture: no fake descriptor available");
	zassert_equal(send_all(fd, body, BODY_LEN), 0, "send_all() failed");
	zassert_equal(reflow_fake_accepted(fd), (size_t)BODY_LEN,
		      "the peer accepted %zu of %d bytes",
		      reflow_fake_accepted(fd), BODY_LEN);
}

/*
 * ---------------------------------------------------------------------------
 * RFO-B30. The length build_state_json() reports.
 *
 * snprintk() returns the length of the string it was ASKED to produce, not of
 * the one it produced. Both callers hand that number straight to send_all()
 * as a byte count, so on truncation the server reads past a 384 byte array on
 * its own thread stack and sends what follows it to the client. Not a
 * corrupted page: a stack leak over the network, on the interface anything in
 * cable or radio range can reach.
 *
 * Reached here by shrinking the buffer rather than by lengthening a name. The
 * issue describes the defect becoming reachable when some stage, profile or
 * fault name grows past the slack in REFLOW_JSON_BUF_SZ, and that is true -
 * but it is the same arithmetic from the other side, and it is the arithmetic
 * that is wrong. A test that needed a longer name would also need the
 * built-in table changed to produce one, which measures the table instead of
 * the clamp.
 * ---------------------------------------------------------------------------
 */

/*
 * A buffer with a known pattern on both sides of the part the code may touch.
 * If the clamp is missing the assertions below fail on the returned length
 * alone, but the canary is what distinguishes "reported too much" from
 * "wrote too much", and only one of those two is a memory bug.
 */
#define CANARY 0x5a
#define PAD    32

struct guarded {
	unsigned char before[PAD];
	char body[JSON_BUF_SZ];
	unsigned char after[PAD];
};

static void guarded_init(struct guarded *g)
{
	memset(g, CANARY, sizeof(*g));
}

static void guarded_check(const struct guarded *g, size_t cap)
{
	size_t i;

	for (i = 0; i < PAD; i++) {
		zassert_equal(g->before[i], CANARY,
			      "wrote %zu bytes BEFORE the buffer", PAD - i);
		zassert_equal(g->after[i], CANARY,
			      "wrote past the end of the buffer at +%zu", i);
	}
	for (i = cap; i < sizeof(g->body); i++) {
		zassert_equal((unsigned char)g->body[i], CANARY,
			      "wrote past the cap it was given, at +%zu", i - cap);
	}
}

/*
 * THE assertion of this ticket, and it is about the RETURN VALUE, because the
 * return value is what becomes a byte count.
 *
 * Every cap from one byte to comfortably more than the object needs. One of
 * them is the boundary where the text stops fitting, and an off-by-one in the
 * clamp shows up here instead of on a board.
 */
ZTEST(reflow_httpd, test_the_reported_length_is_never_more_than_was_written)
{
	size_t cap;

	for (cap = 1; cap <= JSON_BUF_SZ; cap++) {
		struct guarded g;
		int n;

		guarded_init(&g);
		n = build_state_json(g.body, cap);

		zassert_true(n >= 0, "cap %zu: negative length %d", cap, n);
		zassert_true((size_t)n < cap,
			     "cap %zu: reported %d bytes, which is what snprintk "
			     "WOULD have written - send_all() takes this as a "
			     "byte count and reads %zu bytes past a buffer on "
			     "the server's own stack, out to the client "
			     "(RFO-B30)",
			     cap, n, (size_t)n - cap + 1U);
		zassert_equal((size_t)n, strlen(g.body),
			      "cap %zu: reported %d but the string is %zu long",
			      cap, n, strlen(g.body));
		guarded_check(&g, cap);
	}
}

/*
 * The other half, so the clamp cannot be satisfied by always returning zero:
 * at a comfortable size the answer is the whole object, not a truncation.
 */
ZTEST(reflow_httpd, test_a_comfortable_buffer_gets_the_whole_object)
{
	struct guarded g;
	int n;

	guarded_init(&g);
	n = build_state_json(g.body, sizeof(g.body));

	zassert_true(n > 0, "nothing was written into a full-size buffer");
	zassert_equal((size_t)n, strlen(g.body), "length disagrees with the string");
	zassert_equal(g.body[0], '{', "not a JSON object: %s", g.body);
	zassert_equal(g.body[n - 1], '}', "the object is not closed: %s", g.body);
	zassert_not_null(strstr(g.body, "temp_mc"), "no telemetry in it: %s", g.body);
	guarded_check(&g, sizeof(g.body));
}

/*
 * A truncated object is still terminated. The clamp exists so the LENGTH is
 * honest; this is the companion property the length is useless without,
 * because send_response() and the %s in push_event() both read the buffer as
 * a C string.
 */
ZTEST(reflow_httpd, test_a_truncated_object_is_still_a_string)
{
	struct guarded g;
	int n;

	guarded_init(&g);
	n = build_state_json(g.body, 16);

	zassert_true((size_t)n < 16U, "reported %d for a 16 byte buffer", n);
	zassert_equal(g.body[n], '\0',
		      "the buffer is not terminated at the length returned");
	guarded_check(&g, 16);
}

ZTEST_SUITE(reflow_httpd, NULL, suite_setup, scenario_reset, NULL, NULL);
