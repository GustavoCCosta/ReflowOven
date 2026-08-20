/* SPDX-License-Identifier: Apache-2.0 */

#include <stdbool.h>
#include <stddef.h>
#include <string.h>

#include "httpgate.h"

#define CMD_TARGET "/api/cmd"
#define CRLF       "\r\n"

/* A header value we accept at all. Longer than any of ours can legitimately be;
 * a longer one is refused rather than truncated, because truncating is how a
 * wrong token becomes a right prefix. */
#define VALUE_MAX 128

struct field {
	const char *val;
	size_t len;
	unsigned int seen;
};

static bool is_ows(char c)
{
	return c == ' ' || c == '\t';
}

static char lower(char c)
{
	return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
}

/* Case-insensitive compare of a length-delimited slice with a lowercase literal. */
static bool name_is(const char *s, size_t len, const char *lit)
{
	size_t i;

	for (i = 0; i < len; i++) {
		if (lit[i] == '\0' || lower(s[i]) != lit[i]) {
			return false;
		}
	}
	return lit[i] == '\0';
}

static bool same_bytes(const char *a, size_t alen, const char *b, size_t blen)
{
	return alen == blen && (alen == 0U || memcmp(a, b, alen) == 0);
}

/*
 * Fixed-iteration comparison against the configured token.
 *
 * The loop always runs strlen(want) times and never leaves early, so the time
 * does not depend on how many leading bytes the attacker guessed right. Reads
 * past the end of the received value are replaced by zero instead of being
 * performed. The length mismatch is folded into the same accumulator, so a value
 * with the correct prefix but the wrong length still fails.
 *
 * What this does NOT hide is the length of the secret, which the iteration count
 * reveals. That is the accepted trade in every constant-time comparison of this
 * shape, and it is not what an attacker needs.
 */
static bool token_ok(const char *got, size_t got_len, const char *want)
{
	size_t want_len = strlen(want);
	volatile unsigned char diff = (unsigned char)((got_len ^ want_len) != 0U);
	size_t i;

	for (i = 0; i < want_len; i++) {
		unsigned char g = (i < got_len) ? (unsigned char)got[i] : 0U;

		diff = (unsigned char)(diff | (g ^ (unsigned char)want[i]));
	}
	return diff == 0U;
}

/* One to three decimal digits, value 0..255. Returns the digits consumed, 0 on
 * refusal. Leading zeros are refused: "010" and "10" naming the same octet is
 * exactly the kind of second spelling that lets a check be bypassed. */
static size_t octet(const char *s, size_t len)
{
	unsigned int v = 0;
	size_t n = 0;

	while (n < len && n < 3U && s[n] >= '0' && s[n] <= '9') {
		v = v * 10U + (unsigned int)(s[n] - '0');
		n++;
	}
	if (n == 0U || v > 255U) {
		return 0U;
	}
	if (n > 1U && s[0] == '0') {
		return 0U;
	}
	return n;
}

/*
 * Is this authority a literal IPv4, with an optional port?
 *
 * This is the DNS rebinding mitigation. A name that resolves to the oven today
 * can resolve to an attacker tomorrow while the browser keeps treating the page
 * as same-origin, which puts threat A back on the table. An address literal
 * cannot be re-pointed.
 */
static bool is_ipv4_authority(const char *s, size_t len)
{
	size_t i = 0;
	int group;

	for (group = 0; group < 4; group++) {
		size_t n;

		if (group > 0) {
			if (i >= len || s[i] != '.') {
				return false;
			}
			i++;
		}
		n = octet(s + i, len - i);
		if (n == 0U) {
			return false;
		}
		i += n;
	}

	if (i == len) {
		return true;
	}
	if (s[i] != ':') {
		return false;
	}
	i++;
	/* Port: 1..5 digits, and it has to be the whole rest. */
	if (i == len || len - i > 5U) {
		return false;
	}
	for (; i < len; i++) {
		if (s[i] < '0' || s[i] > '9') {
			return false;
		}
	}
	return true;
}

/* Strip the scheme from an Origin, leaving the authority. Only http and https
 * are accepted; "null" and anything opaque falls through as a refusal. */
static bool origin_authority(const char *s, size_t len,
			     const char **auth, size_t *auth_len)
{
	static const char *const schemes[] = { "http://", "https://" };
	size_t k;

	for (k = 0; k < sizeof(schemes) / sizeof(schemes[0]); k++) {
		size_t sl = strlen(schemes[k]);

		if (len > sl && name_is(s, sl, schemes[k])) {
			*auth = s + sl;
			*auth_len = len - sl;
			return true;
		}
	}
	return false;
}

static enum reflow_gate collect(const char *hdr_start, const char *hdr_end,
				struct field *token, struct field *origin,
				struct field *host)
{
	const char *p = hdr_start;

