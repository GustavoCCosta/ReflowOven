/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Wi-Fi access point link for the web UI.
 * Optional: CONFIG_REFLOW_LINK_WIFI_AP.
 *
 * The oven IS the network: it brings up a SoftAP, owns a static address on it
 * and runs a DHCPv4 server, so a phone or a laptop joins the oven's SSID and
 * opens its fixed address. httpd.c is untouched - it talks BSD sockets and
 * does not know the link.
 *
 * This is the same topology usb_net.c already implements over the cable
 * (static address plus a DHCPv4 server), with the medium swapped from USB to
 * radio. It is deliberately a THIRD file rather than a mode inside wifi.c:
 * station and access point are different lifecycles - one joins and retries,
 * the other starts and serves - and merging them behind an `if` is how the
 * Kconfig choice stops being removable.
 *
 * What releases the web server is l4.c, which knows nothing about any of this:
 * it waits for NET_EVENT_L4_CONNECTED. See the comment above ap_up() for what
 * makes that event arrive on an interface that never "connects" anywhere.
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/net/net_if.h>
#include <zephyr/net/net_ip.h>
#include <zephyr/net/wifi.h>
#include <zephyr/net/wifi_mgmt.h>

#ifdef CONFIG_NET_DHCPV4_SERVER
#include <zephyr/net/dhcpv4_server.h>
#endif

#include "net.h"
#include "wifi_ap_cfg.h"

LOG_MODULE_REGISTER(reflow_wifi_ap, CONFIG_REFLOW_LOG_LEVEL);

static struct net_if *ap_iface(void)
{
	/*
	 * Two shapes, one code path. With CONFIG_ESP32_WIFI_AP_STA_MODE the
	 * driver registers a second, SoftAP-flagged interface and this returns
	 * it; without it - which is this link's default, since the oven has no
	 * use for station mode at the same time - there is a single Wi-Fi
	 * interface that serves as the AP, and net_if_get_wifi_sap() has
	 * nothing to return.
	 */
	struct net_if *iface = net_if_get_wifi_sap();

	if (iface != NULL) {
		return iface;
	}
	return net_if_get_first_wifi();
}

static int ipv4_up(struct net_if *iface)
{
	struct in_addr addr, netmask;

	if (net_addr_pton(AF_INET, CONFIG_REFLOW_AP_IPV4_ADDR, &addr) != 0) {
		LOG_ERR("bad REFLOW_AP_IPV4_ADDR '%s'", CONFIG_REFLOW_AP_IPV4_ADDR);
		return -EINVAL;
	}

	if (net_addr_pton(AF_INET, CONFIG_REFLOW_AP_IPV4_MASK, &netmask) != 0) {
		LOG_ERR("bad REFLOW_AP_IPV4_MASK '%s'", CONFIG_REFLOW_AP_IPV4_MASK);
		return -EINVAL;
	}

	if (net_if_ipv4_addr_add(iface, &addr, NET_ADDR_MANUAL, 0) == NULL) {
		LOG_ERR("could not set the oven address");
		return -EADDRNOTAVAIL;
	}

	net_if_ipv4_set_netmask_by_addr(iface, &addr, &netmask);

#ifdef CONFIG_NET_DHCPV4_SERVER
	{
		struct in_addr pool;
		int ret;

		if (net_addr_pton(AF_INET, CONFIG_REFLOW_AP_DHCP_POOL_START,
				  &pool) != 0) {
			LOG_ERR("bad REFLOW_AP_DHCP_POOL_START");
			return -EINVAL;
		}

		ret = net_dhcpv4_server_start(iface, &pool);
		if (ret != 0) {
			/* Not fatal: a client can still be configured by hand. */
			LOG_WRN("DHCPv4 server: %d (configure the client manually)",
				ret);
		}
	}
#endif

	return 0;
}

