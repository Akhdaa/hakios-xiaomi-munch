#ifndef HAKIOS_FILEMAP_H
#define HAKIOS_FILEMAP_H

#include <stdint.h>

#define FILEMAP_MAGIC 0x3150414d464b4148ULL /* HAKFMAP1 */
#define FILEMAP_BLOCK_SIZE 4096u
#define FILEMAP_MAX_EXTENTS 4096u

struct filemap_extent {
	uint64_t physical, blocks;
};

struct filemap {
	uint64_t magic, disk_blocks, image_blocks, extent_count;
	struct filemap_extent extents[];
};

static inline int
filemap_valid(const struct filemap *map, uint64_t size, uint64_t disk_blocks)
{
	if (size < sizeof(*map) || map->magic != FILEMAP_MAGIC || map->disk_blocks != disk_blocks ||
	    !map->image_blocks || !map->extent_count || map->extent_count > FILEMAP_MAX_EXTENTS ||
	    map->extent_count > (size - sizeof(*map)) / sizeof(map->extents[0])) {
		return 0;
	}
	uint64_t checksums = size - sizeof(*map) - map->extent_count * sizeof(map->extents[0]);
	if (checksums % sizeof(uint32_t) || checksums / sizeof(uint32_t) != map->image_blocks) {
		return 0;
	}
	uint64_t remaining = map->image_blocks;
	for (uint64_t i = 0; i < map->extent_count; i++) {
		const struct filemap_extent *e = &map->extents[i];
		if (!e->blocks || e->blocks > remaining || e->physical >= disk_blocks ||
		    e->blocks > disk_blocks - e->physical) {
			return 0;
		}
		remaining -= e->blocks;
	}
	return remaining == 0;
}

static inline uint64_t
filemap_run(const struct filemap *map, uint64_t logical, uint64_t count, uint64_t *physical)
{
	for (uint64_t i = 0; i < map->extent_count; i++) {
		const struct filemap_extent *e = &map->extents[i];
		if (logical < e->blocks) {
			*physical = e->physical + logical;
			uint64_t available = e->blocks - logical;
			return count < available ? count : available;
		}
		logical -= e->blocks;
	}
	return 0;
}

#endif
