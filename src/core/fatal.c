/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * RFO-B44: the fatal-error path cuts the element before the CPU stops.
 *
 * Zephyr's default k_sys_fatal_error_handler ends in `Halting system` - the CPU
 * stops, with no reset. Every fail-safe this firmware has until then is a
 * thread: the stale-duty disarm in heater.c re-checks on every
 * reflow_heater_tick(), and the over-temperature cut-out lives in the control
 * loop. A halted CPU runs none of them, so before this file existed any
 * k_panic(), any CPU exception, any stack overflow in any thread left the SSR
 * gate at whatever level the PWM window had last driven - indefinitely, and at
 * a high level if the run was at a high duty.
 *
 * That is a wider route than the three the gate label was invented for (B03,
 * B05, B06): it does not need a sensor, a profile or a stage to go wrong.
 *
 * WHY HALT AND NOT CONFIG_RESET_ON_FATAL_ERROR.
 *
 * First, what this is NOT: halting is not a cost this file introduces. The
 * kernel's own weak handler (kernel/fatal.c) already ends in LOG_PANIC() plus
 * `Halting system` and arch_system_halt(), and switches to sys_reboot() only
 * when CONFIG_RESET_ON_FATAL_ERROR is set - which this project does not set.
 * So the disposition to stop is today's behaviour, and what this file adds is
 * the cut. The paragraphs below are the reason not to CHANGE that disposition
 * to a reset while adding the cut, not a trade being made here for the first
 * time (RFO-B44 review).
 *
 * Both leave the gate low - a reset would re-run ssr_safe_init() and claim the
 * pin low again (RFO-B06). The difference is what the oven is afterwards.
 *
 * A reset comes back as a working oven that accepts START, and it comes back
 * WITHOUT the latched fault: an oven that stopped in FAULT_OVERTEMP returns
 * idle, having forgotten that it was too hot and why. The latch exists because
 * an over-temperature oven must need a human to clear it, and a reset is a
 * mechanism that clears it without one. A deterministic fault - the stack
 * overflow of RFO-B43 is exactly that shape - would also loop: reset, run, heat,
 * fault, reset, each cycle re-energising the element for as long as the run
 * takes to reach the same bug.
 *
 * A halt leaves the gate driven low and the board inert. It costs availability
 * and it costs the remote UI: the oven does not come back on its own, and
 * nobody is told over the network - a bench operator sees a dead board and
 * power-cycles it, which is the deliberate human step the fault latch was
 * asking for anyway. It also preserves the register state that makes RFO-B43
 * diagnosable.
 *
 * So: cut, say so on the console, then halt - which is where the kernel was
 * already going. Availability is not worth buying back here, because the
 * price is an element that may be energised again by a machine instead of a
 * person.
 *
 * WHAT THIS DOES NOT COVER. A fault that never becomes fatal - a thread stuck
 * in a loop, a deadlock - does not reach this handler at all; that is watchdog
 * territory (CONFIG_TASK_WDT) and it is not this file. Nor does any of it
 * survive a dead CPU or corrupt firmware: only the hardware pull-down on the
 * gate does, and README.md still requires it.
 */

#include <zephyr/fatal.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/logging/log_ctrl.h>

#include "heater.h"

LOG_MODULE_REGISTER(reflow_fatal, CONFIG_REFLOW_LOG_LEVEL);

void k_sys_fatal_error_handler(unsigned int reason, const struct arch_esf *esf)
{
	ARG_UNUSED(esf);

	/*
	 * Before anything else, and before the logging subsystem is asked to
	 * do anything: the element comes off first, because every line below
	 * can fail and the gate still has to be low.
	 */
	reflow_heater_emergency_off();

	/*
	 * LOG_PANIC() flushes what is already buffered and switches the logging
	 * subsystem to synchronous output, so the line the cut emitted and the
	 * one below both reach the console from a context where no thread will
	 * ever drain a buffer.
	 *
	 * It is called AFTER the cut deliberately. The cut is two register
	 * writes; this walks the whole logging subsystem and is the more likely
	 * of the two to fail in a corrupted kernel. Losing the evidence is
	 * survivable, leaving the element on is not.
	 */
	LOG_PANIC();
	LOG_ERR("fatal error %u: element cut, halting (RFO-B44)", reason);

	k_fatal_halt(reason);
}
