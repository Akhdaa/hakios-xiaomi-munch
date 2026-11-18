/*
 * metal-ca-perf, Property 2 (Preservation), the GPU clock: outside the bug condition the kernel's speed choice
 * (drivers/gpu/hakios.dcvs.h, dcvs_next) is today's.
 *
 * Today's decision, observed on the code before the fix and written down here as todays_dcvs() (the body of
 * hakios_gpu_dcvs's choice as task 3 moved it into the header: the thermal cap, one speed up past DCVS_UP %, one down
 * after two windows in a row under DCVS_DOWN %), and as literal speed sequences for fixed busy patterns.
 *
 * Properties, over generated busy sequences (0-100 %), submission counts, hot flags and starting speeds:
 * - not animating (any floor), or floor 0 (animating or not), on munch's speeds: the speed and the down count after
 *   every window are today's;
 * - off munch (redwood's speeds; hakios.adreno.c passes floor 0 there): today's;
 * - hot: never above DCVS_HOT_KHZ (490 MHz), whatever the floor (587, 670) or the animation -- the thermal cap first.
 * These pass on the unfixed code and are to pass, unchanged, after task 5.4 (task 5.6).
 *
 *   cc -O2 -Wall -Wextra -Idrivers/gpu tools/tests/dcvs_preservation_test.c -o dcvs_preservation_test && ./dcvs_preservation_test
 * (tools/tests/dcvs-preservation-test.sh does that.)
 *
 * **Validates: Requirements 3.2, 3.7, 3.8**
 */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include "hakios.dcvs.h"

static const uint32_t speeds_munch[] = { 0, 305000, 400000, 441600, 490000, 525000, 587000, 670000 };
static const uint32_t speeds_redwood[] = { 0, 315000, 450000, 550000 };
#define HOLD_WINDOWS 5u          /* 250 ms of 50 ms windows */

static int failed, checks;

/* today's choice (85 % up, 45 % down twice, the thermal cap), as observed before the fix */
static uint32_t
todays_dcvs(const uint32_t *khz, uint32_t index, uint32_t percent, uint32_t *down_count, uint32_t high, uint32_t low, bool hot)
{
	uint32_t next = index;

	if (hot) {
		while (high > low && khz[high] > 490000) {
			high--;
		}
	}
	if (index > high) {
		next = high;
	} else if (percent > 85 && index < high) {
		next = index + 1;
		*down_count = 0;
	} else if (percent < 45 && index > low) {
		if (++*down_count >= 2) {
			next = index - 1;
			*down_count = 0;
		}
	} else {
		*down_count = 0;
	}
	return next;
}

static uint64_t seed = 0x2545f4914f6cdd1dull;
static uint32_t
random32(void)
{
	seed ^= seed << 13;
	seed ^= seed >> 7;
	seed ^= seed << 17;
	return (uint32_t)(seed >> 32);
}

struct window {
	uint32_t busy, submissions;
	bool hot;
};

enum mode { NOT_ANIMATING, FLOOR_ZERO, OFF_MUNCH, HOT_ANY_FLOOR };

/*
 * One sequence of N windows from START through dcvs_next and todays_dcvs side by side. Returns the first window where
 * the property of MODE breaks, -1 when none.
 */
static int
compare(enum mode m, const struct window *w, unsigned n, uint32_t start, uint32_t floor_index, uint32_t *got, uint32_t *want)
{
	const uint32_t *khz = m == OFF_MUNCH ? speeds_redwood : speeds_munch;
	const uint32_t high = m == OFF_MUNCH ? 3 : 7, low = 1;
	uint32_t a = start, b = start, down_a = 0, down_b = 0, since = HOLD_WINDOWS + 1;

	for (unsigned i = 0; i < n; i++) {
		since = w[i].submissions >= 2 ? 0 : since + 1;
		const bool animating = m == NOT_ANIMATING ? false : since <= HOLD_WINDOWS;
		const uint32_t fl = m == NOT_ANIMATING || m == HOT_ANY_FLOOR ? floor_index : 0;
		a = dcvs_next(khz, a, w[i].busy, &down_a, high, low, w[i].hot, animating, fl);
		b = todays_dcvs(khz, b, w[i].busy, &down_b, high, low, w[i].hot);
		if (m == HOT_ANY_FLOOR) {
			if (w[i].hot && khz[a] > 490000) {
				*got = a;
				*want = b;
				return (int)i;
			}
		} else if (a != b || down_a != down_b) {
			*got = a;
			*want = b;
			return (int)i;
		}
	}
	return -1;
}

