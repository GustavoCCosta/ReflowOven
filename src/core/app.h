/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Public contract between the control core and the optional feature modules
 * (display UI, input, network, ...). Modules never call each other: they
 * observe reflow_telemetry_chan and they push commands with reflow_cmd_post().
 * That is what makes a feature removable with a single Kconfig symbol.
 */

#ifndef REFLOW_APP_H_
#define REFLOW_APP_H_

#include <zephyr/kernel.h>
#include <zephyr/zbus/zbus.h>

#include "profile.h"

enum reflow_state {
	REFLOW_STATE_IDLE = 0,
	REFLOW_STATE_RUNNING,
	REFLOW_STATE_DONE,
	REFLOW_STATE_FAULT,
};

enum reflow_fault {
	REFLOW_FAULT_NONE = 0,
	REFLOW_FAULT_SENSOR,
	REFLOW_FAULT_OVERTEMP,
	REFLOW_FAULT_TIMEOUT,
};

struct reflow_telemetry {
	uint32_t uptime_ms;
	int32_t temp_mc;      /* measured temperature, milli-degC */
	int32_t setpoint_mc;  /* 0 when idle */
	uint16_t duty_permille;
	uint8_t state;        /* enum reflow_state */
	uint8_t fault;        /* enum reflow_fault */
	uint8_t profile_idx;
	uint8_t stage_idx;
	uint8_t n_stages;
	uint32_t stage_ms;
	uint32_t total_ms;
};

ZBUS_CHAN_DECLARE(reflow_telemetry_chan);

enum reflow_cmd_id {
	REFLOW_CMD_START = 0,
	REFLOW_CMD_STOP,
	REFLOW_CMD_SELECT_PROFILE,  /* arg = profile index */
	REFLOW_CMD_CLEAR_FAULT,
};

struct reflow_cmd {
	uint8_t id;
	int32_t arg;
};

/* Thread-safe, callable from any module. Non-blocking with K_NO_WAIT. */
int reflow_cmd_post(const struct reflow_cmd *cmd, k_timeout_t timeout);

/* Human readable helpers, shared by the UI and the web page. */
const char *reflow_state_str(uint8_t state);
const char *reflow_fault_str(uint8_t fault);

#endif /* REFLOW_APP_H_ */
