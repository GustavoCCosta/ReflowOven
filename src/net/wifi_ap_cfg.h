/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * "Esta configuracao de ponto de acesso pode subir?" - a decisao que precede
 * ligar o radio, como funcao pura das duas strings.
 *
 * Kept apart from wifi_ap.c, and free of any Zephyr dependency, for the same
 * reason cmdparse.c and httpgate.c are: the module that talks to the radio is
 * a shell, and the judgement it makes can be exercised with nothing but
 * strings (RFO-F07).
 *
 * Why this is a safety decision and not configuration hygiene: an access point
 * puts START and STOP of a mains heater on the air, in reach of every device
 * within radio range. The USB link needed physical access to the cable; this
 * one needs nothing. What the oven refuses to broadcast is therefore part of
 * the safety surface, and it is decided here, once, where a test can see it.
 *
 * The limits are WPA2-PSK's own, not house rules: IEEE 802.11i takes an ASCII
 * passphrase of 8 to 63 characters, and an SSID of 1 to 32 octets. A
 * passphrase outside that range is not "weak", it is rejected by the radio -
 * and finding that out from a log line after the oven is closed up is worse
 * than finding it out at boot.
 */

#ifndef REFLOW_WIFI_AP_CFG_H_
#define REFLOW_WIFI_AP_CFG_H_

#include <stdbool.h>

enum reflow_ap_cfg {
	/* Usable: SSID within 1..32, passphrase within 8..63. */
	REFLOW_AP_CFG_OK = 0,
	/* No SSID at all: nothing to broadcast. */
	REFLOW_AP_CFG_NO_SSID,
	/* SSID longer than 32 octets. */
	REFLOW_AP_CFG_SSID_TOO_LONG,
	/*
	 * Empty passphrase. This is the shipped default, on purpose, and it is
	 * NOT usable: see reflow_wifi_ap_usable().
	 */
	REFLOW_AP_CFG_OPEN,
	/* Passphrase under 8 characters: WPA2 will not take it. */
	REFLOW_AP_CFG_PSK_TOO_SHORT,
	/* Passphrase over 63 characters: WPA2 will not take it. */
	REFLOW_AP_CFG_PSK_TOO_LONG,
};

/* Classify the pair. NULL is treated as empty. */
enum reflow_ap_cfg reflow_wifi_ap_check(const char *ssid, const char *psk);

/*
 * Whether the radio may be brought up with this configuration.
 *
 * Only REFLOW_AP_CFG_OK is usable, and the case that matters is
 * REFLOW_AP_CFG_OPEN: an oven with no passphrase set does NOT come up as an
 * open network. That is a deliberate choice over the alternative of warning
 * once per boot and broadcasting anyway.
 *
 * The reason is the same one that makes an empty CONFIG_REFLOW_NET_TOKEN
 * answer 503 to every command instead of accepting anonymous ones: the
 * shipped default has to be the safe end of the trade, and "nobody set a
 * passphrase" is exactly the state a freshly flashed oven is in. An open AP
 * cannot be made safe by the token gate either - the gate protects commands,
 * while the network itself would be joinable by anyone in range, which is
 * already an invitation to try.
 *
 * The cost is real and is the point: a build with no passphrase has no web UI.
 * It is announced at boot, at error level, with the symbol to set.
 */
bool reflow_wifi_ap_usable(enum reflow_ap_cfg cfg);

/* Human readable reason, for the boot log. Never NULL. */
const char *reflow_wifi_ap_cfg_str(enum reflow_ap_cfg cfg);

#endif /* REFLOW_WIFI_AP_CFG_H_ */
