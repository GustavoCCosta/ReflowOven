/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * USB Ethernet link (CDC ECM) for the web UI.
 * Optional: CONFIG_REFLOW_LINK_USB_ECM.
 *
 * The oven appears to the host as a USB Ethernet adapter. It owns a static
 * address on that link and runs a one-address DHCPv4 server, so the host
 * configures itself and the browser just needs the oven's fixed address.
 * httpd.c is untouched: it talks BSD sockets and does not know the link.
 *
 * This module initialises the USB device stack itself, registering *every*
 * class present in the devicetree - so a cdc_acm node for the shell ends up on
 * the same cable as the Ethernet function. That is also why
 * CONFIG_CDC_ACM_SERIAL_INITIALIZE_AT_BOOT must be off in this configuration:
 * a USB controller can host several device contexts, but only one at a time.
 */

#include <zephyr/device.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/net/net_if.h>
#include <zephyr/net/net_ip.h>
#include <zephyr/usb/usbd.h>

#ifdef CONFIG_NET_DHCPV4_SERVER
#include <zephyr/net/dhcpv4_server.h>
#endif

#include "net.h"

LOG_MODULE_REGISTER(reflow_usbnet, CONFIG_REFLOW_LOG_LEVEL);

#if !DT_HAS_COMPAT_STATUS_OKAY(zephyr_cdc_ecm_ethernet)
#error "USB web UI needs a zephyr,cdc-ecm-ethernet node (see overlays/)"
#endif

static const struct device *const ecm_dev =
	DEVICE_DT_GET_ANY(zephyr_cdc_ecm_ethernet);

USBD_DEVICE_DEFINE(reflow_usbd, DEVICE_DT_GET(DT_NODELABEL(zephyr_udc0)),
		   CONFIG_REFLOW_USB_VID, CONFIG_REFLOW_USB_PID);

USBD_DESC_LANG_DEFINE(reflow_usb_lang);
USBD_DESC_MANUFACTURER_DEFINE(reflow_usb_mfr, CONFIG_REFLOW_USB_MANUFACTURER);
USBD_DESC_PRODUCT_DEFINE(reflow_usb_product, CONFIG_REFLOW_USB_PRODUCT);
IF_ENABLED(CONFIG_HWINFO, (USBD_DESC_SERIAL_NUMBER_DEFINE(reflow_usb_sn)));
USBD_DESC_CONFIG_DEFINE(reflow_usb_fs_desc, "Reflow oven, full speed");

/* Bus powered; bMaxPower is in 2 mA units, so 125 means 250 mA. */
USBD_CONFIGURATION_DEFINE(reflow_usb_fs_config, 0, 125, &reflow_usb_fs_desc);

static void usbd_msg_cb(struct usbd_context *const ctx,
			const struct usbd_msg *const msg)
{
	LOG_DBG("USB: %s", usbd_msg_type_string(msg->type));

	if (!usbd_can_detect_vbus(ctx)) {
		return;
	}

	if (msg->type == USBD_MSG_VBUS_READY && usbd_enable(ctx)) {
		LOG_ERR("failed to enable USB device on VBUS");
	}

	if (msg->type == USBD_MSG_VBUS_REMOVED && usbd_disable(ctx)) {
		LOG_ERR("failed to disable USB device on VBUS removal");
	}
}

