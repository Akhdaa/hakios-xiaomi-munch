/*
 * Boot-arg fb_stats=1 (MiniIOMobileFramebuffer.cpp): what the panel was shown, every five seconds -- the time from
 * each frame put on the panel to the next, in buckets and as median, 95th percentile and longest, beside what each
 * frame waited for the GPU and what copying it into the panel's memory took on the CPU. The arithmetic is here, plain C
 * with nothing of the kernel's, so that a test on the host checks it (tools/tests/fb_stats_test.c,
 * tools/tests/fb-stats-test.sh). Fixed-size: nothing is allocated where frames are shown.
 */
#ifndef MINI_FB_STATS_H
#define MINI_FB_STATS_H

#include <stdint.h>

#define FB_STATS_INTERVALS      512             /* kept a period for the percentiles; more are still bucketed */
#define FB_STATS_PAUSE_US       500000          /* from so long on, nothing was moving: a pause, not counted */

struct fb_stats {
	uint32_t        interval_us[FB_STATS_INTERVALS];
	uint32_t        kept;                   /* intervals in interval_us */
	uint32_t        counted;                /* intervals counted (kept or not) */
	uint32_t        bucket[4];              /* rounded to the millisecond: to 17, to 34, to 50, past 50 */
	uint32_t        longest_us;
	uint32_t        pauses;                 /* intervals of FB_STATS_PAUSE_US and more */
	uint32_t        frames;                 /* frames put on the panel */
	uint64_t        wait_us;                /* waiting for the GPU, all frames together */
	uint32_t        wait_max_us, timeouts;
	uint64_t        copy_us;                /* the CPU's copy into the panel's memory, all frames together */
	uint32_t        copy_max_us;
	uint32_t        settled;                /* the frame on the panel drawn again, filtered (fb_filter=2) */
};

static inline void
fb_stats_reset(struct fb_stats * s)
{
	uint8_t * p = (uint8_t *) s;

	for (unsigned long i = 0; i < sizeof(*s); i++) {
		p[i] = 0;
	}
}

/* One interval between two frames on the panel. */
static inline void
fb_stats_interval(struct fb_stats * s, uint32_t us)
{
	uint32_t ms = (us + 500) / 1000;

	if (us >= FB_STATS_PAUSE_US) {
		s->pauses++;
		return;
	}
	s->bucket[ms <= 17 ? 0 : ms <= 34 ? 1 : ms <= 50 ? 2 : 3]++;
	s->longest_us = us > s->longest_us ? us : s->longest_us;
	if (s->kept < FB_STATS_INTERVALS) {
		s->interval_us[s->kept++] = us;
	}
	s->counted++;
}

/* One frame put on the panel: what it waited for the GPU (timed out: the GPU was not done) and what its copy took. */
static inline void
fb_stats_frame(struct fb_stats * s, uint32_t wait_us, uint32_t timeouts, uint32_t copy_us)
{
	s->frames++;
	s->wait_us += wait_us;
	s->wait_max_us = wait_us > s->wait_max_us ? wait_us : s->wait_max_us;
	s->timeouts += timeouts;
	s->copy_us += copy_us;
	s->copy_max_us = copy_us > s->copy_max_us ? copy_us : s->copy_max_us;
}

/* Sorts the kept intervals (insertion sort: at most FB_STATS_INTERVALS, once a period). */
static inline void
fb_stats_sort(struct fb_stats * s)
{
	for (uint32_t i = 1; i < s->kept; i++) {
		uint32_t v = s->interval_us[i], j = i;

		while (j > 0 && s->interval_us[j - 1] > v) {
			s->interval_us[j] = s->interval_us[j - 1];
			j--;
		}
		s->interval_us[j] = v;
	}
}

/* The nearest-rank percentile (1 to 100) of the kept intervals, after fb_stats_sort(); 0 when there are none. */
static inline uint32_t
fb_stats_percentile(const struct fb_stats * s, uint32_t percent)
{
	uint32_t rank;

	if (s->kept == 0) {
		return 0;
	}
	rank = (uint32_t) (((uint64_t) percent * s->kept + 99) / 100);
	rank = rank == 0 ? 1 : rank > s->kept ? s->kept : rank;
	return s->interval_us[rank - 1];
}

#endif /* MINI_FB_STATS_H */