/* a fixed busy pattern from START on munch, not animating, against its literal speeds (MHz) */
static void
literal(const char *what, const uint32_t *busy, unsigned n, uint32_t start, bool hot, bool animating, uint32_t floor_index, const uint32_t *mhz)
{
	uint32_t index = start, down = 0;

	checks++;
	for (unsigned i = 0; i < n; i++) {
		index = dcvs_next(speeds_munch, index, busy[i], &down, 7, 1, hot, animating, floor_index);
		if (speeds_munch[index] / 1000 != mhz[i]) {
			failed++;
			printf("FAIL (%s): window %u chose %u MHz, observed before the fix %u MHz\n", what, i, speeds_munch[index] / 1000, mhz[i]);
			return;
		}
	}
}

int
main(void)
{
	static const char *const names[] = { "not animating (any floor)", "floor 0", "off munch (redwood's speeds)", "hot, any floor" };
	struct window w[64];
	uint32_t got = 0, want = 0;

	/* observed: busy 90 % for 7 windows from 305 MHz, then 10 % for 8: one step up a window to the top, then one down every second window */
	{
		const uint32_t busy[15] = { 90, 90, 90, 90, 90, 90, 90, 10, 10, 10, 10, 10, 10, 10, 10 };
		const uint32_t mhz[15] = { 400, 441, 490, 525, 587, 670, 670, 670, 587, 587, 525, 525, 490, 490, 441 };
		literal("ramp up and down, not animating", busy, 15, 1, false, false, 6, mhz);
		literal("ramp up and down, floor 0", busy, 15, 1, false, true, 0, mhz);
	}
	/* observed: the scoped animation load (60-80 % busy) without animation: 305 MHz throughout */
	{
		uint32_t busy[20], mhz[20];
		for (unsigned i = 0; i < 20; i++) {
			busy[i] = 60 + (i * 7) % 21;
			mhz[i] = 305;
		}
		literal("60-80 % busy, not animating", busy, 20, 1, false, false, 6, mhz);
	}
	/* observed: hot from 670 MHz at 95 % busy: 490 MHz at once and held */
	{
		const uint32_t busy[6] = { 95, 95, 95, 95, 95, 95 };
		const uint32_t mhz[6] = { 490, 490, 490, 490, 490, 490 };
		literal("hot at 95 % busy from 670 MHz", busy, 6, 7, true, false, 0, mhz);
	}

	for (int m = NOT_ANIMATING; m <= HOT_ANY_FLOOR; m++) {
		int cases = 0, bad = 0, printed = 0;
		for (unsigned round = 0; round < 2000; round++) {
			const unsigned n = 40;
			const uint32_t high = m == OFF_MUNCH ? 3 : 7;
			const uint32_t start = 1 + random32() % high;
			const bool hot_run = m == HOT_ANY_FLOOR ? true : random32() % 4 == 0;
			const uint32_t floor_index = m == HOT_ANY_FLOOR ? 6 + random32() % 2 : 1 + random32() % 7;   /* 587 or 670; any */
			for (unsigned i = 0; i < n; i++) {
				w[i].busy = random32() % 101;
				w[i].submissions = random32() % 3 == 0 ? 0 : random32() % 6;
				w[i].hot = hot_run ? random32() % 3 != 0 : false;
			}
			cases++;
			const int at = compare((enum mode)m, w, n, start, floor_index, &got, &want);
			if (at < 0) {
				continue;
			}
			bad++;
			if (printed++ < 3) {
				const uint32_t *khz = m == OFF_MUNCH ? speeds_redwood : speeds_munch;
				printf("  counterexample (%s, round %u, from %u MHz): window %d chose %u MHz, today's %u MHz\n", names[m], round,
				    khz[start] / 1000, at, khz[got] / 1000, khz[want] / 1000);
			}
		}
		checks++;
		if (bad != 0) {
			failed++;
			printf("FAIL (%s): %d of %d sequences differ from today's\n", names[m], bad, cases);
		} else {
			printf("  %s: %d sequences of 40 windows, all as today's\n", names[m], cases);
		}
	}
	printf("dcvs_preservation_test: %d checks, %d failed\n", checks, failed);
	return failed != 0;
}