static int usb_stack_up(void)
{
	int ret;

	ret = usbd_add_descriptor(&reflow_usbd, &reflow_usb_lang);
	if (ret != 0) {
		LOG_ERR("language descriptor: %d", ret);
		return ret;
	}

	ret = usbd_add_descriptor(&reflow_usbd, &reflow_usb_mfr);
	if (ret != 0) {
		LOG_ERR("manufacturer descriptor: %d", ret);
		return ret;
	}

	ret = usbd_add_descriptor(&reflow_usbd, &reflow_usb_product);
	if (ret != 0) {
		LOG_ERR("product descriptor: %d", ret);
		return ret;
	}

	IF_ENABLED(CONFIG_HWINFO, (
		ret = usbd_add_descriptor(&reflow_usbd, &reflow_usb_sn);
		if (ret != 0) {
			LOG_WRN("serial number descriptor: %d", ret);
		}
	))

	ret = usbd_add_configuration(&reflow_usbd, USBD_SPEED_FS,
				     &reflow_usb_fs_config);
	if (ret != 0) {
		LOG_ERR("add configuration: %d", ret);
		return ret;
	}

	/* Registers the Ethernet function and, if present, the shell's CDC ACM. */
	ret = usbd_register_all_classes(&reflow_usbd, USBD_SPEED_FS, 1, NULL);
	if (ret != 0) {
		LOG_ERR("register classes: %d", ret);
		return ret;
	}

	ret = usbd_msg_register_cb(&reflow_usbd, usbd_msg_cb);
	if (ret != 0) {
		LOG_WRN("message callback: %d", ret);
	}

	ret = usbd_init(&reflow_usbd);
	if (ret != 0) {
		LOG_ERR("usbd_init: %d", ret);
		return ret;
	}

	if (!usbd_can_detect_vbus(&reflow_usbd)) {
		ret = usbd_enable(&reflow_usbd);
		if (ret != 0) {
			LOG_ERR("usbd_enable: %d", ret);
			return ret;
		}
	}

	return 0;
}

static int ipv4_up(struct net_if *iface)
{
	struct in_addr addr, netmask;

	if (net_addr_pton(AF_INET, CONFIG_REFLOW_USB_IPV4_ADDR, &addr) != 0) {
		LOG_ERR("bad REFLOW_USB_IPV4_ADDR '%s'", CONFIG_REFLOW_USB_IPV4_ADDR);
		return -EINVAL;
	}

	if (net_addr_pton(AF_INET, CONFIG_REFLOW_USB_IPV4_MASK, &netmask) != 0) {
		LOG_ERR("bad REFLOW_USB_IPV4_MASK '%s'", CONFIG_REFLOW_USB_IPV4_MASK);
		return -EINVAL;
	}

	if (net_if_ipv4_addr_add(iface, &addr, NET_ADDR_MANUAL, 0) == NULL) {
		LOG_ERR("could not set the oven address");
		return -EADDRNOTAVAIL;
	}

	net_if_ipv4_set_netmask_by_addr(iface, &addr, &netmask);
	LOG_INF("oven address %s", CONFIG_REFLOW_USB_IPV4_ADDR);

#ifdef CONFIG_NET_DHCPV4_SERVER
	{
		struct in_addr pool;
		int ret;

		if (net_addr_pton(AF_INET, CONFIG_REFLOW_USB_DHCP_POOL_START,
				  &pool) != 0) {
			LOG_ERR("bad REFLOW_USB_DHCP_POOL_START");
			return -EINVAL;
		}

		ret = net_dhcpv4_server_start(iface, &pool);
		if (ret != 0) {
			/* Not fatal: the host can still be configured by hand. */
			LOG_WRN("DHCPv4 server: %d (configure the host manually)", ret);
		} else {
			LOG_INF("serving host addresses from %s",
				CONFIG_REFLOW_USB_DHCP_POOL_START);
		}
	}
#endif

	return 0;
}

static void usb_net_thread(void *a, void *b, void *c)
{
	struct net_if *iface;

	ARG_UNUSED(a);
	ARG_UNUSED(b);
	ARG_UNUSED(c);

	if (!device_is_ready(ecm_dev)) {
		LOG_ERR("CDC ECM device not ready");
		return;
	}

	iface = net_if_lookup_by_dev(ecm_dev);
	if (iface == NULL) {
		LOG_ERR("no network interface for %s", ecm_dev->name);
		return;
	}

	if (ipv4_up(iface) != 0) {
		return;
	}

	if (usb_stack_up() != 0) {
		return;
	}

	LOG_INF("USB Ethernet ready; plug the cable and open the oven address");

	/*
	 * Carrier only comes up once the host selects the data interface, and
	 * that is what releases the web server (it waits on L4). Nothing left
	 * to do here.
	 */
}

K_THREAD_DEFINE(reflow_usbnet_tid, 2048, usb_net_thread, NULL, NULL, NULL,
		7, 0, 300);
