/*
 * The "fb:" line's arithmetic (xnu/files/iokit/Kernel/MiniFBStats.h, boot-arg fb_stats=1) on this machine:
 * buckets, pauses, nearest-rank median and 95th percentile and the longest interval, first from fixed lists,
 * then for random lists against qsort and a direct count.
 *
 *   cc -O2 -Wall -Ixnu/files/iokit/Kernel tools/tests/fb_stats_test.c -o fb_stats_test && ./fb_stats_test
 * (tools/tests/fb-stats-test.sh does that.)
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include "MiniFBStats.h"

static int failed, checks;

#define CHECK(c, ...) do { checks++; if (!(c)) { failed++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static struct fb_stats s;

static int
by_value(const void * a, const void * b)
{
	uint32_t x = *(const uint32_t *) a, y = *(const uint32_t *) b;

	return x < y ? -1 : x > y;
}

static uint64_t seed = 88172645463325252ull;

static uint32_t
random32(void)
{
	seed ^= seed << 13;
	seed ^= seed >> 7;
	seed ^= seed << 17;
	return (uint32_t) (seed >> 32);
}

static void
fixed_lists(void)
{
	/* 60 Hz frames, one late by a refresh, one by two, a hitch and a pause */
	static const uint32_t list[] = { 16667, 16700, 16600, 33333, 16667, 50000, 16667, 120000, 16667, 900000 };

	fb_stats_reset(&s);
	for (unsigned i = 0; i < sizeof(list) / sizeof(list[0]); i++) {
		fb_stats_interval(&s, list[i]);
	}
	CHECK(s.counted == 9 && s.kept == 9, "counted %u kept %u", s.counted, s.kept);
	CHECK(s.pauses == 1, "pauses %u", s.pauses);
	CHECK(s.bucket[0] == 6 && s.bucket[1] == 1 && s.bucket[2] == 1 && s.bucket[3] == 1, "buckets %u %u %u %u",
	    s.bucket[0], s.bucket[1], s.bucket[2], s.bucket[3]);
	CHECK(s.longest_us == 120000, "longest %u", s.longest_us);
	fb_stats_sort(&s);
	CHECK(fb_stats_percentile(&s, 50) == 16667, "median %u", fb_stats_percentile(&s, 50));
	CHECK(fb_stats_percentile(&s, 95) == 120000, "95th %u", fb_stats_percentile(&s, 95));
	CHECK(fb_stats_percentile(&s, 100) == 120000, "100th %u", fb_stats_percentile(&s, 100));

	/* the bucket edges, rounded to the millisecond */
	fb_stats_reset(&s);
	fb_stats_interval(&s, 17499);
	fb_stats_interval(&s, 17500);
	fb_stats_interval(&s, 34499);
	fb_stats_interval(&s, 50499);
	fb_stats_interval(&s, 50500);
	fb_stats_interval(&s, 499999);
	fb_stats_interval(&s, 500000);
	CHECK(s.bucket[0] == 1 && s.bucket[1] == 2 && s.bucket[2] == 1 && s.bucket[3] == 2, "edges %u %u %u %u",
	    s.bucket[0], s.bucket[1], s.bucket[2], s.bucket[3]);
	CHECK(s.pauses == 1, "edge pause %u", s.pauses);

	/* none: zeros, no division */
	fb_stats_reset(&s);
	fb_stats_sort(&s);
	CHECK(fb_stats_percentile(&s, 50) == 0 && fb_stats_percentile(&s, 95) == 0, "empty");

	/* frames: averages and the most */
	fb_stats_frame(&s, 1000, 0, 3000);
	fb_stats_frame(&s, 5000, 1, 2000);
	CHECK(s.frames == 2 && s.wait_us == 6000 && s.wait_max_us == 5000 && s.timeouts == 1, "wait");
	CHECK(s.copy_us == 5000 && s.copy_max_us == 3000, "copy");

	/* more than are kept: all bucketed, the first FB_STATS_INTERVALS kept */
	fb_stats_reset(&s);
	for (unsigned i = 0; i < FB_STATS_INTERVALS + 100; i++) {
		fb_stats_interval(&s, 16667);
	}
	CHECK(s.kept == FB_STATS_INTERVALS && s.counted == FB_STATS_INTERVALS + 100 && s.bucket[0] == FB_STATS_INTERVALS + 100,
	    "overflow kept %u counted %u", s.kept, s.counted);
}

/* Property: for any list, buckets sum to the counted intervals, and the percentiles are qsort's nearest ranks. */
static void
random_lists(void)
{
	static uint32_t ref[FB_STATS_INTERVALS];

	for (unsigned round = 0; round < 2000; round++) {
		unsigned n = random32() % (FB_STATS_INTERVALS + 1), kept = 0, pauses = 0, b[4] = { 0 };
		uint32_t longest = 0;

		fb_stats_reset(&s);
		for (unsigned i = 0; i < n; i++) {
			uint32_t us = random32() % 4 == 0 ? random32() % 1000000 : 16000 + random32() % 40000;
			uint32_t ms = (us + 500) / 1000;

			fb_stats_interval(&s, us);
			if (us >= FB_STATS_PAUSE_US) {
				pauses++;
				continue;
			}
			b[ms <= 17 ? 0 : ms <= 34 ? 1 : ms <= 50 ? 2 : 3]++;
			longest = us > longest ? us : longest;
			ref[kept++] = us;
		}
		CHECK(s.kept == kept && s.counted == kept && s.pauses == pauses, "round %u: kept %u/%u pauses %u/%u", round, s.kept,
		    kept, s.pauses, pauses);
		CHECK(s.bucket[0] + s.bucket[1] + s.bucket[2] + s.bucket[3] == s.counted, "round %u: bucket sum", round);
		CHECK(s.bucket[0] == b[0] && s.bucket[1] == b[1] && s.bucket[2] == b[2] && s.bucket[3] == b[3], "round %u: buckets",
		    round);
		CHECK(s.longest_us == longest, "round %u: longest %u/%u", round, s.longest_us, longest);
		qsort(ref, kept, sizeof(ref[0]), by_value);
		fb_stats_sort(&s);
		for (unsigned i = 0; i < kept; i++) {
			if (s.interval_us[i] != ref[i]) {
				CHECK(0, "round %u: sorted %u: %u/%u", round, i, s.interval_us[i], ref[i]);
				break;
			}
		}
		if (kept != 0) {
			unsigned r50 = (50 * kept + 99) / 100, r95 = (95 * kept + 99) / 100;

			CHECK(fb_stats_percentile(&s, 50) == ref[r50 - 1], "round %u: median", round);
			CHECK(fb_stats_percentile(&s, 95) == ref[r95 - 1], "round %u: 95th", round);
			CHECK(fb_stats_percentile(&s, 95) <= s.longest_us, "round %u: 95th past the longest", round);
		}
	}
}

int
main(void)
{
	fixed_lists();
	random_lists();
	printf("fb_stats_test: %d checks, %d failed\n", checks, failed);
	return failed != 0;
}
