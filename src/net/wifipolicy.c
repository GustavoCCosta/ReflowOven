/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * RFO-B16. The four decisions the ticket asked to be made and justified, with
 * the reasoning kept next to the numbers so the next person can disagree with
 * the argument instead of guessing at the constant.
 *
 * 1. WHEN to try again. On every turn where the link is not ready - including
 *    the turn right after it went down. There is no "give up once it worked",
 *    which is exactly the `break` this ticket removes.
 *
 * 2. HOW FAR APART. REFLOW_WIFI_RETRY_MIN_MS after the first attempt of a
 *    round, doubling to REFLOW_WIFI_RETRY_MAX_MS, then flat.
 *
 *    The ceiling is not a taste: the original acceptance criterion is that the
 *    web UI is back within 60 s of the access point returning. The worst case
 *    is the AP coming back one instant after an attempt failed, so recovery
 *    costs one whole wait plus one association. A 30 s ceiling leaves room for
 *    the association inside 60 s; anything larger breaks the requirement the
 *    ticket was written around.
 *
 *    The floor is the other half. Re-issuing connect every second would hammer
 *    a driver that is already trying, and an AP that is rebooting takes tens of
 *    seconds anyway. But paying the full 30 s for an access point that only
 *    blinked spends half the budget on nothing, so the first wait is 15 s -
 *    long enough for WPA2 plus DHCP, short enough that a quick recovery is
 *    quick. NOT measured on hardware: nobody here has this target running, and
 *    that is said out loud rather than dressed up as a datasheet number.
 *
 * 3. A CAP ON ATTEMPTS: none, deliberately. Giving up means the operator loses
 *    the remote Stop button until someone power-cycles an oven that is heating
 *    - which is the defect, not the fix. The cost of insisting for ever is one
 *    net_mgmt() call every 30 s, and that buys nothing back.
 *
 * 4. DHCP. Restarted on every reassociation, not only the first. A lease
 *    obtained before the link dropped belongs to the previous association; the
 *    AP may have rebooted with a different pool, or it may be a different AP
 *    with the same SSID. Keeping the old address there produces a board that
 *    is associated and unreachable - the failure this ticket exists to remove,
 *    wearing a different hat. The first attempt has nothing to throw away, so
 *    it is flagged separately and the caller simply starts the client.
 */

#include "wifipolicy.h"

void reflow_wifi_policy_init(struct reflow_wifi_policy *p)
{
	p->attempts = 0U;
	p->was_up = false;
}

/*
 * Doubling from the floor, clamped at the ceiling. Written as a loop rather
 * than a shift so that a large `attempts` cannot overflow into a small wait -
 * this counter is never reset while the AP is away, so on a link that has been
 * down for a week it is a big number, and a `1 << attempts` would wrap and hand
 * back a tiny delay at the worst possible moment.
 */
static uint32_t backoff_ms(uint32_t attempts)
{
	uint32_t ms = REFLOW_WIFI_RETRY_MIN_MS;
	uint32_t i;

	for (i = 1U; i < attempts; i++) {
		if (ms >= REFLOW_WIFI_RETRY_MAX_MS) {
			break;
		}
		ms *= 2U;
	}

	return ms > REFLOW_WIFI_RETRY_MAX_MS ? REFLOW_WIFI_RETRY_MAX_MS : ms;
}

struct reflow_wifi_step reflow_wifi_policy_step(struct reflow_wifi_policy *p,
						bool link_ready)
{
	struct reflow_wifi_step step;

	if (link_ready) {
		/*
		 * Up. Remember it - that is what makes the next round a
		 * REassociation - and clear the attempt count so a later drop
		 * starts from the short wait instead of inheriting the ceiling
		 * from whatever happened at boot.
		 */
		p->was_up = true;
		p->attempts = 0U;

		step.action = REFLOW_WIFI_WATCH;
		step.wait_ms = REFLOW_WIFI_SUPERVISE_MS;
		step.stale_lease = false;

		return step;
	}

	p->attempts++;

	step.action = REFLOW_WIFI_CONNECT;
	step.wait_ms = backoff_ms(p->attempts);
	step.stale_lease = p->was_up;

	return step;
}
