/*
 * storage-map-probe: where a file on Android's /data lies on the userdata partition, for HakiOS's storage boot
 * (tools/storage-boot/README.md; the loader reads the system image through that map, xnuloader/loader.c
 * block_start_file()). Run on the phone as root. It only reads.
 *
 *   storage-map-probe [--full] [--map] [IMAGE]       IMAGE defaults to /data/media/0/HakiOS/system.apfs
 *
 * The extents come from FS_IOC_FIEMAP; every one must be plain data, 4 KiB aligned, not encrypted, not inline,
 * not unwritten, not delayed. Then blocks of the file are read both through the file and straight from
 * /dev/block/by-name/userdata at the mapped place, and must be equal (--full: every block; else 64 spread samples
 * and both ends): that proves the map and that the data is not encrypted on the way (file-based encryption would
 * make them differ). The file must not change while this runs. With --map the map is printed for
 * munch-storage-payload.py:
 *   filemap1 <userdata blocks> <image blocks>
 *   extent <first userdata block> <blocks>          (one line per run of blocks, in the file's order)
 *   filemap-end
 * Blocks are 4096 bytes. Build: aarch64-linux-gnu-gcc -static -O2 -o storage-map-probe storage-map-probe.c
 * (or the NDK's aarch64-linux-android21-clang). Written for this repository; the earlier binary had no source here.
 */
#include <errno.h>
#include <fcntl.h>
#include <linux/fiemap.h>
#include <linux/fs.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <unistd.h>

#define BLOCK 4096u
#define MAX_EXTENTS 4096u
#define SAMPLES 64u
#define USERDATA "/dev/block/by-name/userdata"

struct run {
	uint64_t logical, physical, blocks;     /* in blocks */
};

static struct run runs[MAX_EXTENTS];
static unsigned nruns;

static int
fail(const char *what)
{
	fprintf(stderr, "storage-map-probe: %s%s%s\n", what, errno ? ": " : "", errno ? strerror(errno) : "");
	return 1;
}

static int
read_at(int fd, uint64_t block, unsigned char *buf)
{
	ssize_t n = pread(fd, buf, BLOCK, (off_t)(block * BLOCK));

	return n == (ssize_t)BLOCK ? 0 : -1;
}

static int
add_extent(uint64_t logical, uint64_t physical, uint64_t length, uint32_t flags)
{
	const uint32_t refused = FIEMAP_EXTENT_UNKNOWN | FIEMAP_EXTENT_DELALLOC | FIEMAP_EXTENT_ENCODED |
	    FIEMAP_EXTENT_DATA_ENCRYPTED | FIEMAP_EXTENT_NOT_ALIGNED | FIEMAP_EXTENT_DATA_INLINE | FIEMAP_EXTENT_DATA_TAIL |
	    FIEMAP_EXTENT_UNWRITTEN;

	if (flags & refused) {
		errno = 0;
		fprintf(stderr, "storage-map-probe: an extent with flags %#x (encrypted, inline, unwritten or not aligned)\n", flags);
		return -1;
	}
	if (logical % BLOCK || physical % BLOCK || length % BLOCK || length == 0) {
		errno = 0;
		fprintf(stderr, "storage-map-probe: an extent not on 4 KiB blocks\n");
		return -1;
	}
	logical /= BLOCK, physical /= BLOCK, length /= BLOCK;
	if (nruns > 0) {
		struct run *last = &runs[nruns - 1];

		if (last->logical + last->blocks != logical) {
			errno = 0;
			fprintf(stderr, "storage-map-probe: the file has a hole at block %llu\n", (unsigned long long)(last->logical + last->blocks));
			return -1;
		}
		if (last->physical + last->blocks == physical) {
			last->blocks += length;         /* (contiguous on the disk too: one run) */
			return 0;
		}
	} else if (logical != 0) {
		errno = 0;
		fprintf(stderr, "storage-map-probe: the file does not start with data\n");
		return -1;
	}
	if (nruns == MAX_EXTENTS) {
		errno = 0;
		fprintf(stderr, "storage-map-probe: more than %u runs (copy the file again to have it less fragmented)\n", MAX_EXTENTS);
		return -1;
	}
	runs[nruns++] = (struct run){ logical, physical, length };
	return 0;
}

