/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * RFO-G44. The RFO-B08 scenario: connections that open and never speak.
 *
 * A client that connects and says nothing takes a slot. Four of them take
 * every slot, and from then on accept() closes each arrival on sight - the web
 * UI and the remote Stop button are gone until the board is reset. Nothing
 * malicious is needed: forgotten browser tabs are enough.
 *
 * Over the real loopback, not a fake, and the choice is the Gerente's with a
 * reason this suite inherits: this scenario depends on a COUNT OF CONNECTIONS,
 * which loopback gives for free and with full fidelity - opening N sockets and
 * never reading from them is the defect, literally. Its opposite is RFO-B07 in
 * tests/httpd/, which depends on when the peer stops reading - a decision of
 * the kernel's TCP window, and therefore one that needs a fake to be made on
 * command.
 *
 * So httpd.c here is linked, not included: nothing static is under test. What
 * is under test is what the server does with the sockets it is handed.
 */

#include <errno.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/net/socket.h>
#include <zephyr/ztest.h>

#include "net_wait_fake.h"

/*
 * From the symbol that sizes the pool, never a literal: if somebody changes
 * the dimensioning, this suite has to follow it instead of going on measuring
 * a number that stopped being the limit.
 */
#define POOL      CONFIG_REFLOW_NET_MAX_CLIENTS
#define IDLE_MS   CONFIG_REFLOW_NET_IDLE_BUDGET_MS

/* One more than the pool: the point is to exhaust it, not to fill it. */
#define IDLERS    (POOL + 1)

/* How long the suite waits for the server to come up before giving up. */
#define SETTLE_MS 4000

/*
 * Slack over the idle budget. The server only reclaims on its way round the
 * poll loop, so the wait has to cover the budget plus one pass - and on a
 * simulated platform under CI load, generously.
 *
 * Waited once, and then ONE request, rather than retrying in a loop. The first
 * version of this test polled every 250 ms and cost 1840 s on qemu_x86 - five
 * times the next slowest suite in the tree - because every probe opens a
 * connection against a listen backlog the silent clients have already filled,
 * and those connects do not fail quickly. There is nothing to poll for anyway:
 * the deadline is deterministic, so waiting it out once is both cheaper and a
 * sharper claim.
 */
#define RECLAIM_MS (IDLE_MS + 4000)

static int idlers[IDLERS];

static int connect_only(void)
{
	struct sockaddr_in addr = {
		.sin_family = AF_INET,
		.sin_port = htons(CONFIG_REFLOW_NET_HTTP_PORT),
	};
	int fd, ret;

	if (zsock_inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr) != 1) {
		return -EINVAL;
	}

	fd = zsock_socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	if (fd < 0) {
		return -errno;
	}
	if (zsock_connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
		ret = -errno;
		zsock_close(fd);
		return ret;
	}

	/* Deliberately nothing else: no request, no recv. This is the defect. */
	return fd;
}

/* A normal client: asks, reads, closes. Returns the bytes it got back. */
static int get_state(char *buf, size_t len)
{
	static const char req[] = "GET /api/state HTTP/1.1\r\n"
				  "Host: oven\r\n"
				  "Connection: close\r\n\r\n";
	struct sockaddr_in addr = {
		.sin_family = AF_INET,
		.sin_port = htons(CONFIG_REFLOW_NET_HTTP_PORT),
	};
	size_t got = 0;
	int fd, ret;

	if (zsock_inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr) != 1) {
		return -EINVAL;
	}

	fd = zsock_socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	if (fd < 0) {
		return -errno;
	}
	if (zsock_connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
		ret = -errno;
		zsock_close(fd);
		return ret;
	}
	if (zsock_send(fd, req, sizeof(req) - 1, 0) < 0) {
		ret = -errno;
		zsock_close(fd);
		return ret;
	}

	while (got + 1 < len) {
		ret = zsock_recv(fd, &buf[got], len - got - 1, 0);
		if (ret <= 0) {
			break;
		}
		got += (size_t)ret;
	}
	buf[got] = '\0';
	zsock_close(fd);

	return (int)got;
}

