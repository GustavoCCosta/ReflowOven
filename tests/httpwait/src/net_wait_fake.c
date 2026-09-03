/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * See net_wait_fake.h.
 */

#include <errno.h>

#include <zephyr/kernel.h>
#include <zephyr/net/net_if.h>
#include <zephyr/net/net_ip.h>

#include "net.h"
#include "net_wait_fake.h"

/*
 * A failed wait has to cost time, or the loop under test spins as fast as the
 * scheduler allows and the image looks broken for the wrong reason. It must
 * not cost the real CONFIG-free K_MINUTES(5) either, or the suite would take a
 * quarter of an hour to prove a loop.
 *
 * So the timeout the caller asks for is deliberately ignored and compressed to
 * this. What the test measures is how many times the caller comes back, not
 * how long Zephyr's semaphore takes to expire - k_sem_take() with a timeout is
 * not the code under test.
 */
#define FAKE_TIMEOUT_MS 200

static atomic_t calls;

/*
 * What net.h promises the caller: "0 once the stack has an address". l4.c can
 * only return 0 after NET_EVENT_L4_CONNECTED, which by construction means an
 * interface that is up with an address on it.
 *
 * The fake has to honour the same promise or it hands httpd.c a link that does
 * not exist yet: zsock_bind() on the wildcard address resolves through the
 * default interface, and before that interface is up the bind fails with
 * EADDRNOTAVAIL. That is not the behaviour under test, it is the fake lying,
 * and it showed up as a platform difference - on native_sim the image reaches
 * the bind far sooner in simulated time than on qemu_x86.
 */
static bool stack_has_an_address(void)
{
	static const struct net_in_addr loopback = NET_INADDR_LOOPBACK_INIT;
	struct net_if *iface = NULL;

	if (net_if_ipv4_addr_lookup(&loopback, &iface) == NULL) {
		return false;
	}

	return iface != NULL && net_if_is_up(iface);
}

int reflow_net_wait_ready(k_timeout_t timeout)
{
	atomic_val_t n = atomic_inc(&calls);

	ARG_UNUSED(timeout);

	if (n < REFLOW_NET_WAIT_FAKE_FAILURES || !stack_has_an_address()) {
		k_sleep(K_MSEC(FAKE_TIMEOUT_MS));
		return -EAGAIN;
	}

	return 0;
}

uint32_t reflow_net_wait_fake_calls(void)
{
	return (uint32_t)atomic_get(&calls);
}

bool reflow_net_wait_fake_stack_ready(void)
{
	return stack_has_an_address();
}
