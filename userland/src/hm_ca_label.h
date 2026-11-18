/* CoreAnimation's fragment variant labels and the stand-in for a label the library lacks (spec
 * metal-ca-reachability-pink). Plain C, no Objective-C, so the host tests can include it (as hm_ca_cover.h).
 *
 * A label is com.apple.coreanimation.frag.<spec>, and a spec is, by the plug-in's regular expression
 *   ^(T[a-z0-9]{3})?(B[a-z]{3})?(D[a-z]{2})?(A[0-9])(S[0-9])?(L[a-z]{2})?X([a-z]*)((?:_[A-Za-z0-9]+)*)$
 * T what is sampled, B a blend in the shader, D, A the varyings, S a second sampler, L, X the flags, then one
 * _I<kind><modifiers> term per input. Each optional part starts with a letter the next part cannot start with, so one
 * left-to-right pass decides the match as the regular expression does.
 *
 * HM_CA_LABEL_OLD is hm_ca_nearest of HakiosMetalCA.m before this spec, line for line: the same T, B, D, A, S, L and
 * number of inputs; then 100 plus the length difference per input of another kind, 3 per asked flag the candidate
 * lacks, 10 per flag it adds; the lowest cost, ties by name.
 *
 * HM_CA_LABEL_NEW is the same-effect rule of the stop gate (task 2) and task 5.1: a stand-in must have the asked
 * effect key (T, B, D, A, L and every input term, kind and modifiers, in order; S equal, or S1 on one side only when
 * neither label reads a destination attachment) and differ in flags only within the harmless set H. Among those it
 * prefers the asked S, then the old cost (3 per flag left out, 10 per flag added), then the name, so that a stand-in
 * of today that already qualifies is kept. When none qualifies it returns NULL with the reason (the D2 fallback). */
#ifndef HM_CA_LABEL_H
#define HM_CA_LABEL_H

#include <stddef.h>
#include <stdio.h>
#include <string.h>

#define HM_CA_LABEL_PREFIX "com.apple.coreanimation.frag."

/* WHY for a label that is not a fragment label of the grammar (hm_ca_nearest returned nil without a log line) */
#define HM_CA_LABEL_UNPARSED "does not parse"
/* WHY when the old rule finds no candidate (the text of hm_ca_nearest's log line) */
#define HM_CA_LABEL_NOTHING "nothing of the same kind"
/* WHY when the new rule finds no library label with the asked effect key (sampled source, blend, varyings, samplers,
 * every input's kind and modifiers) */
#define HM_CA_LABEL_NO_EFFECT "no variant with the same effect (source, blend, varyings, samplers and inputs)"
/* WHY when labels with the asked effect key exist but each differs in a flag outside H */
#define HM_CA_LABEL_FLAGS_OUTSIDE "only variants whose flags differ outside the harmless set"

/* H, the harmless differences between an asked label and its stand-in (inspection.md §3 and §9; design.md row H):
 * a flag added (+) or left out (-), or the second sampler S1 present on one side only. Two members hold only under a
 * condition the rule checks itself: ±c only when neither label reads buffer 4 colorP (flag g or an input modifier
 * N<digit>), ±S1 only when neither label reads a destination attachment (a B part, T Tbds/Taup/Tvcm, flag g, an input
 * Idst or an input modifier M<digit>). +n and ±g are not harmless. */
#define HM_CA_LABEL_HARMLESS "+c -c +x -x +u -u -n +S1 -S1"

enum hm_ca_label_mode { HM_CA_LABEL_OLD, HM_CA_LABEL_NEW };

struct hm_ca_label_span {
	const char *at;
	size_t len;     /* 0: the part is absent */
};

struct hm_ca_label {
	struct hm_ca_label_span part[6];   /* T, B, D, A, S, L (the expression's groups 1-6) */
	struct hm_ca_label_span flags;     /* the letters after X */
	struct hm_ca_label_span inputs;    /* "" or "_<term>_<term>..." (group 8) */
	size_t ninputs;                    /* terms in INPUTS */
};

static inline int
hm_ca_label_lower(char c)
{
	return c >= 'a' && c <= 'z';
}

