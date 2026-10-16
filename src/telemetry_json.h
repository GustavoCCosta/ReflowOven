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

#include <zephyr/toolchain.h>

#include "core/app.h"
#include "core/profile.h"

/*
 * Enough for the object below with the LONGEST stage name the type allows,
 * not with the longest one the built-in table happens to use today.
 *
 * The worst case is 242 bytes of literal text and widest-case numbers, plus
 * REFLOW_STAGE_NAME_LEN - 1 for the name and its NUL: 254 at the current
 * sizes. The slack is deliberate and the BUILD_ASSERT below is what keeps it
 * honest - growing a stage name past this buffer is a failed build, not a
 * truncated telemetry frame discovered on a board.
 */
#define REFLOW_JSON_BUF_SZ 384

/* 242 = literal characters of the format plus the widest numeric fields. */
#define REFLOW_JSON_WORST_CASE (242U + (REFLOW_STAGE_NAME_LEN - 1U) + 1U)

BUILD_ASSERT(REFLOW_JSON_WORST_CASE <= REFLOW_JSON_BUF_SZ,
	     "a stage name this long no longer fits REFLOW_JSON_BUF_SZ");

/*
 * Writes t as a single line of JSON (no trailing newline). Returns the number
 * of characters written, following snprintk() semantics.
 */
int reflow_telemetry_json(const struct reflow_telemetry *t, char *buf, size_t len);

#endif /* REFLOW_TELEMETRY_JSON_H_ */
