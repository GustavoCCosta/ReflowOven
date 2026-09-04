/* SPDX-License-Identifier: Apache-2.0 */

#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>

#include "cmdparse.h"

/*
 * Longest token we are willing to look at. Every key and value this endpoint
 * understands is far shorter; anything longer cannot be a valid command, so it
 * is rejected instead of truncated. Truncation is how "startle" becomes
 * "start".
 */
#define TOKEN_MAX 16

static int hex_digit(char c)
{
	if (c >= '0' && c <= '9') {
		return c - '0';
	}
	if (c >= 'a' && c <= 'f') {
		return c - 'a' + 10;
	}
	if (c >= 'A' && c <= 'F') {
		return c - 'A' + 10;
	}
	return -1;
}

/*
 * Percent-decode [src, end) into out. '+' is left alone: this endpoint takes
 * no free text, and decoding it as a space would only add a second spelling
 * for values that must match exactly.
 */
static int decode(const char *src, const char *end, char *out, size_t out_sz)
{
	size_t n = 0;

	while (src < end) {
		char c = *src++;

		if (c == '%') {
			int hi, lo;

			if (end - src < 2) {
				return -EINVAL;
			}
			hi = hex_digit(*src++);
			lo = hex_digit(*src++);
			if (hi < 0 || lo < 0) {
				return -EINVAL;
			}
			c = (char)((hi << 4) | lo);
			/* %00 would end the token early: refuse it outright. */
			if (c == '\0') {
				return -EINVAL;
			}
		}
		if (n + 1 >= out_sz) {
			return -ENOSPC;
		}
		out[n++] = c;
	}
	out[n] = '\0';
	return 0;
}

/* Whole-string unsigned decimal. Anything else is malformed, not zero. */
static int parse_index(const char *s, int32_t *out)
{
	uint32_t v = 0;

	if (*s == '\0') {
		return -EINVAL;
	}
	for (; *s != '\0'; s++) {
		if (*s < '0' || *s > '9') {
			return -EINVAL;
		}
		v = v * 10U + (uint32_t)(*s - '0');
		if (v > 0xFFFFU) {
			return -EINVAL;
		}
	}
	*out = (int32_t)v;
	return 0;
}

static int command_id(const char *value, uint8_t *id)
{
	if (strcmp(value, "start") == 0) {
		*id = REFLOW_CMD_START;
	} else if (strcmp(value, "stop") == 0) {
		*id = REFLOW_CMD_STOP;
	} else if (strcmp(value, "clear") == 0) {
		*id = REFLOW_CMD_CLEAR_FAULT;
	} else if (strcmp(value, "profile") == 0) {
		*id = REFLOW_CMD_SELECT_PROFILE;
	} else {
		return -EINVAL;
	}
	return 0;
}

enum reflow_cmd_parse reflow_cmd_parse(const char *query, struct reflow_cmd *cmd)
{
	const char *p = query;
	bool have_id = false;
	bool have_arg = false;
	bool ambiguous = false;   /* a parameter appeared twice */
	bool malformed = false;   /* something in the query does not parse */
	bool saw_stop = false;
	uint8_t id = 0;
	int32_t arg = 0;

	if (query == NULL) {
		return REFLOW_CMD_PARSE_REJECT;
	}

	while (*p != '\0') {
		char key[TOKEN_MAX];
		char value[TOKEN_MAX];
		const char *seg_end = strchr(p, '&');
		const char *eq;

		if (seg_end == NULL) {
			seg_end = p + strlen(p);
		}
		eq = memchr(p, '=', (size_t)(seg_end - p));

		if (decode(p, eq != NULL ? eq : seg_end, key, sizeof(key)) != 0) {
			/*
			 * The key does not decode, so we cannot tell whether it
			 * was 'id'. Refuse the whole request rather than guess.
			 */
			malformed = true;
			key[0] = '\0';
		}
		if (eq == NULL ||
		    decode(eq + 1, seg_end, value, sizeof(value)) != 0) {
			malformed = true;
			value[0] = '\0';
		}

		if (strcmp(key, "id") == 0) {
			uint8_t this_id;

			if (have_id) {
				ambiguous = true;
			}
			have_id = true;

			if (command_id(value, &this_id) != 0) {
				malformed = true;
			} else {
				id = this_id;
				if (this_id == REFLOW_CMD_STOP) {
					saw_stop = true;
				}
			}
		} else if (strcmp(key, "arg") == 0) {
			if (have_arg) {
				ambiguous = true;
			}
			have_arg = true;

			if (parse_index(value, &arg) != 0) {
				malformed = true;
			}
		}
		/* Unknown keys are ignored: they are not commands. */

		p = (*seg_end == '&') ? seg_end + 1 : seg_end;
	}

	if (!have_id) {
		return REFLOW_CMD_PARSE_REJECT;
	}

	/*
	 * Range-check the profile index here, and not where arg is parsed, for
	 * two reasons. The query is order-independent, so at parse time we may
	 * not know yet which command the arg belongs to - and marking a bad arg
	 * malformed regardless of the command would drag "id=stop&arg=999" into
	 * the malformed path. It would still stop the oven, but the point of
	 * REJECT_STOP is that it is reached deliberately, not by accident.
	 *
	 * The count comes from profile.c, which is pure and already linked
	 * wherever this file is. Taking the limit as a parameter instead would
	 * push the decision back into httpd.c - out of the layer that exists to
	 * hold exactly this kind of decision (RFO-B10).
	 */
	/*
	 * RFO-B40. The missing arg is part of the same check, and it has to be:
	 * arg starts at 0, so a request that never mentioned it is
	 * indistinguishable from one that asked for index 0 by the time this
	 * line runs. Guarding the range on have_arg alone let
	 * "id=profile" - and "id=profile&argx=256", which is the same thing
	 * with a typo - answer 204 and switch the oven to profile 0, the
	 * 245 degC lead-free reflow. Not asking for a profile is not asking
	 * for the first one.
	 *
	 * The requirement is the profile command's alone. "id=stop" carries no
	 * arg and must stay valid: a validation patch is exactly where a stop
	 * gets swallowed by accident (RFO-B10).
	 */
	if (id == REFLOW_CMD_SELECT_PROFILE &&
	    (!have_arg || arg < 0 || arg >= (int32_t)reflow_profile_count())) {
		malformed = true;
	}

	if (ambiguous || malformed) {
		/*
		 * The request is not a command. If a stop was one of the
		 * readings, act on it anyway: refusing a stop is the one
		 * failure mode this endpoint must not have.
		 */
		if (saw_stop) {
			cmd->id = REFLOW_CMD_STOP;
			cmd->arg = 0;
			return REFLOW_CMD_PARSE_REJECT_STOP;
		}
		return REFLOW_CMD_PARSE_REJECT;
	}

	cmd->id = id;
	cmd->arg = (id == REFLOW_CMD_SELECT_PROFILE) ? arg : 0;
	return REFLOW_CMD_PARSE_OK;
}
