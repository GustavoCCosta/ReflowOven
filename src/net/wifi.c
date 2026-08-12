/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Station-mode Wi-Fi link for the web UI.
 * Optional: CONFIG_REFLOW_LINK_WIFI.
 *
 * This module only brings the link up. Whether the network is usable is
 * decided in l4.c, which both link modules share, and the web server waits
 * there. Credentials come from Kconfig; move them to settings/NVS before
 * shipping.
 */

#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/net/net_if.h>
#include <zephyr/net/wifi_mgmt.h>
#ifdef CONFIG_NET_DHCPV4
#include <zephyr/net/dhcpv4.h>
#endif

#include "net.h"

LOG_MODULE_REGISTER(reflow_wifi, CONFIG_REFLOW_LOG_LEVEL);

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
}

K_THREAD_DEFINE(reflow_wifi_tid, 2560, wifi_thread, NULL, NULL, NULL, 7, 0, 300);
