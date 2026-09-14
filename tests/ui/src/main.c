/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * RFO-B12: an unavailable panel must not cost the publish timeout.
 *
 * ZBUS_SUBSCRIBER_DEFINE + ZBUS_CHAN_ADD_OBS attach the observer at COMPILE
 * time, independently of the thread. When device_is_ready() fails the display
 * thread returns - and before this patch the observer stayed attached, enabled,
 * with a queue of 4 and nobody calling zbus_sub_wait(). From the fifth publish
 * on, controller.c's zbus_chan_pub(..., K_MSEC(20)) paid the whole 20 ms, every
 * 500 ms and on every state or fault transition. With a 100 ms control period
 * that is 20 % of overrun - exactly when the panel hardware has failed.
 *
 * TWO SCENES, and the split is forced rather than preferred: the display thread
 * reads device_is_ready() ONCE, 500 ms after boot. Denying readiness after that
 * does not make it reconsider, and denying it earlier makes the healthy path
 * unmeasurable. So the difference lives at boot, in
 * CONFIG_REFLOW_TEST_DENY_PANEL.
 */

#include <zephyr/device.h>
#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/zbus/zbus.h>
#include <zephyr/ztest.h>

#include "app.h"

ZBUS_CHAN_DECLARE(reflow_telemetry_chan);

/* The module under test's observer, so the test can assert about its state. */
extern const struct zbus_observer reflow_display_sub;

static const struct device *const panel = DEVICE_DT_GET(DT_CHOSEN(zephyr_display));

/* The same timeout controller.c publishes with. */
#define PUB_TIMEOUT K_MSEC(20)

/*
 * More than the observer's queue (4). Before the patch it is from the fifth on
 * that the timeout starts being paid in full - measuring six shows the regime
 * rather than the edge.
 */
#define PUBS 6

/*
 * Slack over the 500 ms delay in the display thread's K_THREAD_DEFINE, plus
 * room for ui_build() to finish. 900 ms was not enough on the CI runner: the
 * first publish landed while LVGL was still building the screen and cost 30 ms
 * on the HEALTHY path, which says nothing about this ticket.
 */
#define THREAD_SETTLE K_MSEC(2000)

/*
 * Gap between publishes, and the burst is what was wrong before it existed.
 * controller.c publishes every CONFIG_REFLOW_PUBLISH_PERIOD_MS - 500 ms by
 * default - and on state transitions; it never fires six in a row with no
 * yield. Without a gap the healthy scene measured the test's own scheduling
 * rather than the defect, because the display thread never got to run between
 * publishes.
 *
 * It does NOT soften the defect scene: with the observer enabled and nobody
 * calling zbus_sub_wait(), the queue stays full however long the gap is, so
 * publishes past the fourth still pay the whole timeout.
 */
#define PUB_GAP K_MSEC(20)

#if defined(CONFIG_REFLOW_TEST_DENY_PANEL)
/*
 * POST_KERNEL, before any application thread runs: by the time the display
 * thread wakes up the panel is already unavailable. device_is_ready() reads
 * exactly these two fields (kernel/device.c), so denying them here is the same
 * as a driver whose init failed - with no real panel and no fake driver.
 */
static int deny_panel(void)
{
	panel->state->initialized = false;
	panel->state->init_res = 1;
	return 0;
}
SYS_INIT(deny_panel, POST_KERNEL, CONFIG_KERNEL_INIT_PRIORITY_DEFAULT);
#endif

/* Cost, in ms, of PUBS consecutive publishes with controller.c's timeout. */
static void measure(int64_t *worst, int64_t *total)
{
	struct reflow_telemetry t = {
		.temp_mc = 25000,
		.temp_valid = true,
	};

	*worst = 0;
	*total = 0;

	for (int i = 0; i < PUBS; i++) {
		int64_t t0 = k_uptime_get();
		int64_t dt;

		(void)zbus_chan_pub(&reflow_telemetry_chan, &t, PUB_TIMEOUT);
		dt = k_uptime_get() - t0;

		*total += dt;
		if (dt > *worst) {
			*worst = dt;
		}

		/* Outside the measurement on purpose: this is the control loop's
		 * own cadence, not part of what a publish costs. */
		k_sleep(PUB_GAP);
	}
}

#if defined(CONFIG_REFLOW_TEST_DENY_PANEL)

ZTEST(reflow_ui, test_an_unavailable_panel_does_not_cost_the_timeout)
{
	int64_t worst, total;
	bool enabled = true;

	/* Fixture: the panel really has to be unavailable, or nothing is
	 * measured. */
	zassert_false(device_is_ready(panel),
		      "broken fixture: the panel is ready, so the display thread "
		      "never took the failure branch and this test would pass by "
		      "draining rather than by the patch");

	k_sleep(THREAD_SETTLE);

	zassert_ok(zbus_obs_is_enabled(&reflow_display_sub, &enabled));
	zassert_false(enabled,
		      "the display observer is still enabled after the thread "
		      "gave up; the queue fills at 4 and every publish starts "
		      "paying the whole 20 ms (RFO-B12)");

	measure(&worst, &total);

	zassert_true(worst < 5,
		     "a publish cost %lld ms with the panel unavailable (total "
		     "%lld ms over %d publishes). The observer stayed attached "
		     "with nobody draining it, so zbus_chan_pub() pays the whole "
		     "20 ms timeout - 20 %% of the %d ms control period (RFO-B12)",
		     worst, total, PUBS, CONFIG_REFLOW_CTRL_PERIOD_MS);
}

#else

ZTEST(reflow_ui, test_a_ready_panel_keeps_receiving)
{
	int64_t worst, total;
	bool enabled = false;

	zassert_true(device_is_ready(panel), "fixture: the panel should be ready");

	k_sleep(THREAD_SETTLE);

	/*
	 * The healthy path: the fix disables the observer ONLY in the failure
	 * branch. Somebody disabling it too early, or unconditionally, shows up
	 * here - the UI would stop receiving telemetry with a working panel.
	 */
	zassert_ok(zbus_obs_is_enabled(&reflow_display_sub, &enabled));
	zassert_true(enabled,
		     "the display observer is disabled with the panel READY: the "
		     "RFO-B12 fix leaked into the healthy path and the UI no "
		     "longer receives telemetry");

	/* And the thread is draining, so publishing is cheap here too. */
	measure(&worst, &total);
	zassert_true(worst < 5,
		     "a publish cost %lld ms with the panel ready (total %lld "
		     "ms): the display thread is not draining the queue",
		     worst, total);
}

#endif /* CONFIG_REFLOW_TEST_DENY_PANEL */

ZTEST_SUITE(reflow_ui, NULL, NULL, NULL, NULL, NULL);