/*
 * Bring the radio up as an access point.
 *
 * On the event l4.c is waiting for: an AP interface never "connects" to
 * anything, so NET_EVENT_L4_CONNECTED could plausibly never arrive and leave
 * the server parked for ever. It does arrive, and not by luck -
 * conn_mgr_monitor raises L4_CONNECTED for an interface that is admin up, not
 * dormant, and has an address; the Espressif driver takes the AP interface out
 * of dormant on WIFI_EVENT_AP_START (esp_wifi_drv.c, the AP_START case calls
 * net_if_dormant_off()), and the static address above supplies the rest. So
 * the sequence is: address set, AP started, dormant cleared, monitor notifies,
 * httpd.c stops waiting.
 *
 * That is why l4.c needs no change and must not get one: it stays a
 * link-agnostic "the network is usable" gate, and this file does not know
 * about it either.
 */
static int ap_up(struct net_if *iface)
{
	struct wifi_connect_req_params params = {
		.ssid = (const uint8_t *)CONFIG_REFLOW_AP_SSID,
		.ssid_length = sizeof(CONFIG_REFLOW_AP_SSID) - 1,
		.psk = (const uint8_t *)CONFIG_REFLOW_AP_PSK,
		.psk_length = sizeof(CONFIG_REFLOW_AP_PSK) - 1,
		.security = WIFI_SECURITY_TYPE_PSK,
		.channel = WIFI_CHANNEL_ANY,
		.band = WIFI_FREQ_BAND_2_4_GHZ,
		.mfp = WIFI_MFP_OPTIONAL,
	};

	return net_mgmt(NET_REQUEST_WIFI_AP_ENABLE, iface, &params, sizeof(params));
}

static void wifi_ap_thread(void *a, void *b, void *c)
{
	enum reflow_ap_cfg cfg;
	struct net_if *iface;
	int ret;

	ARG_UNUSED(a);
	ARG_UNUSED(b);
	ARG_UNUSED(c);

	/*
	 * The refusal comes FIRST, before the interface is even looked up.
	 * Nothing about a radio can be undone once it is broadcasting, so the
	 * order here is the whole safety property of this file: an oven with no
	 * passphrase set never puts a joinable network on the air, and says so
	 * at error level with the symbol to fix.
	 */
	cfg = reflow_wifi_ap_check(CONFIG_REFLOW_AP_SSID, CONFIG_REFLOW_AP_PSK);
	if (!reflow_wifi_ap_usable(cfg)) {
		LOG_ERR("access point NOT started: %s", reflow_wifi_ap_cfg_str(cfg));
		LOG_ERR("set CONFIG_REFLOW_AP_SSID and CONFIG_REFLOW_AP_PSK "
			"(8 to 63 characters); the web UI stays off until then");
		return;
	}

	iface = ap_iface();
	if (iface == NULL) {
		LOG_ERR("no Wi-Fi interface: is the radio driver in this build?");
		return;
	}

	if (ipv4_up(iface) != 0) {
		return;
	}

	ret = ap_up(iface);
	if (ret != 0) {
		LOG_ERR("could not start the access point: %d", ret);
		return;
	}

	LOG_INF("access point '%s' up; join it and open http://%s:%d/",
		CONFIG_REFLOW_AP_SSID, CONFIG_REFLOW_AP_IPV4_ADDR,
		CONFIG_REFLOW_NET_HTTP_PORT);

	if (CONFIG_REFLOW_NET_TOKEN[0] == '\0') {
		/*
		 * Deliberately repeated here, and at error level, even though
		 * httpd.c logs its own warning: on this link the page is
		 * reachable by anyone within radio range, so "commands are
		 * disabled" is the difference between an oven that only tells
		 * and an oven that also obeys.
		 */
		LOG_ERR("no CONFIG_REFLOW_NET_TOKEN: remote commands are DISABLED "
			"on this access point (POST /api/cmd answers 503)");
	}
}

K_THREAD_DEFINE(reflow_wifi_ap_tid, 2560, wifi_ap_thread, NULL, NULL, NULL,
		7, 0, 300);
