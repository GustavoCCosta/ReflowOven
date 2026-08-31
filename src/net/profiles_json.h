/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * The body of GET /api/profiles, as a pure function of the profile table.
 *
 * Kept apart from httpd.c, and free of any Zephyr dependency, for the same
 * reason cmdparse.c and httpgate.c are: this is a network-facing formatter
 * writing into a fixed stack buffer, and the interesting question about it is
 * what happens when the data does not fit. That question is answerable with
 * nothing but a char array, so it is answered by tests instead of by reading
 * (RFO-B09).
 *
 * The defect this replaces: httpd.c accumulated the return of snprintk(), which
 * is the length that WOULD have been written. Past the buffer, the next call
 * received (size_t)(sizeof(body) - n) with n > sizeof(body) - an unsigned
 * subtraction landing near SIZE_MAX - and a destination already out of bounds.
 * The same n was then handed to send_all() as the length, so the overflow was
 * also a read of stack memory into a socket.
 */

#ifndef REFLOW_PROFILES_JSON_H_
#define REFLOW_PROFILES_JSON_H_

#include <stddef.h>
#include <stdint.h>

#include "../core/profile.h"

/* Returns NULL past the last profile. reflow_profile_get() has this shape. */
typedef const struct reflow_profile *(*reflow_profile_lookup)(uint8_t idx);

/*
 * Write {"profiles":["a","b"]} into buf and return its length, excluding the
 * terminating NUL. Guarantees, for any count and any names:
 *
 *   - the return value is never greater than cap - 1, and never counts a byte
 *     that was not written;
 *   - buf is always NUL-terminated when cap > 0;
 *   - nothing is written past buf[cap - 1];
 *   - what is written is always complete JSON, never a half-finished entry.
 *
 * Truncation is explicit rather than accidental. When the whole list does not
 * fit, the entries that do fit are emitted and the object carries
 * "truncated":true, so a client can tell a short list from a complete one. A
 * silently short list would be a lie to the UI, and a hard error would take the
 * page down over a cosmetic limit.
 *
 * Returns 0 when even the empty, truncated form does not fit (cap below
 * REFLOW_PROFILES_JSON_MIN); buf is then an empty string and the caller has
 * nothing to send. 0 is never a valid body length, so it doubles as the error.
 */
size_t reflow_profiles_json(char *buf, size_t cap,
			    reflow_profile_lookup lookup, uint8_t count);

/*
 * The smallest cap that can hold a valid answer: {"profiles":[]} plus the
 * truncation marker, plus the NUL. Below this the function writes nothing.
 */
#define REFLOW_PROFILES_JSON_MIN (sizeof("{\"profiles\":[],\"truncated\":true}"))

#endif /* REFLOW_PROFILES_JSON_H_ */
