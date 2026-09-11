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
#include "buttonmap.h"

LOG_MODULE_REGISTER(reflow_input, CONFIG_REFLOW_LOG_LEVEL);

#define LONG_PRESS_MS 1000

static uint8_t selected;
static int64_t press_started;

/*
 * Ultimo estado visto na telemetria, ou ESTADO_DESCONHECIDO antes da
 * primeira publicacao. Um atomic so, e nao um par (conhecido, estado),
 * porque os dois sao lidos juntos numa decisao: em dois objetos poderiam ser
 * lidos em instantes diferentes, e a combinacao invalida seria justamente
 * 'conhecido' com estado velho.
 */
#define ESTADO_DESCONHECIDO (-1)
static atomic_t last_state = ATOMIC_INIT(ESTADO_DESCONHECIDO);

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

	if (count == 0U || atomic_get(&last_state) == REFLOW_STATE_RUNNING) {
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

	/*
	 * A decisao inteira vive em reflow_button_decide(), pura e testada em
	 * tests/logic/ (RFO-B13). Aqui fica so o que precisa do kernel: medir a
	 * pressao e ler o estado. Uma leitura atomica so, para a decisao nao ver
	 * um estado que mudou no meio dela.
	 */
	atomic_val_t estado = atomic_get(&last_state);
	bool longa = (k_uptime_get() - press_started) >= LONG_PRESS_MS;
	int cmd = reflow_button_decide(estado != ESTADO_DESCONHECIDO,
				       (uint8_t)estado, longa);

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
	selected = t->profile_idx;
}

ZBUS_LISTENER_DEFINE(reflow_input_lsnr, telemetry_cb);
ZBUS_CHAN_ADD_OBS(reflow_telemetry_chan, reflow_input_lsnr, 4);