static int
map_file(int fd, uint64_t size)
{
	enum { BATCH = 256 };
	size_t bytes = sizeof(struct fiemap) + BATCH * sizeof(struct fiemap_extent);
	struct fiemap *fm = calloc(1, bytes);
	uint64_t start = 0;
	int last = 0;

	if (fm == NULL) {
		return -1;
	}
	while (!last && start < size) {
		memset(fm, 0, bytes);
		fm->fm_start = start;
		fm->fm_length = size - start;
		fm->fm_flags = FIEMAP_FLAG_SYNC;
		fm->fm_extent_count = BATCH;
		if (ioctl(fd, FS_IOC_FIEMAP, fm) != 0) {
			free(fm);
			return -1;
		}
		if (fm->fm_mapped_extents == 0) {
			break;
		}
		for (unsigned i = 0; i < fm->fm_mapped_extents; i++) {
			struct fiemap_extent *e = &fm->fm_extents[i];

			if (add_extent(e->fe_logical, e->fe_physical, e->fe_length, e->fe_flags & ~FIEMAP_EXTENT_LAST) != 0) {
				free(fm);
				return -1;
			}
			start = e->fe_logical + e->fe_length;
			last |= (e->fe_flags & FIEMAP_EXTENT_LAST) != 0;
		}
	}
	free(fm);
	return 0;
}

static uint64_t
physical_of(uint64_t block)
{
	for (unsigned i = 0; i < nruns; i++) {
		if (block >= runs[i].logical && block < runs[i].logical + runs[i].blocks) {
			return runs[i].physical + (block - runs[i].logical);
		}
	}
	return UINT64_MAX;
}

int
main(int argc, char **argv)
{
	const char *image = "/data/media/0/HakiOS/system.apfs";
	int full = 0, print_map = 0, img, disk;
	unsigned char a[BLOCK], b[BLOCK];
	struct stat before, after;
	uint64_t disk_bytes = 0, blocks, mapped = 0, checks = 0;

	for (int i = 1; i < argc; i++) {
		if (strcmp(argv[i], "--full") == 0) {
			full = 1;
		} else if (strcmp(argv[i], "--map") == 0) {
			print_map = 1;
		} else if (argv[i][0] != '-') {
			image = argv[i];
		} else {
			fprintf(stderr, "usage: storage-map-probe [--full] [--map] [IMAGE]\n");
			return 2;
		}
	}
	errno = 0;
	if ((img = open(image, O_RDONLY)) < 0 || fstat(img, &before) != 0) {
		return fail("open the image");
	}
	if (before.st_size <= 0 || before.st_size % BLOCK) {
		errno = 0;
		return fail("the image is not a whole number of 4 KiB blocks");
	}
	blocks = (uint64_t)before.st_size / BLOCK;
	if ((disk = open(USERDATA, O_RDONLY)) < 0 || ioctl(disk, BLKGETSIZE64, &disk_bytes) != 0 || disk_bytes < BLOCK) {
		return fail("open " USERDATA);
	}
	if (map_file(img, (uint64_t)before.st_size) != 0) {
		return fail("map the image");
	}
	for (unsigned i = 0; i < nruns; i++) {
		mapped += runs[i].blocks;
		if (runs[i].physical + runs[i].blocks > disk_bytes / BLOCK) {
			errno = 0;
			return fail("an extent lies past the end of userdata");
		}
	}
	if (mapped != blocks) {
		errno = 0;
		return fail("the extents do not cover the whole image");
	}
	/* the same bytes through the file and straight from the partition */
	for (uint64_t k = 0; k < (full ? blocks : SAMPLES + 2); k++) {
		uint64_t blk = full ? k : k == SAMPLES ? 0 : k == SAMPLES + 1 ? blocks - 1 : k * (blocks / SAMPLES);
		uint64_t phys = physical_of(blk);

		if (phys == UINT64_MAX || read_at(img, blk, a) != 0 || read_at(disk, phys, b) != 0) {
			return fail("read a block");
		}
		if (memcmp(a, b, BLOCK) != 0) {
			errno = 0;
			fprintf(stderr, "storage-map-probe: block %llu differs between the file and userdata (encrypted storage?): "
			    "the map is refused\n", (unsigned long long)blk);
			return 1;
		}
		checks++;
	}
	if (fstat(img, &after) != 0 || after.st_size != before.st_size || after.st_mtime != before.st_mtime ||
	    after.st_ino != before.st_ino) {
		errno = 0;
		return fail("the image changed while it was checked");
	}
	if (print_map) {
		printf("filemap1 %llu %llu\n", (unsigned long long)(disk_bytes / BLOCK), (unsigned long long)blocks);
		for (unsigned i = 0; i < nruns; i++) {
			printf("extent %llu %llu\n", (unsigned long long)runs[i].physical, (unsigned long long)runs[i].blocks);
		}
		printf("filemap-end\n");
	} else {
		printf("image_bytes=%lld runs=%u blocks_compared=%llu raw_%s_match=yes\n", (long long)before.st_size, nruns,
		    (unsigned long long)checks, full ? "full" : "sample");
	}
	return 0;
}
