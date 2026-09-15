/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * See temp_fake.h. This mirrors the shape of src/core/temp.c — plausibility
 * window, then reflow_spike_filter(), then both the filtered and the raw value
 * out — with the sensor driver replaced by a value the test writes.
 *
 * Keeping the guard rails here rather than returning the injected value
 * straight through is the point: a fake that skipped the filter would let a
 * controller test pass while the real path suppressed the same sample.
 */

#include <errno.h>

#include <zephyr/kernel.h>

#include "temp.h"
#include "tempguard.h"
#include "temp_fake.h"

/*
 * Same window as temp.c, copied rather than shared: the constants are private to
 * temp.c and hoisting them into temp.h is a production change, out of scope here.
 *
 * Say the cost out loud, because no test catches it: widen or narrow the window in
 * temp.c and this fake keeps the old one, silently. Nothing goes red. Unlike the
 * spike filter below -- which calls the real tempguard.c precisely so it cannot
 * drift -- this pair is kept honest by whoever edits temp.c remembering to look
 * here. If the window ever becomes a decision rather than a sanity check, move it
 * to tempguard.c and delete these two lines.
 */
#define TEMP_MIN_MC (-20000)
#define TEMP_MAX_MC (400000)

/*
 * How many times reflow_temp_init() fails before it starts succeeding.
 *
 * Set at BUILD time - see tests/coldstart/CMakeLists.txt - and deliberately
 * not at runtime. The control thread runs its first init from K_THREAD_DEFINE
 * before any test body, and before a `setup` hook too, so a runtime knob could
 * never be set in time to be observed. That is exactly what made the old
 * reflow_temp_fake_fail_init() dead code (RFO-G17). A build-time value is
 * honest: it is fixed before anything runs, which is when it has to be.
 */
#ifndef REFLOW_TEMP_FAKE_INIT_FAILURES
#define REFLOW_TEMP_FAKE_INIT_FAILURES 0
#endif

/* Room temperature: what a healthy sensor reads with the oven cold. */
#define FAKE_IDLE_MC 25000

static struct k_spinlock lock;

static struct {
	struct reflow_spike spike;
	int32_t raw_mc;
	int read_err;
	uint32_t reads;
} fake = {
	.raw_mc = FAKE_IDLE_MC,
};

void reflow_temp_fake_reset(void)
{
	k_spinlock_key_t key = k_spin_lock(&lock);
	int32_t discard;

	reflow_spike_reset(&fake.spike);
	fake.raw_mc = FAKE_IDLE_MC;
	fake.read_err = 0;
	fake.reads = 0;

	/*
	 * Prime the filter with the idle reading, so the fake starts out as a
	 * sensor that has been reading room temperature rather than one that
	 * has never been read. It matters: reflow_spike_filter() accepts its
	 * first sample unconditionally, having nothing to compare against, so
	 * an unprimed fake would swallow the very first injected step -- and a
	 * test that means to exercise the spike rejector would quietly exercise
	 * nothing.
	 */
	(void)reflow_spike_filter(&fake.spike, FAKE_IDLE_MC, &discard);
	k_spin_unlock(&lock, key);
}

void reflow_temp_fake_set_raw_mc(int32_t raw_mc)
{
	k_spinlock_key_t key = k_spin_lock(&lock);

	fake.raw_mc = raw_mc;
	k_spin_unlock(&lock, key);
}

void reflow_temp_fake_fail(int err)
{
	k_spinlock_key_t key = k_spin_lock(&lock);

	fake.read_err = err;
	k_spin_unlock(&lock, key);
}

uint32_t reflow_temp_fake_reads(void)
{
	k_spinlock_key_t key = k_spin_lock(&lock);
	uint32_t n = fake.reads;

	k_spin_unlock(&lock, key);
	return n;
}

/*
 * Fails the first REFLOW_TEMP_FAKE_INIT_FAILURES times, then succeeds.
 *
 * The count is a build-time value and not a runtime knob, and the difference is
 * the whole point: the control thread runs this from K_THREAD_DEFINE at boot,
 * before any ztest body and before a `setup` hook, so nothing set at runtime
 * could ever be in place in time. A knob that looks usable and is not is what
 * RFO-G17 removed from this file; this replaces it with something that is fixed
 * before the first line of the image runs.
 *
 * Default 0, so tests/controller sees a front-end that is simply there. The
 * cold-start image in tests/coldstart builds the same file with 1 and watches
 * the controller recover without a reboot (RFO-B39).
 */
static uint8_t init_failures_left = REFLOW_TEMP_FAKE_INIT_FAILURES;

int reflow_temp_init(void)
{
	k_spinlock_key_t key = k_spin_lock(&lock);

	if (init_failures_left > 0U) {
		init_failures_left--;
		k_spin_unlock(&lock, key);
		return -ENODEV;
	}

	reflow_spike_reset(&fake.spike);
	k_spin_unlock(&lock, key);

	return 0;
}

int reflow_temp_read(int32_t *temp_mc, int32_t *raw_mc)
{
	k_spinlock_key_t key = k_spin_lock(&lock);
	int32_t raw, filtered;
	int ret = 0;

	fake.reads++;

	if (fake.read_err != 0) {
		ret = fake.read_err;
		goto out;
	}

	raw = fake.raw_mc;
	if (raw < TEMP_MIN_MC || raw > TEMP_MAX_MC) {
		ret = -ERANGE;
		goto out;
	}

	(void)reflow_spike_filter(&fake.spike, raw, &filtered);

	*temp_mc = filtered;
	if (raw_mc != NULL) {
		*raw_mc = raw;
	}

out:
	k_spin_unlock(&lock, key);
	return ret;
}
