/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * When the station link tries to associate again, and how far apart the
 * attempts are. Pure, no Zephyr, tested in tests/logic/ - the shape RFO-B13
 * (ui/buttonmap.c) and RFO-B17 (ui/selmap.c) established, next to
 * net/cmdparse.c and net/httpgate.c.
 *
 * Why it is separate from wifi.c (RFO-B16): wifi_thread() left its loop with a
 * `break` the moment the link came up once, so the thread that re-issues
 * NET_REQUEST_WIFI_CONNECT was already dead when the association dropped.
 * l4.c resets the ready semaphore on NET_EVENT_L4_DISCONNECTED - correctly -
 * and nobody was left to react to it. The web UI, and with it the remote Stop
 * button, came back only on a power cycle, with the oven mid-run.
 *
 * The file this module has no way to reach is the one that decides: this
 * target has never been compiled or run (CLAUDE.md, Status), and there is no
 * harness that builds an image with Wi-Fi. So the policy lives here, where a
 * test walks it end to end, and wifi.c keeps only what needs the kernel -
 * waiting, calling, sleeping.
 */

#ifndef REFLOW_WIFIPOLICY_H_
#define REFLOW_WIFIPOLICY_H_

#include <stdbool.h>
#include <stdint.h>

/*
 * How often the link is looked at while it IS up. It costs one semaphore check,
 * and it bounds how long a drop can go unnoticed - which is the first term of
 * the recovery time the original ticket asked for ("the web UI is back within
 * 60 s").
 */
#define REFLOW_WIFI_SUPERVISE_MS 5000U

/*
 * The wait after the FIRST attempt of a round, and the ceiling the backoff
 * climbs to. Both numbers are argued in the .c; the short version is that 15 s
 * is enough for WPA2 plus DHCP without paying half the recovery budget for an
 * access point that only blinked, and 30 s is the largest ceiling that still
 * fits the 60 s the original criterion asked for.
 */
#define REFLOW_WIFI_RETRY_MIN_MS 15000U
#define REFLOW_WIFI_RETRY_MAX_MS 30000U

enum reflow_wifi_action {
	/* Issue NET_REQUEST_WIFI_CONNECT now. */
	REFLOW_WIFI_CONNECT,
	/* The link is up: do nothing, just look again later. */
	REFLOW_WIFI_WATCH,
};

struct reflow_wifi_step {
	enum reflow_wifi_action action;
	/* How long to wait after doing it, before asking the policy again. */
	uint32_t wait_ms;
	/*
	 * The link has been up before, so any DHCPv4 lease in hand belongs to
	 * the previous association and may be for another subnet entirely.
	 * False on the very first attempt, when there is nothing to throw away.
	 */
	bool stale_lease;
};

struct reflow_wifi_policy {
	/* Connect attempts since the link was last up. */
	uint32_t attempts;
	/* The link has been up at least once. */
	bool was_up;
};

void reflow_wifi_policy_init(struct reflow_wifi_policy *p);

/*
 * One turn of the loop. `link_ready` is what the caller observes right now.
 * Returns what to do and how long to wait afterwards.
 */
struct reflow_wifi_step reflow_wifi_policy_step(struct reflow_wifi_policy *p,
						bool link_ready);

#endif /* REFLOW_WIFIPOLICY_H_ */
