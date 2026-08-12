/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Link-agnostic "the network is usable" gate.
 *
 * The web UI does not care how it got connectivity: it waits here, and either
 * link module (Wi-Fi station or USB CDC ECM) satisfies it by bringing an
 * interface up with an address. Keeping this out of the link modules is what
 * lets the link be swapped by a Kconfig choice.
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/net/net_event.h>
#include <zephyr/net/net_if.h>
#include <zephyr/net/net_ip.h>
#include <zephyr/net/net_mgmt.h>

#include "net.h"

LOG_MODULE_REGISTER(reflow_l4, CONFIG_REFLOW_LOG_LEVEL);

static K_SEM_DEFINE(l4_ready, 0, 1);
static struct net_mgmt_event_callback mgmt_cb;

static void log_url(struct net_if *iface)
{
	char buf[NET_IPV4_ADDR_LEN];
	struct in_addr *addr;

	if (iface == NULL) {
		return;
	}

	addr = net_if_ipv4_get_global_addr(iface, NET_ADDR_PREFERRED);
	if (addr != NULL) {
		LOG_INF("web UI at http://%s:%d/",
			net_addr_ntop(AF_INET, addr, buf, sizeof(buf)),
			CONFIG_REFLOW_NET_HTTP_PORT);
	} else {
		LOG_INF("network up, web UI on port %d (address pending)",
			CONFIG_REFLOW_NET_HTTP_PORT);
	}
}

/*
 * The event mask is 64 bit wide (net_mgmt_event_handler_t): declaring it as
 * uint32_t truncates the NET_EVENT_L4_* constants and no case ever matches.
 */
static void mgmt_handler(struct net_mgmt_event_callback *cb, uint64_t event,
			 struct net_if *iface)
{
	ARG_UNUSED(cb);

	switch (event) {
	case NET_EVENT_L4_CONNECTED:
		k_sem_give(&l4_ready);
		log_url(iface);
		break;
	case NET_EVENT_L4_DISCONNECTED:
		LOG_WRN("network down");
		k_sem_reset(&l4_ready);
		break;
	default:
		break;
	}
}

int reflow_net_wait_ready(k_timeout_t timeout)
{
	int ret = k_sem_take(&l4_ready, timeout);

	if (ret == 0) {
		/* Leave it signalled: several modules may wait on it. */
		k_sem_give(&l4_ready);
	}

	return ret;
}

static int l4_init(void)
{
	net_mgmt_init_event_callback(&mgmt_cb, mgmt_handler,
				     NET_EVENT_L4_CONNECTED | NET_EVENT_L4_DISCONNECTED);
	net_mgmt_add_event_callback(&mgmt_cb);

	return 0;
}

/* Must be registered before any link module thread starts. */
SYS_INIT(l4_init, APPLICATION, 0);