static bool wait_for(bool (*cond)(void), int32_t limit_ms)
{
	for (int32_t waited = 0; waited <= limit_ms; waited += 100) {
		if (cond()) {
			return true;
		}
		k_sleep(K_MSEC(100));
	}
	return cond();
}

static bool server_is_listening(void)
{
	char buf[256];

	return get_state(buf, sizeof(buf)) > 0;
}

static void *suite_setup(void)
{
	for (int i = 0; i < IDLERS; i++) {
		idlers[i] = -1;
	}
	return NULL;
}

static void scenario_teardown(void *f)
{
	ARG_UNUSED(f);

	for (int i = 0; i < IDLERS; i++) {
		if (idlers[i] >= 0) {
			zsock_close(idlers[i]);
			idlers[i] = -1;
		}
	}
}

/*
 * The acceptance criterion, and the assertion is about the client that comes
 * AFTER the silent ones - the operator arriving to press Stop. That is what
 * RFO-B08 broke, and asserting on the idle sockets themselves would measure
 * the wrong end of it.
 */
ZTEST(reflow_httpidle, test_silent_connections_do_not_lock_the_operator_out)
{
	char buf[512];
	int n;

	zassert_true(wait_for(server_is_listening, SETTLE_MS),
		     "fixture: the server never started listening on port %d",
		     CONFIG_REFLOW_NET_HTTP_PORT);

	/* More silent connections than the pool has slots. */
	for (int i = 0; i < IDLERS; i++) {
		idlers[i] = connect_only();
		zassert_true(idlers[i] >= 0,
			     "fixture: silent connection %d failed: %d",
			     i, idlers[i]);
	}

	/*
	 * Now the operator. Without the reclaim in httpd.c every slot is held
	 * by a socket that will never speak, so this accept() is closed on
	 * arrival and the request gets nothing back - for ever, because none of
	 * the holders is going anywhere.
	 *
	 * With it, the slots come back after IDLE_BUDGET_MS and the request is
	 * served. So the wait is not slack around a flaky test: it is the
	 * mechanism under test, and its length is the budget itself.
	 *
	 * One request after one wait, deliberately. Retrying would turn a
	 * deterministic deadline into "it worked eventually", and eventually is
	 * not what the operator gets.
	 */
	k_sleep(K_MSEC(RECLAIM_MS));
	n = get_state(buf, sizeof(buf));

	zassert_true(n > 0,
		     "a client arriving after %d silent connections got nothing "
		     "back %d ms later (pool is %d, idle budget %d ms). The "
		     "slots are held by sockets that never spoke, so the web UI "
		     "and the remote Stop button are gone until the board is "
		     "reset (RFO-B08)",
		     IDLERS, RECLAIM_MS, POOL, IDLE_MS);

	zassert_not_null(strstr(buf, "200 OK"),
			 "the answer was not a 200: %s", buf);
	zassert_not_null(strstr(buf, "temp_mc"),
			 "the answer carried no telemetry: %s", buf);
}

/*
 * The counterpart, and the reason the reclaim is written against silence
 * rather than against age: a connection that ASKS is served, and the pool is
 * not something the server empties on a timer.
 */
ZTEST(reflow_httpidle, test_a_client_that_speaks_is_served_immediately)
{
	char buf[512];
	int n;

	zassert_true(wait_for(server_is_listening, SETTLE_MS),
		     "fixture: the server never started listening");

	n = get_state(buf, sizeof(buf));

	zassert_true(n > 0, "a lone, well-behaved client got nothing back");
	zassert_not_null(strstr(buf, "200 OK"),
			 "the answer was not a 200: %s", buf);
}

ZTEST_SUITE(reflow_httpidle, NULL, suite_setup, NULL, scenario_teardown, NULL);
