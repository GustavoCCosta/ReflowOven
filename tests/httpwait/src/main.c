/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * RFO-B11: an oven whose link comes up late has to end up with a working web
 * UI, without a reboot.
 *
 * This is its own image, not a test in tests/controller, for the reason
 * tests/boot and tests/coldstart are their own images: the property is about
 * the state the httpd thread starts in. It waits for the link from
 * K_THREAD_DEFINE, before any test body can influence anything, so the
 * influence is a build-time value - REFLOW_NET_WAIT_FAKE_FAILURES=2 in this
 * directory's CMakeLists - and the whole image is built around a link that is
 * down at boot and appears afterwards.
 *
 * What the old code did: reflow_net_wait_ready(K_MINUTES(5)) once, and on
 * timeout the thread returned. No listening socket for the rest of the
 * session, so no page, no event stream and no remote Stop - on a mains heater,
 * with the local UI as the only way to intervene.
 *
 * The two tests below are deliberately different questions:
 *
 *   - the first one asks whether the thread came back to the gate at all. It
 *     is the direct negation of "return on timeout" and it needs no socket.
 *   - the second one asks the acceptance criterion, end to end: a client that
 *     shows up after the late link gets telemetry over a real socket. A server
 *     that looped but never reached listen_socket() would pass the first and
 *     fail this one.
 */

#include <errno.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/net/socket.h>
#include <zephyr/ztest.h>

#include "app.h"
#include "net_wait_fake.h"

/*
 * Every failed wait costs the fake's compressed timeout, and after the last one
 * the thread still has to create the socket and reach zsock_poll(). Give the
 * waits several times both.
 */
#define SETTLE_MS 5000
#define POLL_MS   50

static bool wait_for(bool (*done)(void), int64_t budget_ms)
{
	int64_t deadline = k_uptime_get() + budget_ms;

	while (true) {
		if (done()) {
			return true;
		}
		if (k_uptime_get() >= deadline) {
			return false;
		}
		k_msleep(POLL_MS);
	}
}

static bool came_back_to_the_gate(void)
{
	return reflow_net_wait_fake_calls() > REFLOW_NET_WAIT_FAKE_FAILURES;
}

/*
 * The gate is asked again after a timeout instead of the thread returning.
 *
 * REFLOW_NET_WAIT_FAKE_FAILURES timeouts, so a server that gives up on the
 * first one is stuck at 1 call, and one that retries a fixed number of times
 * stops at that number. The count has to pass the failures for the link to
 * have been seen at all.
 */
ZTEST(reflow_httpwait, test_espera_do_link_nao_desiste_no_timeout)
{
	zassert_true(wait_for(came_back_to_the_gate, SETTLE_MS),
		     "o servidor desistiu do link: %u chamadas a "
		     "reflow_net_wait_ready() para %u timeouts",
		     reflow_net_wait_fake_calls(),
		     (unsigned int)REFLOW_NET_WAIT_FAKE_FAILURES);
}

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

static bool server_is_listening(void)
{
	char buf[256];

	return get_state(buf, sizeof(buf)) > 0;
}

/*
 * The acceptance criterion of the issue: the client that arrives after the
 * link came up late is served. Without the patch there is nothing on the port
 * at all and this fails at connect().
 */
ZTEST(reflow_httpwait, test_ui_web_funciona_com_link_atrasado)
{
	char buf[512];
	int n;

	zassert_true(wait_for(reflow_net_wait_fake_stack_ready, SETTLE_MS),
		     "a imagem nao tem interface para escutar: 127.0.0.1 nao esta "
		     "registrado. Isso e configuracao desta suite (NET_DRIVERS, "
		     "NET_LOOPBACK), nao o servidor");
	zassert_true(wait_for(server_is_listening, SETTLE_MS),
		     "nada escutando em :%d depois de %d ms",
		     CONFIG_REFLOW_NET_HTTP_PORT, SETTLE_MS);

	n = get_state(buf, sizeof(buf));
	zassert_true(n > 0, "GET /api/state falhou: %d", n);
	zassert_not_null(strstr(buf, "200 OK"), "resposta sem 200: %s", buf);
	zassert_not_null(strstr(buf, "\"temp_mc\":"),
			 "resposta sem telemetria: %s", buf);
}

ZTEST_SUITE(reflow_httpwait, NULL, NULL, NULL, NULL, NULL);
