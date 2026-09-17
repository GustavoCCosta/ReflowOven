/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * A socket the test drives, in place of the one the kernel provides.
 *
 * RFO-G42. httpd.c is the file with the longest list of closed defects in this
 * project - B07, B08, B09, B14, B15/G32, B30, B33, B43 - and every one of them
 * was found by reading the code, none by a test that failed first. What stood
 * in the way is that the interesting decisions live between the socket calls,
 * and a real socket cannot be asked to stall on command: tests/httpwait talks
 * over the loopback driver, which is faithful but gives the test no say over
 * when a peer stops reading.
 *
 * So the substitution is at the call, not at the link. main.c includes
 * <zephyr/net/socket.h> FIRST - the real declarations go in untouched - and
 * only then this header, so the names below redirect httpd.c's CALL SITES and
 * nothing else. httpd.c is compiled exactly as it is flashed, with no #ifdef
 * for the test and no change to the file. The one thing faked is the kernel on
 * the other side of the call.
 *
 * Deliberately not a general socket emulator: it answers the calls httpd.c
 * makes, in the shapes this suite needs, and nothing more.
 */

#ifndef REFLOW_SOCKET_FAKE_H_
#define REFLOW_SOCKET_FAKE_H_

#include <stddef.h>
#include <sys/types.h>

/* What the peer on the other end does with what the server sends it. */
enum reflow_fake_peer {
	/* Reads everything immediately: the healthy client. */
	REFLOW_FAKE_PEER_FAST,
	/*
	 * Stopped reading. Every send answers EAGAIN after a slice, which is
	 * what a real socket carrying SO_SNDTIMEO does once the peer's receive
	 * window is full. This is the RFO-B07 client: one browser tab on a bad
	 * Wi-Fi, nothing malicious needed.
	 */
	REFLOW_FAKE_PEER_STALLED,
	/*
	 * Reads, but barely: one byte per slice. The transfer takes far longer
	 * than the budget, and the server must NOT drop it - the budget
	 * measures time without progress, not time on the connection. This is
	 * the case that tells a correct fix from one that just counts seconds.
	 */
	REFLOW_FAKE_PEER_TRICKLE,
};

/* Hand out a descriptor whose peer behaves this way. */
int reflow_fake_open(enum reflow_fake_peer peer);

/* Bytes this descriptor has actually accepted since it was opened. */
size_t reflow_fake_accepted(int fd);

/* True once the server closed it. */
bool reflow_fake_closed(int fd);

/*
 * How long a send that cannot make progress blocks before answering EAGAIN.
 * It mirrors the SO_SNDTIMEO httpd.c sets on every accepted socket: without
 * it the server's retry loop would spin at the speed of the host and the
 * budget would expire in a number of iterations nobody can reason about.
 */
#define REFLOW_FAKE_SLICE_MS 100

/* Forget every descriptor. Called between scenarios. */
void reflow_fake_reset(void);

ssize_t reflow_fake_send(int sock, const void *buf, size_t len, int flags);
ssize_t reflow_fake_recv(int sock, void *buf, size_t max_len, int flags);
int reflow_fake_close(int sock);
int reflow_fake_socket(int family, int type, int proto);

/*
 * The redirection itself. Below this line, inside httpd.c, these names mean
 * the functions above.
 */
#define zsock_send   reflow_fake_send
#define zsock_recv   reflow_fake_recv
#define zsock_close  reflow_fake_close
#define zsock_socket reflow_fake_socket

#endif /* REFLOW_SOCKET_FAKE_H_ */
