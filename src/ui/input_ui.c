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
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/version.h>

#include "../core/app.h"

LOG_MODULE_REGISTER(reflow_input, CONFIG_REFLOW_LOG_LEVEL);

#define LONG_PRESS_MS 1000

static uint8_t selected;
static int64_t press_started;
static atomic_t running;

static void post(uint8_t id, int32_t arg)
{
	struct reflow_cmd cmd = { .id = id, .arg = arg };

	if (reflow_cmd_post(&cmd, K_NO_WAIT) != 0) {
		LOG_WRN("command queue full, input dropped");
	}
}

static void on_rotate(int32_t steps)
{
	uint8_t count = reflow_profile_count();
	int32_t next;

	if (count == 0U || atomic_get(&running)) {
		return;
	}

	next = ((int32_t)selected + steps) % (int32_t)count;
	if (next < 0) {
		next += count;
	}
	selected = (uint8_t)next;
	post(REFLOW_CMD_SELECT_PROFILE, selected);
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

	if (k_uptime_get() - press_started >= LONG_PRESS_MS) {
		post(REFLOW_CMD_CLEAR_FAULT, 0);
	} else if (atomic_get(&running)) {
		post(REFLOW_CMD_STOP, 0);
	} else {
		post(REFLOW_CMD_START, 0);
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

/* Track the run state so the button can act as start/stop. */
static void telemetry_cb(const struct zbus_channel *chan)
{
	const struct reflow_telemetry *t = zbus_chan_const_msg(chan);

	atomic_set(&running, t->state == REFLOW_STATE_RUNNING ? 1 : 0);
	selected = t->profile_idx;
}

ZBUS_LISTENER_DEFINE(reflow_input_lsnr, telemetry_cb);
ZBUS_CHAN_ADD_OBS(reflow_telemetry_chan, reflow_input_lsnr, 4);
