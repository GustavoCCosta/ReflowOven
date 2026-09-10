/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Slow-PWM (time proportioning) driver for a zero-cross SSR.
 * The window is a few hundred ms to a few seconds: a mains SSR cannot be
 * switched faster than a half cycle, and the oven's thermal time constant is
 * tens of seconds, so a slow window costs nothing in control quality.
 */

#ifndef REFLOW_HEATER_H_
#define REFLOW_HEATER_H_

#include <stdbool.h>
#include <stdint.h>

int reflow_heater_init(void);

/* Requested duty, 0..1000 permille. Also re-arms the output watchdog. */
void reflow_heater_set_duty(uint16_t permille);

/* Force the output off and zero the request. Safe to call from any context. */
void reflow_heater_off(void);

/*
 * Drive the gate low from a fatal-error path, WITHOUT taking the lock.
 *
 * reflow_heater_off() is the wrong call there (RFO-B44). It takes the module
 * spinlock, and a fatal error raised inside this module - or in an ISR that
 * interrupted it - arrives with that spinlock already held. Waiting for it
 * would hang before the gate ever moved, on the one path where the gate is
 * all that matters.
 *
 * The cost of skipping the lock is that the bookkeeping can be left
 * inconsistent with the pin. That is acceptable and only here: this path ends
 * in a halt or a reset, so nothing reads the bookkeeping again.
 *
 * Call this BEFORE LOG_PANIC(), not after. The cut is two register writes;
 * LOG_PANIC() walks the whole logging subsystem and is the more likely of the
 * two to fail in a corrupted kernel, so the element comes off first and the
 * evidence second. Nothing is lost by that order: LOG_PANIC() flushes what is
 * already buffered, so the line this function logs still reaches the console.
 */
void reflow_heater_emergency_off(void);

/*
 * Advance the PWM window by dt_ms and drive the GPIO. Must be called
 * periodically by the control thread; if it stops being called, or if
 * set_duty() goes stale, the output is forced off.
 */
void reflow_heater_tick(uint32_t dt_ms);

uint16_t reflow_heater_duty(void);
bool reflow_heater_is_on(void);

#endif /* REFLOW_HEATER_H_ */
