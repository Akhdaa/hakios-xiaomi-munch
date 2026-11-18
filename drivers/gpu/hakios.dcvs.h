/*
 * The GPU's speed choice of hakios.adreno.c's DCVS (hakios_gpu_dcvs), one 50 ms window at a time, in plain C so
 * that a host test (tools/tests/dcvs_model_test.c) runs the very same decision.
 *
 * dcvs_next: KHZ the board's speeds (index 0 unused), INDEX the speed now, PERCENT the window's busy share, *DOWN_COUNT
 * the windows in a row below DCVS_DOWN so far (updated), HIGH and LOW the board's highest and lowest index, HOT the
 * thermal driver's verdict (hakios_adreno_hot(): no speed above DCVS_HOT_KHZ), ANIMATING whether the window had the
 * UI animating, FLOOR_INDEX the lowest speed while animating (0: none). Returns the speed to vote for (INDEX: no
 * change). Busier than DCVS_UP %: one speed up; less busy than DCVS_DOWN % twice in a row: one speed down.
 *
 * The floor (metal-ca-perf, boot-arg gpu_anim_floor): while ANIMATING and FLOOR_INDEX is not 0, the speed is at least
 * FLOOR_INDEX, capped by HIGH after the thermal cap -- so while hot at least the highest speed within DCVS_HOT_KHZ,
 * never above it (the thermal cap first). Not animating, or FLOOR_INDEX 0: the choice above, unchanged.
 */
#ifndef HAKIOS_DCVS_H
#define HAKIOS_DCVS_H
#ifndef KERNEL
#include <stdbool.h>
#include <stdint.h>
#endif

#define DCVS_UP                 85
#define DCVS_DOWN               45
#define DCVS_HOT_KHZ            490000

static inline uint32_t
dcvs_next(const uint32_t *khz, uint32_t index, uint32_t percent, uint32_t *down_count, uint32_t high, uint32_t low, bool hot,
    bool animating, uint32_t floor_index)
{
	uint32_t next = index;

	if (hot) {
		while (high > low && khz[high] > DCVS_HOT_KHZ) {
			high--;
		}
	}
	if (index > high) {
		next = high;
	} else if (percent > DCVS_UP && index < high) {
		next = index + 1;
		*down_count = 0;
	} else if (percent < DCVS_DOWN && index > low) {
		if (++*down_count >= 2) {
			next = index - 1;
			*down_count = 0;
		}
	} else {
		*down_count = 0;
	}
	if (animating && floor_index != 0) {
		const uint32_t floor = floor_index < high ? floor_index : high;
		if (next < floor) {
			next = floor;
			*down_count = 0;
		}
	}
	return next;
}
#endif /* HAKIOS_DCVS_H */
