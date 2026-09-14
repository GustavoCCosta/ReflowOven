/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Rotary encoder + push button, through the Zephyr input subsystem
 * (gpio-qdec and gpio-keys). Optional: CONFIG_REFLOW_UI_INPUT.
 *
 * Rotating selects the profile while idle; a short press starts or stops the
 * run; a long press clears a latched fault.
 */

#include <zephyr/input/input.h>
#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/version.h>

#include "../core/app.h"
#include "buttonmap.h"
#include "selmap.h"

LOG_MODULE_REGISTER(reflow_input, CONFIG_REFLOW_LOG_LEVEL);

#define LONG_PRESS_MS 1000

/*
 * RFO-B17. The panel's index and the rule for when telemetry may replace it,
 * both in reflow_sel_*(), pure and tested in tests/logic/. What is left here is
 * only what needs the kernel: the two callbacks and the command queue.
 *
 * Written by both threads, like the uint8_t it replaces - but every write now
 * goes through a function whose contract says which value wins, and the next
 * index is derived from the panel's own value instead of from whatever the
 * last frame wrote. See selmap.c for why a lock alone would not have fixed it.
 */
static struct reflow_sel sel;
static int64_t press_started;

/*
 * Last state seen on the telemetry channel, or STATE_UNKNOWN before the
 * first publish. One atomic and not a (known, state) pair, because both are
 * read together in one decision: in two objects they could be read at
 * different instants, and the invalid combination would be exactly 'known'
 * carrying a stale state.
 */
#define STATE_UNKNOWN (-1)
static atomic_t last_state = ATOMIC_INIT(STATE_UNKNOWN);

/* Returns 0 when the queue took the command. */
static int post(uint8_t id, int32_t arg)
{
	struct reflow_cmd cmd = { .id = id, .arg = arg };

	if (reflow_cmd_post(&cmd, K_NO_WAIT) != 0) {
		LOG_WRN("command queue full, input dropped");
		return -1;
	}

	return 0;
}

static void on_rotate(int32_t steps)
{
	uint8_t count = reflow_profile_count();
	int next;

	if (atomic_get(&last_state) == REFLOW_STATE_RUNNING) {
		return;
	}

	next = reflow_sel_next(&sel, steps, count);
	if (next == REFLOW_SEL_NONE) {
		return;
	}

	/*
	 * Commit only if the queue took it. A command the queue dropped is one
	 * the controller will never confirm, and counting it as in flight would
	 * make the panel ignore telemetry for no reason at all.
	 */
	if (post(REFLOW_CMD_SELECT_PROFILE, next) == 0) {
		reflow_sel_commit(&sel, (uint8_t)next);
	}
}

static void on_button(bool pressed)
{
	if (pressed) {
		press_started = k_uptime_get();
		return;
	}

	if (press_started == 0) {
		return;
	}

	/*
	 * The whole decision lives in reflow_button_decide(), pure and tested in
	 * tests/logic/ (RFO-B13). What stays here is only what needs the kernel:
	 * timing the press and reading the state. A single atomic read, so the
	 * decision cannot see a state that changed halfway through it.
	 */
	atomic_val_t state = atomic_get(&last_state);
	bool long_press = (k_uptime_get() - press_started) >= LONG_PRESS_MS;
	int cmd = reflow_button_decide(state != STATE_UNKNOWN,
				       (uint8_t)state, long_press);

	if (cmd != REFLOW_BUTTON_NONE) {
		post((uint8_t)cmd, 0);
	}
	press_started = 0;
}

#if KERNEL_VERSION_NUMBER >= 0x030700
static void input_cb(struct input_event *evt, void *user_data)
{
	ARG_UNUSED(user_data);
#else
static void input_cb(struct input_event *evt)
{
#endif
	switch (evt->type) {
	case INPUT_EV_REL:
		if (evt->code == INPUT_REL_WHEEL || evt->code == INPUT_REL_X) {
			on_rotate(evt->value);
		}
		break;
	case INPUT_EV_KEY:
		if (evt->code == INPUT_KEY_ENTER || evt->code == INPUT_BTN_0) {
			on_button(evt->value != 0);
		}
		break;
	default:
		break;
	}
}

#if KERNEL_VERSION_NUMBER >= 0x030700
INPUT_CALLBACK_DEFINE(NULL, input_cb, NULL);
#else
INPUT_CALLBACK_DEFINE(NULL, input_cb);
#endif

/* Track the oven state: the button's decision needs it, and needs to know
 * whether it has ever been seen (RFO-B13). */
static void telemetry_cb(const struct zbus_channel *chan)
{
	const struct reflow_telemetry *t = zbus_chan_const_msg(chan);

	atomic_set(&last_state, (atomic_val_t)t->state);
	reflow_sel_telemetry(&sel, t->profile_idx);
}

/*
 * Explicitly, and not by leaning on the static zero-initialisation that happens
 * to match today: reflow_sel_init() is where the starting index is DECIDED, and
 * the day it stops being 0 this file should not silently disagree with it.
 */
static int sel_init(void)
{
	reflow_sel_init(&sel);
	return 0;
}
SYS_INIT(sel_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);

ZBUS_LISTENER_DEFINE(reflow_input_lsnr, telemetry_cb);
ZBUS_CHAN_ADD_OBS(reflow_telemetry_chan, reflow_input_lsnr, 4);
