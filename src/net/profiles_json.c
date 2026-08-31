/* SPDX-License-Identifier: Apache-2.0 */

#include <stdbool.h>
#include <string.h>

#include "profiles_json.h"

static const char json_head[] = "{\"profiles\":[";
static const char json_tail[] = "]}";
static const char json_tail_truncated[] = "],\"truncated\":true}";

/*
 * Deliberately no snprintf here, in any form.
 *
 * The defect being fixed is what snprintf's return value means: it reports what
 * would have been written, so every "advance by the return value" is a trap one
 * long name away from writing out of bounds. These helpers append and report
 * whether they fit, and the length they maintain only ever counts bytes that
 * actually landed in the buffer.
 */
struct out {
	char *buf;
	size_t cap;   /* total size of buf, including room for the NUL */
	size_t len;   /* bytes written so far, never >= cap */
};

/*
 * Append s, but only if it leaves at least `reserve` bytes free on top of the
 * terminating NUL. The reserve is how the closing brace is guaranteed a place
 * to go: the entries are written knowing the tail still has to fit.
 *
 * Returns false and writes nothing at all on failure - partial appends are what
 * would produce half an entry in the output.
 */
static bool put(struct out *o, const char *s, size_t reserve)
{
	size_t n = strlen(s);

	if (n + reserve + 1U > o->cap - o->len) {
		return false;
	}
	memcpy(o->buf + o->len, s, n);
	o->len += n;
	o->buf[o->len] = '\0';
	return true;
}

/*
 * Append one character under the same rule as put().
 */
static bool put_ch(struct out *o, char c, size_t reserve)
{
	if (1U + reserve + 1U > o->cap - o->len) {
		return false;
	}
	o->buf[o->len++] = c;
	o->buf[o->len] = '\0';
	return true;
}

/*
 * Append s as a quoted JSON string.
 *
 * The escaping is not decoration: a profile name reaches this function from a
 * table that is compile-time today and editable tomorrow (RFO-F01 and the
 * custom-profile roadmap), and a name holding a quote or a backslash would
 * otherwise produce a body that is not JSON - served to a browser. Control
 * characters go out as \uXXXX because JSON forbids them raw.
 */
static bool put_json_string(struct out *o, const char *s, size_t reserve)
{
	static const char hex[] = "0123456789abcdef";

	if (!put_ch(o, '"', reserve)) {
		return false;
	}
	for (; *s != '\0'; s++) {
		unsigned char c = (unsigned char)*s;

		if (c == '"' || c == '\\') {
			if (!put_ch(o, '\\', reserve) || !put_ch(o, (char)c, reserve)) {
				return false;
			}
		} else if (c < 0x20U) {
			char esc[7] = { '\\', 'u', '0', '0', 0, 0, '\0' };

			esc[4] = hex[(c >> 4) & 0x0fU];
			esc[5] = hex[c & 0x0fU];
			if (!put(o, esc, reserve)) {
				return false;
			}
		} else {
			if (!put_ch(o, (char)c, reserve)) {
				return false;
			}
		}
	}
	return put_ch(o, '"', reserve);
}

size_t reflow_profiles_json(char *buf, size_t cap,
			    reflow_profile_lookup lookup, uint8_t count)
{
	/* Room the closing brace needs; the widest of the two tails. */
	const size_t reserve = sizeof(json_tail_truncated) - 1U;
	struct out o;
	bool truncated = false;
	uint8_t emitted = 0;

	if (buf == NULL || cap == 0U) {
		return 0;
	}
	buf[0] = '\0';

	if (cap < REFLOW_PROFILES_JSON_MIN || lookup == NULL) {
		return 0;
	}

	o.buf = buf;
	o.cap = cap;
	o.len = 0;

	/* Guaranteed by the cap check above, but say it in code, not in prose. */
	if (!put(&o, json_head, reserve)) {
		buf[0] = '\0';
		return 0;
	}

	for (uint8_t i = 0; i < count; i++) {
		const struct reflow_profile *p = lookup(i);
		size_t before = o.len;

		if (p == NULL) {
			/* Short table: stop, and do not call it truncation. */
			break;
		}

		if (emitted > 0 && !put_ch(&o, ',', reserve)) {
			o.len = before;
			o.buf[o.len] = '\0';
			truncated = true;
			break;
		}
		if (!put_json_string(&o, p->name != NULL ? p->name : "", reserve)) {
			/*
			 * Roll back to before this entry. Half a name is not a
			 * shorter list, it is malformed JSON.
			 */
			o.len = before;
			o.buf[o.len] = '\0';
			truncated = true;
			break;
		}
		emitted++;
	}

	/* The reserve above exists precisely so this cannot fail. */
	(void)put(&o, truncated ? json_tail_truncated : json_tail, 0);

	return o.len;
}
