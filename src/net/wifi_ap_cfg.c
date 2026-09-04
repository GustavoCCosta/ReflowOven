/* SPDX-License-Identifier: Apache-2.0 */

#include <stddef.h>

#include "wifi_ap_cfg.h"

/* IEEE 802.11: SSID is 1..32 octets, WPA2-PSK passphrase is 8..63 ASCII. */
#define SSID_MAX 32U
#define PSK_MIN  8U
#define PSK_MAX  63U

static size_t len_of(const char *s)
{
	size_t n = 0;

	if (s == NULL) {
		return 0;
	}
	while (s[n] != '\0') {
		n++;
	}
	return n;
}

enum reflow_ap_cfg reflow_wifi_ap_check(const char *ssid, const char *psk)
{
	size_t ssid_len = len_of(ssid);
	size_t psk_len = len_of(psk);

	if (ssid_len == 0U) {
		return REFLOW_AP_CFG_NO_SSID;
	}
	if (ssid_len > SSID_MAX) {
		return REFLOW_AP_CFG_SSID_TOO_LONG;
	}

	/*
	 * The empty passphrase is reported as its own case rather than folded
	 * into "too short": it is the shipped default, so the boot log has to
	 * be able to say "nobody set one" instead of "yours is invalid".
	 */
	if (psk_len == 0U) {
		return REFLOW_AP_CFG_OPEN;
	}
	if (psk_len < PSK_MIN) {
		return REFLOW_AP_CFG_PSK_TOO_SHORT;
	}
	if (psk_len > PSK_MAX) {
		return REFLOW_AP_CFG_PSK_TOO_LONG;
	}

	return REFLOW_AP_CFG_OK;
}

bool reflow_wifi_ap_usable(enum reflow_ap_cfg cfg)
{
	return cfg == REFLOW_AP_CFG_OK;
}

const char *reflow_wifi_ap_cfg_str(enum reflow_ap_cfg cfg)
{
	switch (cfg) {
	case REFLOW_AP_CFG_OK:            return "ok";
	case REFLOW_AP_CFG_NO_SSID:       return "no SSID set";
	case REFLOW_AP_CFG_SSID_TOO_LONG: return "SSID longer than 32 characters";
	case REFLOW_AP_CFG_OPEN:          return "no passphrase set";
	case REFLOW_AP_CFG_PSK_TOO_SHORT: return "passphrase shorter than 8 characters";
	case REFLOW_AP_CFG_PSK_TOO_LONG:  return "passphrase longer than 63 characters";
	default:                          return "unknown";
	}
}
