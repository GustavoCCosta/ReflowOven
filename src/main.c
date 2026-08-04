/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * The application entry point is deliberately empty of feature code: the
 * control core and every optional module are self-registering threads, so
 * enabling or removing a feature is a Kconfig edit and nothing else.
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include "core/app.h"

LOG_MODULE_REGISTER(reflow_main, CONFIG_REFLOW_LOG_LEVEL);

int main(void)
{
	LOG_INF("reflow oven firmware, board %s", CONFIG_BOARD);
	LOG_INF("profiles available: %u", reflow_profile_count());

	for (uint8_t i = 0; i < reflow_profile_count(); i++) {
		LOG_INF("  [%u] %s", i, reflow_profile_get(i)->name);
	}

	LOG_INF("features:"
#ifdef CONFIG_REFLOW_UI_DISPLAY
		" display"
#endif
#ifdef CONFIG_REFLOW_UI_INPUT
		" input"
#endif
#ifdef CONFIG_REFLOW_NET
		" net"
#endif
#ifdef CONFIG_REFLOW_SHELL
		" shell"
#endif
		"");

	return 0;
}