static inline int
hm_ca_label_digit(char c)
{
	return c >= '0' && c <= '9';
}

static inline int
hm_ca_label_alnum(char c)
{
	return hm_ca_label_lower(c) || hm_ca_label_digit(c) || (c >= 'A' && c <= 'Z');
}

/* an optional part: LETTER then N characters each lower case (or a digit, when DIGITS is 1; only digits when it is 2) */
static inline size_t
hm_ca_label_take(const char *s, size_t at, size_t end, char letter, size_t n, int digits, struct hm_ca_label_span *out)
{
	out->at = s + at;
	out->len = 0;
	if (at + 1 + n > end || s[at] != letter) {
		return at;
	}
	for (size_t i = 1; i <= n; i++) {
		char c = s[at + i];

		if (!(digits == 2 ? hm_ca_label_digit(c) : hm_ca_label_lower(c) || (digits == 1 && hm_ca_label_digit(c)))) {
			return at;
		}
	}
	out->len = 1 + n;
	return at + 1 + n;
}

/* SPEC (without the prefix) into its parts; 1 if it matches the grammar, else 0. As ICU's '$', one line terminator
 * at the very end ("\n", "\r" or "\r\n") is allowed after the match. */
static inline int
hm_ca_label_parse(const char *spec, struct hm_ca_label *out)
{
	size_t end, at = 0;

	if (spec == NULL || out == NULL) {
		return 0;
	}
	memset(out, 0, sizeof(*out));
	end = strlen(spec);
	if (end >= 2 && spec[end - 2] == '\r' && spec[end - 1] == '\n') {
		end -= 2;
	} else if (end >= 1 && (spec[end - 1] == '\n' || spec[end - 1] == '\r')) {
		end -= 1;
	}
	at = hm_ca_label_take(spec, at, end, 'T', 3, 1, &out->part[0]);
	at = hm_ca_label_take(spec, at, end, 'B', 3, 0, &out->part[1]);
	at = hm_ca_label_take(spec, at, end, 'D', 2, 0, &out->part[2]);
	at = hm_ca_label_take(spec, at, end, 'A', 1, 2, &out->part[3]);
	if (out->part[3].len == 0) {
		return 0;
	}
	at = hm_ca_label_take(spec, at, end, 'S', 1, 2, &out->part[4]);
	at = hm_ca_label_take(spec, at, end, 'L', 2, 0, &out->part[5]);
	if (at >= end || spec[at] != 'X') {
		return 0;
	}
	at++;
	out->flags.at = spec + at;
	while (at < end && hm_ca_label_lower(spec[at])) {
		at++;
	}
	out->flags.len = (size_t)(spec + at - out->flags.at);
	out->inputs.at = spec + at;
	while (at < end) {
		size_t from;

		if (spec[at] != '_') {
			return 0;
		}
		from = ++at;
		while (at < end && hm_ca_label_alnum(spec[at])) {
			at++;
		}
		if (at == from) {
			return 0;
		}
		out->ninputs++;
	}
	out->inputs.len = (size_t)(spec + at - out->inputs.at);
	return 1;
}

static inline int
hm_ca_label_span_eq(struct hm_ca_label_span a, struct hm_ca_label_span b)
{
	return a.len == b.len && (a.len == 0 || memcmp(a.at, b.at, a.len) == 0);
}

static inline int
hm_ca_label_has_flag(struct hm_ca_label_span flags, char c)
{
	return flags.len != 0 && memchr(flags.at, c, flags.len) != NULL;
}

/* the term after *AT in INPUTS (which starts at '_'), *AT moved past it */
static inline struct hm_ca_label_span
hm_ca_label_next_input(struct hm_ca_label_span inputs, size_t *at)
{
	struct hm_ca_label_span t = { inputs.at + *at + 1, 0 };

	*at += 1;
	while (*at < inputs.len && inputs.at[*at] != '_') {
		(*at)++;
		t.len++;
	}
	return t;
}

