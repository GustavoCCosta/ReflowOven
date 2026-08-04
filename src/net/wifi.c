/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Station-mode Wi-Fi bring-up. Optional: CONFIG_REFLOW_NET.
 * Credentials come from Kconfig; move them to settings/NVS before shipping.
 */

#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/net/net_event.h>
#include <zephyr/net/net_if.h>
#include <zephyr/net/net_ip.h>
#include <zephyr/net/net_mgmt.h>
#include <zephyr/net/wifi_mgmt.h>
#ifdef CONFIG_NET_DHCPV4
#include <zephyr/net/dhcpv4.h>
#endif

#include "net.h"

LOG_MODULE_REGISTER(reflow_wifi, CONFIG_REFLOW_LOG_LEVEL);

static K_SEM_DEFINE(l4_ready, 0, 1);
static struct net_mgmt_event_callback mgmt_cb;

static void mgmt_handler(struct net_mgmt_event_callback *cb, uint32_t event,
			 struct net_if *iface)
{
	ARG_UNUSED(cb);
	ARG_UNUSED(iface);

	switch (event) {
	case NET_EVENT_L4_CONNECTED:
		LOG_INF("network connected");
		k_sem_give(&l4_ready);
		break;
	case NET_EVENT_L4_DISCONNECTED:
		LOG_WRN("network disconnected");
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
		/* Keep the semaphore signalled for other waiters. */
		k_sem_give(&l4_ready);
	}
	return ret;
}

static int wifi_connect(struct net_if *iface)
{
	static struct wifi_connect_req_params params;

	params.ssid = (const uint8_t *)CONFIG_REFLOW_WIFI_SSID;
	params.ssid_length = strlen(CONFIG_REFLOW_WIFI_SSID);
	params.psk = (const uint8_t *)CONFIG_REFLOW_WIFI_PSK;
	params.psk_length = strlen(CONFIG_REFLOW_WIFI_PSK);
	params.security = params.psk_length > 0 ? WIFI_SECURITY_TYPE_PSK
					       : WIFI_SECURITY_TYPE_NONE;
	params.channel = WIFI_CHANNEL_ANY;
	params.band = WIFI_FREQ_BAND_2_4_GHZ;
	params.mfp = WIFI_MFP_OPTIONAL;
	params.timeout = SYS_FOREVER_MS;

	return net_mgmt(NET_REQUEST_WIFI_CONNECT, iface, &params, sizeof(params));
}

static void wifi_thread(void *a, void *b, void *c)
{
	struct net_if *iface;

	ARG_UNUSED(a);
	ARG_UNUSED(b);
	ARG_UNUSED(c);

	net_mgmt_init_event_callback(&mgmt_cb, mgmt_handler,
				     NET_EVENT_L4_CONNECTED | NET_EVENT_L4_DISCONNECTED);
	net_mgmt_add_event_callback(&mgmt_cb);

	iface = net_if_get_first_wifi();
	if (iface == NULL) {
		iface = net_if_get_default();
	}
	if (iface == NULL) {
		LOG_ERR("no network interface");
		return;
	}

	while (reflow_net_wait_ready(K_NO_WAIT) != 0) {
		int ret = wifi_connect(iface);

		if (ret != 0) {
			LOG_ERR("connect request failed: %d", ret);
		} else {
			LOG_INF("associating with '%s'", CONFIG_REFLOW_WIFI_SSID);
#ifdef CONFIG_NET_DHCPV4
			net_dhcpv4_start(iface);
#endif
		}

		if (reflow_net_wait_ready(K_SECONDS(30)) == 0) {
			break;
		}
		LOG_WRN("no link yet, retrying");
	}

	{
		char buf[NET_IPV4_ADDR_LEN];
		struct in_addr *addr;

		addr = net_if_ipv4_get_global_addr(iface, NET_ADDR_PREFERRED);
		if (addr != NULL) {
			LOG_INF("web UI at http://%s:%d/",
				net_addr_ntop(AF_INET, addr, buf, sizeof(buf)),
				CONFIG_REFLOW_NET_HTTP_PORT);
		} else {
			LOG_INF("web UI on port %d (address pending)",
				CONFIG_REFLOW_NET_HTTP_PORT);
		}
	}
}

K_THREAD_DEFINE(reflow_wifi_tid, 2560, wifi_thread, NULL, NULL, NULL, 7, 0, 300);
