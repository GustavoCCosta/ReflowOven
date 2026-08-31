/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Minimal HTTP/1.1 server for the web UI. Optional: CONFIG_REFLOW_NET.
 *
 *   GET  /              single page UI
 *   GET  /api/profiles  {"profiles":["...", ...]}
 *   GET  /api/state     one telemetry object
 *   GET  /api/events    text/event-stream, one telemetry object per push
 *   POST /api/cmd?id=start|stop|clear|profile[&arg=N]
 *
 * Written directly on BSD sockets (single thread, poll based) rather than on
 * the HTTP server subsystem: the socket API is stable across Zephyr releases
 * and the whole surface here is four routes. Server-sent events replace a
 * WebSocket because telemetry is one-directional; commands are plain POSTs.
 */

#include <errno.h>
#include <stdio.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/net/socket.h>

#include "../core/app.h"
#include "../telemetry_json.h"
#include "cmdparse.h"
#include "httpgate.h"
#include "index_html.h"
#include "net.h"
#include "profiles_json.h"
#include "sendbudget.h"

LOG_MODULE_REGISTER(reflow_httpd, CONFIG_REFLOW_LOG_LEVEL);

#define PORT        CONFIG_REFLOW_NET_HTTP_PORT
#define MAX_CLIENTS CONFIG_REFLOW_NET_MAX_CLIENTS
#define PUSH_MS     CONFIG_REFLOW_NET_PUSH_PERIOD_MS
#define REQ_BUF_SZ  512
#define SEND_BUDGET_MS CONFIG_REFLOW_NET_SEND_BUDGET_MS

/*
 * How long one zsock_send() may block before it hands control back so the
 * budget can be re-checked. This is the granularity of the deadline, not the
 * deadline: a client gets SEND_BUDGET_MS of no progress in total, checked
 * every slice. Small enough that the stall is short, large enough not to spin.
 */
#define SEND_SLICE_MS 100
#define JSON_BUF_SZ REFLOW_JSON_BUF_SZ

static struct k_mutex snap_lock;
static struct reflow_telemetry snapshot;

static void telemetry_cb(const struct zbus_channel *chan)
{
	const struct reflow_telemetry *t = zbus_chan_const_msg(chan);

	if (k_mutex_lock(&snap_lock, K_MSEC(5)) == 0) {
		snapshot = *t;
		k_mutex_unlock(&snap_lock);
	}
}

ZBUS_LISTENER_DEFINE(reflow_httpd_lsnr, telemetry_cb);
ZBUS_CHAN_ADD_OBS(reflow_telemetry_chan, reflow_httpd_lsnr, 4);

static int build_state_json(char *buf, size_t len)
{
	struct reflow_telemetry t;

	k_mutex_lock(&snap_lock, K_FOREVER);
	t = snapshot;
	k_mutex_unlock(&snap_lock);

	return reflow_telemetry_json(&t, buf, len);
}

/*
 * Send it all, or give up on this client - but never park the server thread
 * on one socket (RFO-B07).
 *
 * The accepted socket carries SO_SNDTIMEO, so zsock_send() returns EAGAIN
 * after a slice instead of blocking for ever. Whether that means "slow" or
 * "gone" is not a socket question, and it is not answered here: the budget in
 * sendbudget.c decides, from the time since the last byte actually moved.
 */
static int send_all(int fd, const char *data, size_t len)
{
	struct reflow_send_budget budget;

	reflow_send_budget_start(&budget, k_uptime_get(), SEND_BUDGET_MS);

	while (len > 0) {
		ssize_t sent = zsock_send(fd, data, len, 0);

		if (sent > 0) {
			data += sent;
			len -= (size_t)sent;
			reflow_send_budget_progress(&budget, k_uptime_get());
			continue;
		}

		/*
		 * EAGAIN here is the slice expiring with the peer not reading.
		 * Anything else is a dead socket and there is nothing to wait for.
		 */
		if (sent < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
			if (!reflow_send_budget_expired(&budget, k_uptime_get())) {
				continue;
			}
			LOG_WRN("client stopped reading, dropping it after %d ms",
				SEND_BUDGET_MS);
		}

		reflow_send_budget_done(&budget);
		return -1;
	}

	reflow_send_budget_done(&budget);
	return 0;
}

/*
 * Every response the server ever sends goes through here, and it emits NO
 * Access-Control-Allow-Origin. That absence is load bearing: with it, a page on
 * another origin could read the answer to a command it managed to send. Do not
 * add a CORS header here "for convenience" without reopening RFO-B02.
 */
static int send_response(int fd, const char *status, const char *ctype,
			 const char *body, size_t body_len)
{
	char hdr[160];
	int n = snprintk(hdr, sizeof(hdr),
			 "HTTP/1.1 %s\r\nContent-Type: %s\r\n"
			 "Content-Length: %zu\r\nCache-Control: no-store\r\n"
			 "Connection: close\r\n\r\n",
			 status, ctype, body_len);

	if (send_all(fd, hdr, (size_t)n) != 0) {
		return -1;
	}
	if (body_len > 0) {
		return send_all(fd, body, body_len);
	}
	return 0;
}