/* hm_ca_nearest's cost of HAVE for WANT, or -1 when it is not a candidate at all */
static inline long
hm_ca_label_old_cost(const struct hm_ca_label *want, const struct hm_ca_label *have)
{
	long cost = 0;
	size_t wi = 0, hi = 0;

	for (int i = 0; i < 6; i++) {
		if (!hm_ca_label_span_eq(want->part[i], have->part[i])) {
			return -1;
		}
	}
	if (want->ninputs != have->ninputs) {
		return -1;
	}
	for (size_t i = 0; i < want->ninputs; i++) {
		/* (an input that does more than was asked for last: IsrcN3Oc3mtc4 is a colour matrix, not a plain sample) */
		struct hm_ca_label_span w = hm_ca_label_next_input(want->inputs, &wi), h = hm_ca_label_next_input(have->inputs, &hi);

		if (!hm_ca_label_span_eq(w, h)) {
			cost += 100 + (h.len > w.len ? (long)(h.len - w.len) : (long)(w.len - h.len));
		}
	}
	for (size_t i = 0; i < want->flags.len; i++) {
		cost += hm_ca_label_has_flag(have->flags, want->flags.at[i]) ? 0 : 3;
	}
	for (size_t i = 0; i < have->flags.len; i++) {
		cost += hm_ca_label_has_flag(want->flags, have->flags.at[i]) ? 0 : 10;
	}
	return cost;
}

/* 1 if an input term of L has LETTER followed by a digit among its modifiers (after the four characters of its kind) */
static inline int
hm_ca_label_input_mod(const struct hm_ca_label *l, char letter)
{
	size_t at = 0;

	for (size_t i = 0; i < l->ninputs; i++) {
		struct hm_ca_label_span t = hm_ca_label_next_input(l->inputs, &at);

		for (size_t k = 4; k + 1 < t.len; k++) {
			if (t.at[k] == letter && hm_ca_label_digit(t.at[k + 1])) {
				return 1;
			}
		}
	}
	return 0;
}

/* L reads buffer 4 colorP: flag g or an input modifier N<digit> */
static inline int
hm_ca_label_reads_colorp(const struct hm_ca_label *l)
{
	return hm_ca_label_has_flag(l->flags, 'g') || hm_ca_label_input_mod(l, 'N');
}

/* L reads a destination attachment: a B part, T Tbds/Taup/Tvcm, flag g, an input Idst or an input modifier M<digit> */
static inline int
hm_ca_label_reads_destination(const struct hm_ca_label *l)
{
	const struct hm_ca_label_span t = l->part[0];
	size_t at = 0;

	if (l->part[1].len != 0 || hm_ca_label_has_flag(l->flags, 'g')) {
		return 1;
	}
	if (t.len == 4 && (memcmp(t.at, "Tbds", 4) == 0 || memcmp(t.at, "Taup", 4) == 0 || memcmp(t.at, "Tvcm", 4) == 0)) {
		return 1;
	}
	for (size_t i = 0; i < l->ninputs; i++) {
		struct hm_ca_label_span in = hm_ca_label_next_input(l->inputs, &at);

		if (in.len >= 4 && memcmp(in.at, "Idst", 4) == 0) {
			return 1;
		}
	}
	return hm_ca_label_input_mod(l, 'M');
}

/* 1 if HARMLESS (space-separated members such as "+c" or "-S1") holds SIGN followed by WHAT */
static inline int
hm_ca_label_in_h(const char *harmless, char sign, const char *what)
{
	const size_t n = strlen(what);

	for (const char *p = harmless; *p != '\0';) {
		size_t len;

		while (*p == ' ') {
			p++;
		}
		len = strcspn(p, " ");
		if (len == n + 1 && p[0] == sign && memcmp(p + 1, what, n) == 0) {
			return 1;
		}
		p += len;
	}
	return 0;
}

/* The new rule for HAVE standing in for WANT: -1 when HAVE has another effect key (T, B, D, A, L, the input terms;
 * S equal, or S1 on one side only when H has it and neither label reads a destination); -2 when the key is the same
 * but a flag differs outside H; else 0 (the asked S) or 1 (S1 crossed), *COST set to the old flag cost. */
