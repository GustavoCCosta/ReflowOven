/* SPDX-License-Identifier: Apache-2.0 */

#include <zephyr/kernel.h>

#include "telemetry_json.h"

int reflow_telemetry_json(const struct reflow_telemetry *t, char *buf, size_t len)
{
	const struct reflow_profile *prof = reflow_profile_get(t->profile_idx);
	const char *stage = "-";

	if (prof != NULL && t->stage_idx < prof->n_stages) {
		stage = prof->stages[t->stage_idx].name;
	}

	return snprintk(buf, len,
			"{\"temp_mc\":%d,\"temp_valid\":%s,"
			"\"setpoint_mc\":%d,\"duty\":%u,"
			"\"state\":\"%s\",\"fault\":\"%s\",\"profile\":%u,"
			"\"stage\":%u,\"n_stages\":%u,\"stage_name\":\"%s\","
			"\"stage_ms\":%u,\"total_ms\":%u,\"uptime_ms\":%u}",
			t->temp_mc, t->temp_valid ? "true" : "false",
			t->setpoint_mc, t->duty_permille,
			reflow_state_str(t->state), reflow_fault_str(t->fault),
			t->profile_idx, t->stage_idx, t->n_stages, stage,
			t->stage_ms, t->total_ms, t->uptime_ms);
}