static int send_profiles(int fd)
{
	char body[256];
	size_t n = reflow_profiles_json(body, sizeof(body), reflow_profile_get,
					reflow_profile_count());

	/*
	 * Zero means not even the empty form fit, which with a 256 byte buffer
	 * cannot happen today - it would take shrinking this array below
	 * REFLOW_PROFILES_JSON_MIN. Handled anyway, because the alternative is
	 * sending a Content-Length of 0 with a 200 and letting the page parse an
	 * empty body as JSON.
	 */
	if (n == 0U) {
		LOG_ERR("profile list does not fit in %zu bytes", sizeof(body));
		return send_response(fd, "500 Internal Server Error", "text/plain", "", 0);
	}

	return send_response(fd, "200 OK", "application/json", body, n);
}

static int handle_cmd(int fd, const char *query)
{
	enum reflow_cmd_parse res;
	struct reflow_cmd cmd;

	res = reflow_cmd_parse(query, &cmd);
	if (res == REFLOW_CMD_PARSE_REJECT) {
		return send_response(fd, "400 Bad Request", "text/plain", "", 0);
	}

	if (reflow_cmd_post(&cmd, K_MSEC(100)) != 0) {
		return send_response(fd, "503 Service Unavailable", "text/plain", "", 0);
	}

	if (res == REFLOW_CMD_PARSE_REJECT_STOP) {
		/* The stop was honoured; the request itself was still malformed. */
		LOG_WRN("ambiguous /api/cmd request, stopped as a precaution");
		return send_response(fd, "400 Bad Request", "text/plain", "", 0);
	}
	return send_response(fd, "204 No Content", "text/plain", "", 0);
}

static int push_event(int fd)
{
	char json[JSON_BUF_SZ];
	char frame[JSON_BUF_SZ + 16];
	int n = build_state_json(json, sizeof(json));

	n = snprintk(frame, sizeof(frame), "data: %s\n\n", json);
	return send_all(fd, frame, (size_t)n);
}

struct client {
	int fd;
	bool sse;
};

static struct client clients[MAX_CLIENTS];

static void client_close(struct client *cl)
{
	if (cl->fd >= 0) {
		zsock_close(cl->fd);
		cl->fd = -1;
		cl->sse = false;
	}
}

/* Returns true to keep the connection open (SSE), false to close it. */
static bool serve(struct client *cl)
{
	char buf[REQ_BUF_SZ];
	char *method, *target, *query, *sp;
	enum reflow_gate gate;
	ssize_t got;

	got = zsock_recv(cl->fd, buf, sizeof(buf) - 1, 0);
	if (got <= 0) {
		return false;
	}
	buf[got] = '\0';

	/*
	 * The authorisation gate has to see the request exactly as it arrived:
	 * the parsing below writes NUL bytes into the request line and would hide
	 * the header block from it. It answers ALLOW for everything that is not a
	 * command, so one call before the routing covers every route.
	 */
	gate = reflow_gate_check(buf, CONFIG_REFLOW_NET_TOKEN);

	method = buf;
	sp = strchr(buf, ' ');
	if (sp == NULL) {
		return false;
	}
	*sp = '\0';
	target = sp + 1;
	sp = strchr(target, ' ');
	if (sp != NULL) {
		*sp = '\0';
	}

	query = strchr(target, '?');
	if (query != NULL) {
		*query++ = '\0';
	}

	LOG_DBG("%s %s", method, target);

	if (gate != REFLOW_GATE_ALLOW) {
		/*
		 * Only /api/cmd can land here, and nothing was posted to the
		 * control core. Logged at warning level because an oven refusing
		 * a command is something the operator has to be able to find.
		 */
		LOG_WRN("%s %s refused: %s", method, target,
			reflow_gate_status(gate));
		(void)send_response(cl->fd, reflow_gate_status(gate),
				    "text/plain", "", 0);
		return false;
	}

	if (strcmp(method, "GET") == 0) {
		if (strcmp(target, "/") == 0 || strcmp(target, "/index.html") == 0) {
			(void)send_response(cl->fd, "200 OK", "text/html; charset=utf-8",
					    index_html, sizeof(index_html) - 1);
			return false;
		}
		if (strcmp(target, "/api/profiles") == 0) {
			(void)send_profiles(cl->fd);
			return false;
		}
		if (strcmp(target, "/api/state") == 0) {
			char json[JSON_BUF_SZ];
			int n = build_state_json(json, sizeof(json));

			(void)send_response(cl->fd, "200 OK", "application/json",
					    json, (size_t)n);
			return false;
		}
		if (strcmp(target, "/api/events") == 0) {
			static const char hdr[] =
				"HTTP/1.1 200 OK\r\n"
				"Content-Type: text/event-stream\r\n"
				"Cache-Control: no-store\r\n"
				"Connection: keep-alive\r\n\r\n";

			if (send_all(cl->fd, hdr, sizeof(hdr) - 1) != 0) {
				return false;
			}
			cl->sse = true;
			return push_event(cl->fd) == 0;
		}
	} else if (strcmp(method, "POST") == 0 && strcmp(target, "/api/cmd") == 0) {
		(void)handle_cmd(cl->fd, query);
		return false;
	}

	(void)send_response(cl->fd, "404 Not Found", "text/plain", "", 0);
	return false;
}