static inline int
hm_ca_label_new_fit(const struct hm_ca_label *want, const struct hm_ca_label *have, const char *harmless, long *cost)
{
	const struct hm_ca_label_span ws = want->part[4], hs = have->part[4];
	int colorp, crossed = 0;

	for (int i = 0; i < 6; i++) {
		if (i != 4 && !hm_ca_label_span_eq(want->part[i], have->part[i])) {
			return -1;
		}
	}
	if (!hm_ca_label_span_eq(want->inputs, have->inputs)) {
		return -1;
	}
	if (!hm_ca_label_span_eq(ws, hs)) {
		const int added = ws.len == 0 && hs.len == 2 && memcmp(hs.at, "S1", 2) == 0;
		const int dropped = hs.len == 0 && ws.len == 2 && memcmp(ws.at, "S1", 2) == 0;

		if ((!added && !dropped) || !hm_ca_label_in_h(harmless, added ? '+' : '-', "S1") ||
		    hm_ca_label_reads_destination(want) || hm_ca_label_reads_destination(have)) {
			return -1;
		}
		crossed = 1;
	}
	colorp = hm_ca_label_reads_colorp(want) || hm_ca_label_reads_colorp(have);
	*cost = 0;
	for (size_t i = 0; i < want->flags.len; i++) {   /* left out */
		const char c = want->flags.at[i], f[2] = { c, '\0' };

		if (hm_ca_label_has_flag(have->flags, c)) {
			continue;
		}
		if (!hm_ca_label_in_h(harmless, '-', f) || (c == 'c' && colorp)) {
			return -2;
		}
		*cost += 3;
	}
	for (size_t i = 0; i < have->flags.len; i++) {   /* added */
		const char c = have->flags.at[i], f[2] = { c, '\0' };

		if (hm_ca_label_has_flag(want->flags, c)) {
			continue;
		}
		if (!hm_ca_label_in_h(harmless, '+', f) || (c == 'c' && colorp)) {
			return -2;
		}
		*cost += 10;
	}
	return crossed;
}

/* The library label to compile for ASKED (a full label, com.apple.coreanimation.frag.<spec>) when the library has no
 * variant of that name. LIBRARY holds COUNT full labels, each with a variant (hm_ca_frag_vert's keys that
 * hm_ca_variants has); NULL entries and labels without the prefix are skipped. Returns one of LIBRARY's pointers, or
 * NULL with *WHY (when WHY is not NULL) saying why: HM_CA_LABEL_UNPARSED for a label outside the grammar, else the
 * reason nothing stands in. *WHY is NULL when a label is returned. HARMLESS is the new rule's set H in the form of
 * HM_CA_LABEL_HARMLESS (NULL: that constant; "": no flag difference and no S crossing qualifies); the old rule does
 * not read it. */
static inline const char *
hm_ca_label_nearest(const char *asked, const char *const *library, size_t count, const char *harmless,
		    enum hm_ca_label_mode mode, const char **why)
{
	const size_t plen = sizeof(HM_CA_LABEL_PREFIX) - 1;
	struct hm_ca_label want, have;
	const char *best = NULL;
	long bestCost = 0;