	while (p < hdr_end) {
		const char *eol = strstr(p, CRLF);
		const char *colon;
		const char *name = p;
		size_t line_len, name_len, val_len;
		const char *val;
		struct field *f = NULL;

		if (eol == NULL || eol > hdr_end) {
			return REFLOW_GATE_BAD_REQUEST;
		}
		line_len = (size_t)(eol - p);
		if (line_len == 0U) {
			break;
		}

		colon = memchr(p, ':', line_len);
		if (colon == NULL) {
			/* No colon: not a header field. Obsolete line folding
			 * (a continuation starting with SP) lands here too, and
			 * is refused on purpose — RFC 7230 deprecated it and no
			 * browser sends it. */
			return REFLOW_GATE_BAD_REQUEST;
		}
		name_len = (size_t)(colon - name);
		if (name_len == 0U || is_ows(name[name_len - 1U])) {
			/* Whitespace before the colon must be rejected, not
			 * trimmed: it is a request smuggling primitive. */
			return REFLOW_GATE_BAD_REQUEST;
		}

		val = colon + 1;
		val_len = line_len - name_len - 1U;
		while (val_len > 0U && is_ows(val[0])) {
			val++;
			val_len--;
		}
		while (val_len > 0U && is_ows(val[val_len - 1U])) {
			val_len--;
		}

		if (name_is(name, name_len, "x-reflow-token")) {
			f = token;
		} else if (name_is(name, name_len, "origin")) {
			f = origin;
		} else if (name_is(name, name_len, "host")) {
			f = host;
		}

		if (f != NULL) {
			if (f->seen != 0U) {
				/* Two answers to the same question about an oven
				 * command is an error, not a thing to resolve.
				 * Same rule as the repeated 'id' of RFO-B01. */
				return REFLOW_GATE_BAD_REQUEST;
			}
			if (val_len > VALUE_MAX) {
				return REFLOW_GATE_BAD_REQUEST;
			}
			f->seen = 1U;
			f->val = val;
			f->len = val_len;
		}

		p = eol + 2;
	}

	return REFLOW_GATE_ALLOW;
}

enum reflow_gate reflow_gate_check(const char *request, const char *token)
{
	struct field tok = { NULL, 0U, 0U };
	struct field org = { NULL, 0U, 0U };
	struct field hst = { NULL, 0U, 0U };
	const char *line_end, *sp, *target, *target_end, *hdr_end;
	enum reflow_gate res;
	size_t method_len, target_len;

	if (request == NULL || token == NULL) {
		return REFLOW_GATE_BAD_REQUEST;
	}

	line_end = strstr(request, CRLF);
	if (line_end == NULL) {
		/* Not even a complete request line arrived. */
		return REFLOW_GATE_BAD_REQUEST;
	}

	sp = memchr(request, ' ', (size_t)(line_end - request));
	if (sp == NULL) {
		return REFLOW_GATE_BAD_REQUEST;
	}
	method_len = (size_t)(sp - request);

	target = sp + 1;
	target_end = memchr(target, ' ', (size_t)(line_end - target));
	if (target_end == NULL) {
		target_end = line_end;
	}
	{
		const char *q = memchr(target, '?', (size_t)(target_end - target));

		if (q != NULL) {
			target_end = q;
		}
	}
	target_len = (size_t)(target_end - target);

	/* Everything that is not the command endpoint passes: the GET routes are
	 * telemetry and cannot change the state of the oven. */
	if (!same_bytes(target, target_len, CMD_TARGET, strlen(CMD_TARGET))) {
		return REFLOW_GATE_ALLOW;
	}

	/* Method is checked before the token so that OPTIONS gets 405 even on a
	 * build with no token: a preflight that succeeds would undo the whole
	 * protection, and it must never look like it might. */
	if (!same_bytes(request, method_len, "POST", 4U)) {
		return REFLOW_GATE_METHOD_NOT_ALLOWED;
	}

	if (token[0] == '\0') {
		return REFLOW_GATE_DISABLED;
	}

	hdr_end = strstr(line_end, CRLF CRLF);
	if (hdr_end == NULL) {
		/* The header block did not finish inside what we read. There may
		 * be an Origin or a duplicate token in the part we never saw, so
		 * there is nothing to decide on: refuse. */
		return REFLOW_GATE_BAD_REQUEST;
	}

	res = collect(line_end + 2, hdr_end + 2, &tok, &org, &hst);
	if (res != REFLOW_GATE_ALLOW) {
		return res;
	}

	/* HTTP/1.1 requires Host, and without it neither the rebinding check nor
	 * the Origin comparison can be made. */
	if (hst.seen == 0U || hst.len == 0U) {
		return REFLOW_GATE_BAD_REQUEST;
	}
	if (!is_ipv4_authority(hst.val, hst.len)) {
		return REFLOW_GATE_FORBIDDEN;
	}

	if (org.seen != 0U) {
		const char *auth;
		size_t auth_len;

		if (!origin_authority(org.val, org.len, &auth, &auth_len)) {
			return REFLOW_GATE_FORBIDDEN;
		}
		if (!same_bytes(auth, auth_len, hst.val, hst.len)) {
			return REFLOW_GATE_FORBIDDEN;
		}
	}

	if (tok.seen == 0U || !token_ok(tok.val, tok.len, token)) {
		return REFLOW_GATE_UNAUTHORIZED;
	}

	return REFLOW_GATE_ALLOW;
}

const char *reflow_gate_status(enum reflow_gate verdict)
{
	switch (verdict) {
	case REFLOW_GATE_BAD_REQUEST:        return "400 Bad Request";
	case REFLOW_GATE_UNAUTHORIZED:       return "401 Unauthorized";
	case REFLOW_GATE_FORBIDDEN:          return "403 Forbidden";
	case REFLOW_GATE_METHOD_NOT_ALLOWED: return "405 Method Not Allowed";
	case REFLOW_GATE_DISABLED:           return "503 Service Unavailable";
	case REFLOW_GATE_ALLOW:
	default:                             return NULL;
	}
}
