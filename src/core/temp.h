/* SPDX-License-Identifier: Apache-2.0 */

#ifndef REFLOW_TEMP_H_
#define REFLOW_TEMP_H_

#include <stdint.h>

/* 0 on success, -ENODEV when the thermocouple front-end is missing. */
int reflow_temp_init(void);

/*
 * Read the thermocouple, in milli-degC.
 * 0 on success, negative errno on sensor failure (open thermocouple, SPI
 * error, implausible reading). A failure must be treated as a hard fault:
 * losing the sensor while the heater is on is the dangerous case.
 *
 * *temp_mc receives the spike-filtered value, which is what the control loop
 * should follow. *raw_mc, when not NULL, receives the sample as it came off the
 * chip: the absolute over-temperature backstop must use that one, or the spike
 * rejector can suppress a real rise and blind the cut-out (RFO-B05). Both are
 * written only when the call returns 0.
 */
int reflow_temp_read(int32_t *temp_mc, int32_t *raw_mc);

#endif /* REFLOW_TEMP_H_ */