	if (why != NULL) {
		*why = HM_CA_LABEL_UNPARSED;
	}
	if (asked == NULL || strncmp(asked, HM_CA_LABEL_PREFIX, plen) != 0 || !hm_ca_label_parse(asked + plen, &want)) {
		return NULL;
	}
	if (mode == HM_CA_LABEL_NEW) {
		int bestCrossed = 0, sameKey = 0;

		if (harmless == NULL) {
			harmless = HM_CA_LABEL_HARMLESS;
		}
		for (size_t i = 0; i < count; i++) {
			const char *candidate = library != NULL ? library[i] : NULL;
			long cost = 0;
			int fit;

			if (candidate == NULL || strncmp(candidate, HM_CA_LABEL_PREFIX, plen) != 0 ||
			    !hm_ca_label_parse(candidate + plen, &have) || (fit = hm_ca_label_new_fit(&want, &have, harmless, &cost)) == -1) {
				continue;
			}
			sameKey = 1;
			if (fit < 0) {
				continue;
			}
			/* the asked S first, then the old cost, then the name */
			if (best == NULL || fit < bestCrossed || (fit == bestCrossed && cost < bestCost) ||
			    (fit == bestCrossed && cost == bestCost && strcmp(candidate, best) < 0)) {
				best = candidate;
				bestCost = cost;
				bestCrossed = fit;
			}
		}
		if (why != NULL) {
			*why = best != NULL ? NULL : sameKey ? HM_CA_LABEL_FLAGS_OUTSIDE : HM_CA_LABEL_NO_EFFECT;
		}
		return best;
	}
	for (size_t i = 0; i < count; i++) {
		const char *candidate = library != NULL ? library[i] : NULL;
		long cost;

		if (candidate == NULL || strncmp(candidate, HM_CA_LABEL_PREFIX, plen) != 0 ||
		    !hm_ca_label_parse(candidate + plen, &have) || (cost = hm_ca_label_old_cost(&want, &have)) < 0) {
			continue;
		}
		if (best == NULL || cost < bestCost || (cost == bestCost && strcmp(candidate, best) < 0)) {
			best = candidate;
			bestCost = cost;
		}
	}
	if (why != NULL) {
		*why = best != NULL ? NULL : HM_CA_LABEL_NOTHING;
	}
	return best;
}

/* What the plug-in does with a fragment label the library lacks, from hm_ca_label_nearest's GIVEN and WHY under MODE
 * (task 5.2, decision D2 of the stop gate: (c) skip the draw, log once per label; (a), a variant made from a generic
 * module, is deferred, so it is not an outcome here). */
enum hm_ca_label_outcome {
	HM_CA_LABEL_STAND_IN,      /* GIVEN is compiled in the asked label's place */
	HM_CA_LABEL_OWN_MODULE,    /* the function's own module, as before this spec: a label outside the grammar, or the old
				    * rule (metal_m2v=1, the opt-out) finding nothing */
	HM_CA_LABEL_LEAVE_OUT,     /* D2 (c): the new rule finds no same-effect variant; the pipeline is made with a fragment
				    * stage that writes nothing and its draws are left out, so the area shows what is under it */
};

static inline enum hm_ca_label_outcome
hm_ca_label_outcome_of(const char *given, const char *why, enum hm_ca_label_mode mode)
{
	if (given != NULL) {
		return HM_CA_LABEL_STAND_IN;
	}
	if (mode != HM_CA_LABEL_NEW || why == NULL || strcmp(why, HM_CA_LABEL_UNPARSED) == 0) {
		return HM_CA_LABEL_OWN_MODULE;
	}
	return HM_CA_LABEL_LEAVE_OUT;
}

/* The once-per-label line of HM_CA_LABEL_LEAVE_OUT into BUF (SIZE bytes, always terminated): what was asked (ASKED, a
 * full label or its spec), what is done and why (WHY), and the device's time of day (CLOCK, "hh:mm:ss"; NULL or "": left
 * out). Returns what snprintf returns. */
static inline int
hm_ca_label_leave_out_line(char *buf, size_t size, const char *asked, const char *why, const char *clock)
{
	const size_t plen = sizeof(HM_CA_LABEL_PREFIX) - 1;

	if (buf == NULL || size == 0) {
		return 0;
	}
	if (asked == NULL) {
		asked = "?";
	}
	if (strncmp(asked, HM_CA_LABEL_PREFIX, plen) == 0) {
		asked += plen;
	}
	return snprintf(buf, size, "left out: CoreAnimation's %s has no stand-in of the same effect (%s): its pipelines are made with a "
			"fragment stage that writes nothing and their draws are left out, the area shows what is under it (D2)%s%s\n",
			asked, why != NULL ? why : "no reason", clock != NULL && clock[0] != '\0' ? "; device time " : "",
			clock != NULL ? clock : "");
}

/* The rule the plug-in's draws use, from the boot arguments ARGS (kern.bootargs, space separated): HM_CA_LABEL_OLD when
 * one argument is exactly metal_ca_nearest=old (the opt-out, task 5.3), else HM_CA_LABEL_NEW. */
