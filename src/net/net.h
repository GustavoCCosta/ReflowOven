/* SPDX-License-Identifier: Apache-2.0 */

#ifndef REFLOW_NET_H_
#define REFLOW_NET_H_

#include <zephyr/kernel.h>

/* 0 once the stack has an address, -EAGAIN on timeout. */
int reflow_net_wait_ready(k_timeout_t timeout);

#endif /* REFLOW_NET_H_ */
