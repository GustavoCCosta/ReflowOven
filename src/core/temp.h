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
 */
int reflow_temp_read(int32_t *temp_mc);

#endif /* REFLOW_TEMP_H_ */
