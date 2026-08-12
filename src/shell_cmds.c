/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Serial console commands. Optional: CONFIG_REFLOW_SHELL.
 * Handy for bring-up before the display or Wi-Fi exist:
 *   reflow status | json | start | stop | clear | profile <n>
 *
 * 'json' is the machine-readable twin of 'status': one line, same object the
 * HTTP server serves at /api/state. It is what lets a host - a script, or the
 * local Web Serial page in web/ - drive the oven over the USB serial port
 * without parsing human text.
 */

#include <stdlib.h>

#include <zephyr/kernel.h>
#include <zephyr/shell/shell.h>

#include "core/app.h"
#include "telemetry_json.h"

static struct reflow_telemetry last;

static void telemetry_cb(const struct zbus_channel *chan)
{
	last = *(const struct reflow_telemetry *)zbus_chan_const_msg(chan);
}

ZBUS_LISTENER_DEFINE(reflow_shell_lsnr, telemetry_cb);
ZBUS_CHAN_ADD_OBS(reflow_telemetry_chan, reflow_shell_lsnr, 4);

static int post(const struct shell *sh, uint8_t id, int32_t arg)
{
	struct reflow_cmd cmd = { .id = id, .arg = arg };
	int ret = reflow_cmd_post(&cmd, K_MSEC(100));

	if (ret != 0) {
		shell_error(sh, "command queue busy (%d)", ret);
	}
	return ret;
}

static int cmd_status(const struct shell *sh, size_t argc, char **argv)
{
	const struct reflow_profile *prof = reflow_profile_get(last.profile_idx);

	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	shell_print(sh, "state      : %s", reflow_state_str(last.state));
	shell_print(sh, "fault      : %s", reflow_fault_str(last.fault));
	if (last.temp_valid) {
		shell_print(sh, "temperature: %d.%03d C", last.temp_mc / 1000,
			    abs(last.temp_mc) % 1000);
	} else {
		shell_print(sh, "temperature: --  (no valid reading)");
	}
	shell_print(sh, "setpoint   : %d.%03d C", last.setpoint_mc / 1000,
		    abs(last.setpoint_mc) % 1000);
	shell_print(sh, "duty       : %u.%u %%", last.duty_permille / 10,
		    last.duty_permille % 10);
	shell_print(sh, "profile    : %u (%s)", last.profile_idx,
		    prof ? prof->name : "?");
	shell_print(sh, "stage      : %u/%u, %u s (total %u s)",
		    last.n_stages ? last.stage_idx + 1U : 0U, last.n_stages,
		    last.stage_ms / 1000U, last.total_ms / 1000U);
	return 0;
}

static int cmd_json(const struct shell *sh, size_t argc, char **argv)
{
	char buf[REFLOW_JSON_BUF_SZ];

	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	(void)reflow_telemetry_json(&last, buf, sizeof(buf));
	/* One line, always starting with '{': that is how a host picks it out
	 * of the shell's echo and prompt without parsing them.
	 */
	shell_print(sh, "%s", buf);
	return 0;
}

static int cmd_start(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);
	return post(sh, REFLOW_CMD_START, 0);
}

static int cmd_stop(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);
	return post(sh, REFLOW_CMD_STOP, 0);
}

static int cmd_clear(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);
	return post(sh, REFLOW_CMD_CLEAR_FAULT, 0);
}

static int cmd_profile(const struct shell *sh, size_t argc, char **argv)
{
	if (argc < 2) {
		for (uint8_t i = 0; i < reflow_profile_count(); i++) {
			shell_print(sh, "[%u] %s", i, reflow_profile_get(i)->name);
		}
		return 0;
	}
	return post(sh, REFLOW_CMD_SELECT_PROFILE, atoi(argv[1]));
}

SHELL_STATIC_SUBCMD_SET_CREATE(reflow_sub,
	SHELL_CMD(status, NULL, "Show controller state", cmd_status),
	SHELL_CMD(json, NULL, "Same state as one line of JSON", cmd_json),
	SHELL_CMD(start, NULL, "Start the selected profile", cmd_start),
	SHELL_CMD(stop, NULL, "Abort the run", cmd_stop),
	SHELL_CMD(clear, NULL, "Clear a latched fault", cmd_clear),
	SHELL_CMD_ARG(profile, NULL, "List or select profile: profile [n]",
		      cmd_profile, 1, 1),
	SHELL_SUBCMD_SET_END);

SHELL_CMD_REGISTER(reflow, &reflow_sub, "Reflow oven control", NULL);