static inline enum hm_ca_label_mode
hm_ca_label_mode_from_bootargs(const char *args)
{
	static const char opt[] = "metal_ca_nearest=old";
	const size_t n = sizeof(opt) - 1;

	for (const char *p = args; p != NULL && *p != '\0';) {
		size_t len;

		while (*p == ' ' || *p == '\t' || *p == '\n') {
			p++;
		}
		len = strcspn(p, " \t\n");
		if (len == n && memcmp(p, opt, n) == 0) {
			return HM_CA_LABEL_OLD;
		}
		p += len;
	}
	return HM_CA_LABEL_NEW;
}

/* What GIVEN differs in from ASKED (full labels or their specs), into BUF (SIZE bytes, always terminated), for the
 * stand-in lines: "; "-separated items, a part as "T Tcim -> Timg" ("none" for an absent one), the flags as
 * "flags +c -n", the inputs as "input 1 Isqr -> Icir" (or "inputs 2 -> 1"); "same" when nothing differs, "unparsed"
 * when either label is outside the grammar. Returns the length written. */
static inline size_t
hm_ca_label_difference(const char *asked, const char *given, char *buf, size_t size)
{
	static const char names[6] = { 'T', 'B', 'D', 'A', 'S', 'L' };
	const size_t plen = sizeof(HM_CA_LABEL_PREFIX) - 1;
	struct hm_ca_label w, h;
	size_t at = 0;

#define HM_CA_LABEL_PUT(...) do { \
		if (at < size) { \
			int k = snprintf(buf + at, size - at, __VA_ARGS__); \
			if (k > 0) at = at + (size_t)k < size ? at + (size_t)k : size - 1; \
		} \
	} while (0)
	if (buf == NULL || size == 0) {
		return 0;
	}
	buf[0] = '\0';
	if (asked != NULL && strncmp(asked, HM_CA_LABEL_PREFIX, plen) == 0) asked += plen;
	if (given != NULL && strncmp(given, HM_CA_LABEL_PREFIX, plen) == 0) given += plen;
	if (!hm_ca_label_parse(asked, &w) || !hm_ca_label_parse(given, &h)) {
		HM_CA_LABEL_PUT("unparsed");
		return at;
	}
	for (int i = 0; i < 6; i++) {
		if (!hm_ca_label_span_eq(w.part[i], h.part[i])) {
			HM_CA_LABEL_PUT("%s%c %.*s -> %.*s", at ? "; " : "", names[i], w.part[i].len ? (int)w.part[i].len : 4,
					w.part[i].len ? w.part[i].at : "none", h.part[i].len ? (int)h.part[i].len : 4,
					h.part[i].len ? h.part[i].at : "none");
		}
	}
	{
		int any = 0;

		for (size_t i = 0; i < h.flags.len; i++) {
			if (!hm_ca_label_has_flag(w.flags, h.flags.at[i])) {
				HM_CA_LABEL_PUT("%s+%c", any ? " " : at ? "; flags " : "flags ", h.flags.at[i]);
				any = 1;
			}
		}
		for (size_t i = 0; i < w.flags.len; i++) {
			if (!hm_ca_label_has_flag(h.flags, w.flags.at[i])) {
				HM_CA_LABEL_PUT("%s-%c", any ? " " : at ? "; flags " : "flags ", w.flags.at[i]);
				any = 1;
			}
		}
	}
	if (w.ninputs != h.ninputs) {
		HM_CA_LABEL_PUT("%sinputs %zu -> %zu", at ? "; " : "", w.ninputs, h.ninputs);
	} else {
		size_t wi = 0, hi = 0;

		for (size_t i = 0; i < w.ninputs; i++) {
			struct hm_ca_label_span a = hm_ca_label_next_input(w.inputs, &wi), b = hm_ca_label_next_input(h.inputs, &hi);

			if (!hm_ca_label_span_eq(a, b)) {
				HM_CA_LABEL_PUT("%sinput %zu %.*s -> %.*s", at ? "; " : "", i + 1, (int)a.len, a.at, (int)b.len, b.at);
			}
		}
	}
	if (at == 0) {
		HM_CA_LABEL_PUT("same");
	}
#undef HM_CA_LABEL_PUT
	return at;
}

#endif /* HM_CA_LABEL_H */
