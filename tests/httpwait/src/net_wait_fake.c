/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * See net_wait_fake.h.
 */

#include <errno.h>

#include <zephyr/kernel.h>

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

int reflow_net_wait_ready(k_timeout_t timeout)
{
	atomic_val_t n = atomic_inc(&calls);

	ARG_UNUSED(timeout);

	if (n < REFLOW_NET_WAIT_FAKE_FAILURES) {
		k_sleep(K_MSEC(FAKE_TIMEOUT_MS));
		return -EAGAIN;
	}

	return 0;
}

uint32_t reflow_net_wait_fake_calls(void)
{
	return (uint32_t)atomic_get(&calls);
}
