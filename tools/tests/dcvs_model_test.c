/*
 * metal-ca-perf, Property 1 (Bug Condition), C3: the GPU clock floor while the UI animates, against the very decision
 * the kernel takes (drivers/gpu/hakios.dcvs.h, dcvs_next(), called by hakios.adreno.c's hakios_gpu_dcvs every 50 ms).
 *
 * On munch (speeds_munch: 305, 400, 441.6, 490, 525, 587, 670 MHz) with gpu_anim_floor=587, every DCVS window while
 * animating -- 2 or more GPU submissions in the window, held for 250 ms (5 windows) after the last such window -- SHALL
 * end with a speed of at least min(587 MHz, the thermal cap): 490 MHz while hot (DCVS_HOT_KHZ), 670 otherwise.
 *
 * Scoped first: 50 ms windows at 60-80 % busy with 3 submissions each from 305 MHz; then generated: random busy
 * sequences (0-100 %), submission counts (0-5), hot flags and starting speeds.
 *
 * On the code before the fix dcvs_next ignores ANIMATING and FLOOR_INDEX (the seam) and steps up only past 85 % busy,
 * so this test FAILS: that is the bug (it holds 305 MHz).
 *
 *   cc -O2 -Wall -Wextra -Idrivers/gpu tools/tests/dcvs_model_test.c -o dcvs_model_test && ./dcvs_model_test
 * (tools/tests/dcvs-model-test.sh does that.)
 *
 * **Validates: Requirements 1.3, 2.5**
 */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include "hakios.dcvs.h"

static const uint32_t speeds_munch[] = { 0, 305000, 400000, 441600, 490000, 525000, 587000, 670000 };
#define LOW   1u
#define HIGH  7u
#define HOLD_WINDOWS 5u          /* 250 ms of 50 ms windows */
#define FLOOR_MHZ 587u

static int failed, checks;

/* gpu_anim_floor=MHz rounded up to a speeds_munch entry (the design's rule); 0 for no floor */
static uint32_t
floor_index_of(uint32_t mhz)
{
	if (mhz == 0) {
		return 0;
	}
	for (uint32_t i = LOW; i <= HIGH; i++) {
		if (speeds_munch[i] >= mhz * 1000u) {
			return i;
		}
	}
	return HIGH;
}

/* the highest index the thermal cap leaves */
static uint32_t
cap_index(bool hot)
{
	uint32_t high = HIGH;

	while (hot && high > LOW && speeds_munch[high] > DCVS_HOT_KHZ) {
		high--;
	}
	return high;
}

struct window {
	uint32_t busy, submissions;
	bool hot;
};

/*
 * Runs N windows from speed START through dcvs_next; returns the first window (0-based) whose chosen speed is below
 * min(floor, cap) while animating, -1 when none. *AT the speed chosen in that window, *TRACE the speeds chosen.
 */
static int
run(const struct window *w, unsigned n, uint32_t start, uint32_t floor_index, uint32_t *at, uint32_t *trace)
{
	uint32_t index = start, down = 0, since = HOLD_WINDOWS + 1;
	int bad = -1;

	for (unsigned i = 0; i < n; i++) {
		since = w[i].submissions >= 2 ? 0 : since + 1;
		const bool animating = since <= HOLD_WINDOWS;
		index = dcvs_next(speeds_munch, index, w[i].busy, &down, HIGH, LOW, w[i].hot, animating, floor_index);
		trace[i] = index;
		if (animating && floor_index != 0) {
			const uint32_t want = floor_index < cap_index(w[i].hot) ? floor_index : cap_index(w[i].hot);
			if (index < want && bad < 0) {
				bad = (int)i;
				*at = index;
			}
		}
	}
	return bad;
}

static void
print_trace(const struct window *w, unsigned n, const uint32_t *trace, unsigned upto)
{
	for (unsigned i = 0; i < n && i <= upto; i++) {
		printf("    window %u: busy %u %%, %u submissions%s -> %u MHz\n", i, w[i].busy, w[i].submissions, w[i].hot ? ", hot" : "",
		    speeds_munch[trace[i]] / 1000);
	}
}

static uint64_t seed = 0x9e3779b97f4a7c15ull;
static uint32_t
random32(void)
{
	seed ^= seed << 13;
	seed ^= seed >> 7;
	seed ^= seed << 17;
	return (uint32_t)(seed >> 32);
}

int
main(void)
{
	const uint32_t floor_index = floor_index_of(FLOOR_MHZ);
	uint32_t trace[64], at = 0;
	struct window w[64];

	checks++;
	if (speeds_munch[floor_index] != 587000) {
		failed++;
		printf("FAIL: gpu_anim_floor=587 rounds to %u MHz\n", speeds_munch[floor_index] / 1000);
	}

	/* scoped: 20 windows (1 s) of animation at 60-80 % busy, 3 submissions each, from 305 MHz, not hot */
	for (unsigned i = 0; i < 20; i++) {
		w[i].busy = 60 + (i * 7) % 21;
		w[i].submissions = 3;
		w[i].hot = false;
	}
	checks++;
	int bad = run(w, 20, LOW, floor_index, &at, trace);
	if (bad >= 0) {
		failed++;
		printf("FAIL (scoped): animating at 60-80 %% busy with 3 submissions a window and gpu_anim_floor=%u: window %d chose %u MHz, "
		    "below %u MHz; the speeds chosen over 1 s:", FLOOR_MHZ, bad, speeds_munch[at] / 1000, FLOOR_MHZ);
		for (unsigned i = 0; i < 20; i++) {
			printf(" %u", speeds_munch[trace[i]] / 1000);
		}
		printf("\n");
	}

	/* generated: 2000 sequences of 40 windows */
	int bug_cases = 0, failures = 0, printed = 0;
	for (unsigned round = 0; round < 2000; round++) {
		const unsigned n = 40;
		const uint32_t start = LOW + random32() % (HIGH - LOW + 1);
		const bool hot_run = random32() % 4 == 0;
		bool any_animating = false;
		for (unsigned i = 0; i < n; i++) {
			w[i].busy = random32() % 101;
			w[i].submissions = random32() % 3 == 0 ? 0 : random32() % 6;
			w[i].hot = hot_run ? random32() % 3 != 0 : false;
			any_animating = any_animating || w[i].submissions >= 2;
		}
		if (!any_animating) {
			continue;
		}
		bug_cases++;
		bad = run(w, n, start, floor_index, &at, trace);
		if (bad < 0) {
			continue;
		}
		failures++;
		if (printed++ < 3) {
			printf("  counterexample (round %u, from %u MHz): window %d animating chose %u MHz, below min(%u MHz, the cap)\n", round,
			    speeds_munch[start] / 1000, bad, speeds_munch[at] / 1000, FLOOR_MHZ);
			print_trace(w, n, trace, (unsigned)bad);
		}
	}
	checks++;
	if (failures != 0) {
		failed++;
		printf("FAIL (generated): %d of %d sequences with animation went below min(%u MHz, the cap) while animating\n", failures, bug_cases,
		    FLOOR_MHZ);
	}
	checks++;
	if (bug_cases < 1000) {
		failed++;
		printf("FAIL: the generator reached the bug condition in %d sequences only\n", bug_cases);
	}
	printf("dcvs_model_test: %d checks, %d failed\n", checks, failed);
	return failed != 0;
}
