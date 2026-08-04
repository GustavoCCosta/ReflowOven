/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Control core: owns the sensor, the heater and the profile state machine.
 * It is the only writer of reflow_telemetry_chan and the only consumer of
 * commands. Everything else in the firmware is optional.
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include "app.h"
#include "heater.h"
#include "pid.h"
#include "profile.h"
#include "temp.h"

LOG_MODULE_REGISTER(reflow_ctrl, CONFIG_REFLOW_LOG_LEVEL);

ZBUS_CHAN_DEFINE(reflow_telemetry_chan, struct reflow_telemetry,
		 NULL, NULL, ZBUS_OBSERVERS_EMPTY,
		 ZBUS_MSG_INIT(.state = REFLOW_STATE_IDLE));

#define PERIOD_MS  CONFIG_REFLOW_CTRL_PERIOD_MS
#define SAMPLE_MS  CONFIG_REFLOW_SAMPLE_PERIOD_MS
#define PUBLISH_MS CONFIG_REFLOW_PUBLISH_PERIOD_MS
#define ABS_MAX_MC (CONFIG_REFLOW_ABS_MAX_TEMP_C * 1000)

/* No 'static' here: K_MSGQ_DEFINE already declares its buffer static. */
K_MSGQ_DEFINE(cmd_q, sizeof(struct reflow_cmd), 8, 4);

static const struct pid_cfg pid_cfg = {
	.kp = CONFIG_REFLOW_PID_KP_MILLI / 1000.0f,
	.ki = CONFIG_REFLOW_PID_KI_MILLI / 1000.0f,
	.kd = CONFIG_REFLOW_PID_KD_MILLI / 1000.0f,
	.out_min = 0.0f,
	.out_max = 1000.0f,
	.i_min = -(float)CONFIG_REFLOW_PID_I_CLAMP,
	.i_max = (float)CONFIG_REFLOW_PID_I_CLAMP,
	.d_alpha = CONFIG_REFLOW_PID_D_ALPHA_MILLI / 1000.0f,
};

static struct {
	enum reflow_state state;
	enum reflow_fault fault;
	uint8_t profile_idx;
	const struct reflow_profile *prof;
	struct reflow_run run;
	struct pid_state pid;
	int32_t temp_mc;
	bool temp_valid;
	uint16_t duty;
	int32_t setpoint_mc;
} ctx;

const char *reflow_state_str(uint8_t state)
{
	switch (state) {
	case REFLOW_STATE_IDLE:    return "idle";
	case REFLOW_STATE_RUNNING: return "running";
	case REFLOW_STATE_DONE:    return "done";
	case REFLOW_STATE_FAULT:   return "fault";
	default:                   return "?";
	}
}

const char *reflow_fault_str(uint8_t fault)
{
	switch (fault) {
	case REFLOW_FAULT_NONE:     return "none";
	case REFLOW_FAULT_SENSOR:   return "sensor";
	case REFLOW_FAULT_OVERTEMP: return "overtemp";
	case REFLOW_FAULT_TIMEOUT:  return "timeout";
	default:                    return "?";
	}
}

int reflow_cmd_post(const struct reflow_cmd *cmd, k_timeout_t timeout)
{
	return k_msgq_put(&cmd_q, cmd, timeout);
}

static void publish(void)
{
	struct reflow_telemetry t = {
		.uptime_ms = (uint32_t)k_uptime_get(),
		.temp_mc = ctx.temp_mc,
		.setpoint_mc = ctx.state == REFLOW_STATE_RUNNING ? ctx.setpoint_mc : 0,
		.duty_permille = ctx.duty,
		.state = (uint8_t)ctx.state,
		.fault = (uint8_t)ctx.fault,
		.profile_idx = ctx.profile_idx,
		.stage_idx = ctx.run.stage,
		.n_stages = ctx.prof != NULL ? ctx.prof->n_stages : 0,
		.stage_ms = ctx.run.stage_ms,
		.total_ms = ctx.run.total_ms,
	};

	(void)zbus_chan_pub(&reflow_telemetry_chan, &t, K_MSEC(20));
}

static void enter_fault(enum reflow_fault fault)
{
	reflow_heater_off();
	ctx.duty = 0;
	ctx.state = REFLOW_STATE_FAULT;
	ctx.fault = fault;
	LOG_ERR("fault: %s", reflow_fault_str(fault));
	publish();
}

static void stop_run(enum reflow_state next)
{
	reflow_heater_off();
	ctx.duty = 0;
	ctx.state = next;
	LOG_INF("run stopped -> %s", reflow_state_str(next));
	publish();
}

