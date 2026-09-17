/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * RFO-G42. See socket_fake.h for why the substitution is at the call site.
 *
 * This file does NOT include socket_fake.h's redirection - it defines the
 * functions the redirection points at, and renaming them here would rename the
 * definitions too.
 */

#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <sys/types.h>

#include <zephyr/kernel.h>

#include "socket_fake.h"

#define FAKE_FDS 8

struct fake_sock {
	bool open;
	enum reflow_fake_peer peer;
	size_t accepted;
	bool closed;
	bool stall_next;
};

static struct fake_sock socks[FAKE_FDS];

void reflow_fake_reset(void)
{
	for (int i = 0; i < FAKE_FDS; i++) {
		socks[i].open = false;
		socks[i].peer = REFLOW_FAKE_PEER_FAST;
		socks[i].accepted = 0U;
		socks[i].closed = false;
		socks[i].stall_next = false;
	}
}

int reflow_fake_open(enum reflow_fake_peer peer)
{
	for (int i = 0; i < FAKE_FDS; i++) {
		if (!socks[i].open) {
			socks[i].open = true;
			socks[i].peer = peer;
			socks[i].accepted = 0U;
			socks[i].closed = false;
			socks[i].stall_next = false;
			return i;
		}
	}
	return -1;
}

size_t reflow_fake_accepted(int fd)
{
	return (fd >= 0 && fd < FAKE_FDS) ? socks[fd].accepted : 0U;
}

bool reflow_fake_closed(int fd)
{
	return (fd >= 0 && fd < FAKE_FDS) ? socks[fd].closed : false;
}

/*
 * The one call that matters for RFO-B07.
 *
 * A stalled peer costs a slice and answers EAGAIN, exactly as a real socket
 * with SO_SNDTIMEO does when the receive window stays full. The slice is what
 * makes the scenario measurable in wall time instead of in iterations, and it
 * is also what the server's retry loop is written against - send_all() has no
 * sleep of its own, because the blocking send IS the sleep.
 */
ssize_t reflow_fake_send(int sock, const void *buf, size_t len, int flags)
{
	ARG_UNUSED(buf);
	ARG_UNUSED(flags);

	if (sock < 0 || sock >= FAKE_FDS || !socks[sock].open) {
		errno = EBADF;
		return -1;
	}
	if (len == 0U) {
		return 0;
	}

	switch (socks[sock].peer) {
	case REFLOW_FAKE_PEER_FAST:
		socks[sock].accepted += len;
		return (ssize_t)len;

	case REFLOW_FAKE_PEER_TRICKLE:
		/*
		 * ALTERNATES, and that is the whole point of this mode rather
		 * than a detail of it. A peer that returned a byte on every
		 * call would never take send_all() into its EAGAIN branch, so
		 * the budget would never be consulted and this scenario would
		 * be blind to what it exists to measure - the first version of
		 * this fake did exactly that, and the mutation stayed GREEN.
		 *
		 * A real window that is nearly full behaves this way: a little
		 * room, then none, then a little more.
		 */
		k_sleep(K_MSEC(REFLOW_FAKE_SLICE_MS));
		socks[sock].stall_next = !socks[sock].stall_next;
		if (socks[sock].stall_next) {
			socks[sock].accepted += 1U;
			return 1;
		}
		errno = EAGAIN;
		return -1;

	case REFLOW_FAKE_PEER_STALLED:
	default:
		k_sleep(K_MSEC(REFLOW_FAKE_SLICE_MS));
		errno = EAGAIN;
		return -1;
	}
}

ssize_t reflow_fake_recv(int sock, void *buf, size_t max_len, int flags)
{
	ARG_UNUSED(buf);
	ARG_UNUSED(max_len);
	ARG_UNUSED(flags);

	if (sock < 0 || sock >= FAKE_FDS || !socks[sock].open) {
		errno = EBADF;
		return -1;
	}
	/* Nothing to read: the peer of this suite never speaks first. */
	return 0;
}

int reflow_fake_close(int sock)
{
	if (sock >= 0 && sock < FAKE_FDS && socks[sock].open) {
		socks[sock].closed = true;
		socks[sock].open = false;
	}
	return 0;
}

/*
 * Refused on purpose, and it is what keeps the image quiet: httpd_thread()
 * gives up and returns when listen_socket() fails, so the server thread never
 * enters its poll loop and the scenarios below have the fake to themselves.
 *
 * The accept loop and the multi-client event stream are therefore NOT reached
 * by this suite. That is a real limit and the PR says so - it is the dato that
 * decides whether the next tickets can use this harness or need another one.
 */
int reflow_fake_socket(int family, int type, int proto)
{
	ARG_UNUSED(family);
	ARG_UNUSED(type);
	ARG_UNUSED(proto);

	errno = EAFNOSUPPORT;
	return -1;
}