static int listen_socket(void)
{
	struct sockaddr_in addr = {
		.sin_family = AF_INET,
		.sin_port = htons(PORT),
		.sin_addr.s_addr = htonl(INADDR_ANY),
	};
	int fd, opt = 1;

	fd = zsock_socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	if (fd < 0) {
		LOG_ERR("socket: %d", errno);
		return -1;
	}
	(void)zsock_setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

	if (zsock_bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
		LOG_ERR("bind: %d", errno);
		zsock_close(fd);
		return -1;
	}
	if (zsock_listen(fd, MAX_CLIENTS) < 0) {
		LOG_ERR("listen: %d", errno);
		zsock_close(fd);
		return -1;
	}

	LOG_INF("http server listening on :%d", PORT);
	return fd;
}

static void httpd_thread(void *a, void *b, void *c)
{
	struct zsock_pollfd fds[MAX_CLIENTS + 1];
	int64_t last_push;
	int srv;

	ARG_UNUSED(a);
	ARG_UNUSED(b);
	ARG_UNUSED(c);

	k_mutex_init(&snap_lock);
	for (int i = 0; i < MAX_CLIENTS; i++) {
		clients[i].fd = -1;
	}

	if (reflow_net_wait_ready(K_MINUTES(5)) != 0) {
		LOG_WRN("no network after 5 min, http server not started");
		return;
	}

	srv = listen_socket();
	if (srv < 0) {
		return;
	}

	if (CONFIG_REFLOW_NET_TOKEN[0] == '\0') {
		LOG_WRN("no CONFIG_REFLOW_NET_TOKEN: remote commands are DISABLED "
			"(POST /api/cmd answers 503). Telemetry is still served.");
	}

	last_push = k_uptime_get();

	while (true) {
		int nfds = 1;

		fds[0].fd = srv;
		fds[0].events = ZSOCK_POLLIN;
		fds[0].revents = 0;

		for (int i = 0; i < MAX_CLIENTS; i++) {
			if (clients[i].fd >= 0) {
				fds[nfds].fd = clients[i].fd;
				fds[nfds].events = ZSOCK_POLLIN;
				fds[nfds].revents = 0;
				nfds++;
			}
		}

		if (zsock_poll(fds, nfds, PUSH_MS / 2) < 0) {
			LOG_ERR("poll: %d", errno);
			k_sleep(K_MSEC(100));
			continue;
		}

		/* New connection. */
		if (fds[0].revents & ZSOCK_POLLIN) {
			int fd = zsock_accept(srv, NULL, NULL);

			if (fd >= 0) {
				struct client *slot = NULL;

				for (int i = 0; i < MAX_CLIENTS; i++) {
					if (clients[i].fd < 0) {
						slot = &clients[i];
						break;
					}
				}
				if (slot == NULL) {
					LOG_WRN("client limit reached");
					zsock_close(fd);
				} else {
					/*
					 * Without this the send below can block for
					 * ever on one peer and take the whole server
					 * with it (RFO-B07). A failure here is not
					 * fatal but it does restore the old hazard, so
					 * it is logged rather than ignored.
					 */
					struct zsock_timeval tv = {
						.tv_sec = SEND_SLICE_MS / 1000,
						.tv_usec = (SEND_SLICE_MS % 1000) * 1000,
					};

					if (zsock_setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO,
							     &tv, sizeof(tv)) < 0) {
						LOG_WRN("SO_SNDTIMEO: %d; a stalled client "
							"can block the server", errno);
					}
					slot->fd = fd;
					slot->sse = false;
				}
			}
		}

		/* Requests / disconnects on existing connections. */
		for (int i = 0; i < MAX_CLIENTS; i++) {
			struct client *cl = &clients[i];
			bool readable = false;

			if (cl->fd < 0) {
				continue;
			}
			for (int j = 1; j < nfds; j++) {
				if (fds[j].fd == cl->fd) {
					readable = (fds[j].revents &
						    (ZSOCK_POLLIN | ZSOCK_POLLHUP |
						     ZSOCK_POLLERR)) != 0;
					break;
				}
			}
			if (!readable) {
				continue;
			}
			if (cl->sse) {
				/* Any traffic on an event stream means it ended. */
				client_close(cl);
			} else if (!serve(cl)) {
				client_close(cl);
			}
		}

		/* Periodic telemetry push to every open event stream. */
		if (k_uptime_get() - last_push >= PUSH_MS) {
			last_push = k_uptime_get();
			for (int i = 0; i < MAX_CLIENTS; i++) {
				if (clients[i].sse && clients[i].fd >= 0 &&
				    push_event(clients[i].fd) != 0) {
					client_close(&clients[i]);
				}
			}
		}
	}
}

K_THREAD_DEFINE(reflow_httpd_tid, CONFIG_REFLOW_NET_STACK_SIZE, httpd_thread,
		NULL, NULL, NULL, 8, 0, 1000);
