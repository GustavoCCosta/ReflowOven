/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Injectable thermocouple for the controller suite (RFO-G06).
 *
 * This file provides the temp.h contract in place of src/core/temp.c, so
 * controller.c can be linked into a test image with no MAX6675 and no SPI
 * behind it. The test writes the sample; the controller reads it through
 * exactly the same two functions it calls on hardware.
 *
 * It is deliberately NOT a Kconfig option in the firmware. A production build
 * able to read a made-up temperature is a foot-gun on a mains heater: the
 * substitution happens at link time, in the test's CMakeLists, so no shipped
 * image can contain it.
 */

#ifndef REFLOW_TEMP_FAKE_H_
#define REFLOW_TEMP_FAKE_H_

#include <stdint.h>

/* Healthy sensor at 25 degC, no injected errors, spike history forgotten. */
void reflow_temp_fake_reset(void);

/*
 * The raw sample the next reads return, in milli-degC. It goes through the
 * same plausibility window and the same reflow_spike_filter() that temp.c
 * uses, so a jump larger than REFLOW_SPIKE_MAX_STEP_MC is suppressed here for
 * the same number of samples it would be suppressed on hardware. That is what
 * makes "stuck reading" (RFO-B05) reproducible instead of hand-waved: the test
 * injects a real step, the filter does the sticking.
 */
void reflow_temp_fake_set_raw_mc(int32_t raw_mc);

/*
 * Make reflow_temp_read() return this errno instead of a sample; 0 stops
 * failing. -ENOENT is what the MAX6675 driver reports for an open
 * thermocouple, -EIO what a failed SPI transfer reports.
 */
void reflow_temp_fake_fail(int err);

/* Make reflow_temp_init() return this errno; 0 stops failing. */
void reflow_temp_fake_fail_init(int err);

/* Number of reflow_temp_read() calls since the last reset. */
uint32_t reflow_temp_fake_reads(void);

#endif /* REFLOW_TEMP_FAKE_H_ */
