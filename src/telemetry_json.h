/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * One formatter for the telemetry object, shared by every consumer: the HTTP
 * server, the `reflow json` shell command and anything reading the serial
 * port. Three hand-written copies of the same JSON would drift apart.
 */

#ifndef REFLOW_TELEMETRY_JSON_H_
#define REFLOW_TELEMETRY_JSON_H_

#include <stddef.h>

#include "core/app.h"

/* Enough for the object below with the longest built-in stage name. */
#define REFLOW_JSON_BUF_SZ 384

/*
 * Writes t as a single line of JSON (no trailing newline). Returns the number
 * of characters written, following snprintk() semantics.
 */
int reflow_telemetry_json(const struct reflow_telemetry *t, char *buf, size_t len);

#endif /* REFLOW_TELEMETRY_JSON_H_ */