static void handle_cmd(const struct reflow_cmd *cmd)
{
	switch (cmd->id) {
	case REFLOW_CMD_START:
		if (ctx.state == REFLOW_STATE_FAULT) {
			LOG_WRN("start refused: clear the fault first");
			return;
		}
		if (!ctx.temp_valid) {
			LOG_WRN("start refused: no valid temperature yet");
			return;
		}
		if (ctx.state == REFLOW_STATE_RUNNING) {
			return;
		}
		reflow_run_start(&ctx.run, ctx.prof, ctx.temp_mc);
		pid_reset(&ctx.pid);
		ctx.setpoint_mc = ctx.run.setpoint_mc;
		ctx.state = REFLOW_STATE_RUNNING;
		LOG_INF("start profile '%s' at %d.%02d C", ctx.prof->name,
			ctx.temp_mc / 1000, (ctx.temp_mc % 1000) / 10);
		publish();
		break;

	case REFLOW_CMD_STOP:
		if (ctx.state == REFLOW_STATE_RUNNING) {
			stop_run(REFLOW_STATE_IDLE);
		}
		break;

	case REFLOW_CMD_SELECT_PROFILE: {
		const struct reflow_profile *p;

		if (ctx.state == REFLOW_STATE_RUNNING) {
			LOG_WRN("cannot switch profile while running");
			return;
		}
		p = reflow_profile_get((uint8_t)cmd->arg);
		if (p == NULL) {
			LOG_WRN("no such profile: %d", cmd->arg);
			return;
		}
		ctx.profile_idx = (uint8_t)cmd->arg;
		ctx.prof = p;
		LOG_INF("profile: %s", p->name);
		publish();
		break;
	}

	case REFLOW_CMD_CLEAR_FAULT:
		if (ctx.state == REFLOW_STATE_FAULT) {
			ctx.fault = REFLOW_FAULT_NONE;
			ctx.state = REFLOW_STATE_IDLE;
			LOG_INF("fault cleared");
			publish();
		}
		break;

	default:
		break;
	}
}

static void run_step(uint32_t dt_ms, bool new_sample, uint32_t sample_dt_ms)
{
	enum reflow_run_result res;
	float duty;

	res = reflow_run_tick(&ctx.run, ctx.prof, dt_ms, ctx.temp_mc);
	ctx.setpoint_mc = ctx.run.setpoint_mc;

	switch (res) {
	case REFLOW_RUN_ACTIVE:
		break;
	case REFLOW_RUN_DONE:
		stop_run(REFLOW_STATE_DONE);
		return;
	case REFLOW_RUN_ERR_OVERTEMP:
		enter_fault(REFLOW_FAULT_OVERTEMP);
		return;
	case REFLOW_RUN_ERR_TIMEOUT:
		enter_fault(REFLOW_FAULT_TIMEOUT);
		return;
	}

	if (!reflow_run_heater_allowed(&ctx.run, ctx.prof)) {
		ctx.duty = 0;
		reflow_heater_set_duty(0);
		return;
	}

	if (new_sample) {
		duty = pid_step(&pid_cfg, &ctx.pid,
				ctx.setpoint_mc / 1000.0f,
				ctx.temp_mc / 1000.0f,
				sample_dt_ms / 1000.0f, NULL);
		ctx.duty = (uint16_t)(duty + 0.5f);
	}

	reflow_heater_set_duty(ctx.duty);
}

static void controller_thread(void *a, void *b, void *c)
{
	int64_t last_tick, last_publish, last_sample;
	uint32_t since_sample = 0;
	bool ready;

	ARG_UNUSED(a);
	ARG_UNUSED(b);
	ARG_UNUSED(c);

	ctx.state = REFLOW_STATE_IDLE;
	ctx.fault = REFLOW_FAULT_NONE;
	ctx.profile_idx = 0;
	ctx.prof = reflow_profile_get(0);

	ready = (reflow_heater_init() == 0) && (reflow_temp_init() == 0);
	if (!ready) {
		enter_fault(REFLOW_FAULT_SENSOR);
	}

	last_tick = k_uptime_get();
	last_publish = last_tick;
	last_sample = last_tick;

	while (true) {
		int64_t now = k_uptime_get();
		uint32_t dt_ms = (uint32_t)(now - last_tick);
		bool new_sample = false;
		uint32_t sample_dt = 0;
		int64_t deadline;

		last_tick = now;
		since_sample += dt_ms;

		if (ready && since_sample >= SAMPLE_MS) {
			int32_t mc;
			int ret = reflow_temp_read(&mc);

			sample_dt = (uint32_t)(now - last_sample);
			last_sample = now;
			since_sample = 0;

			if (ret == 0) {
				ctx.temp_mc = mc;
				ctx.temp_valid = true;
				new_sample = true;
			} else {
				ctx.temp_valid = false;
				if (ctx.state != REFLOW_STATE_FAULT) {
					enter_fault(REFLOW_FAULT_SENSOR);
				}
			}
		}

		/* Hard limit, independent of the profile's own abort level. */
		if (ctx.temp_valid && ctx.temp_mc >= ABS_MAX_MC &&
		    ctx.state != REFLOW_STATE_FAULT) {
			enter_fault(REFLOW_FAULT_OVERTEMP);
		}

		if (ctx.state == REFLOW_STATE_RUNNING) {
			run_step(dt_ms, new_sample, sample_dt ? sample_dt : SAMPLE_MS);
		} else {
			ctx.duty = 0;
			reflow_heater_off();
		}

		reflow_heater_tick(dt_ms);

		if (now - last_publish >= PUBLISH_MS) {
			last_publish = now;
			publish();
		}

		/* Sleep out the period, but service commands immediately. */
		deadline = now + PERIOD_MS;
		while (true) {
			struct reflow_cmd cmd;
			int64_t rem = deadline - k_uptime_get();

			if (rem <= 0) {
				break;
			}
			if (k_msgq_get(&cmd_q, &cmd, K_MSEC(rem)) == 0) {
				handle_cmd(&cmd);
			}
		}
	}
}

K_THREAD_DEFINE(reflow_ctrl_tid, CONFIG_REFLOW_CTRL_STACK_SIZE,
		controller_thread, NULL, NULL, NULL,
		CONFIG_REFLOW_CTRL_PRIORITY, 0, 100);
