/*
 * hfsclone - Dump only HFS+ metadata regions into a sparse image
 * This file is part of the hfsfuse project.
 */

#include "hfsuser.h"

#ifndef HFSFUSE_VERSION_STRING
#include "version.h"
#endif

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define COPY_CHUNK (1U << 20)

struct copy_stats {
	uint64_t regions;
	uint64_t bytes;
};

static int write_all_at(int fd, const void* data, size_t size, uint64_t offset) {
	const char* p = (const char*)data;
	while(size > 0) {
		ssize_t written = pwrite(fd, p, size, (off_t)offset);
		if(written < 0) {
			if(errno == EINTR)
				continue;
			return -errno;
		}
		if(written == 0)
			return -EIO;
		offset += (uint64_t)written;
		p += written;
		size -= (size_t)written;
	}
	return 0;
}

static int copy_region(hfs_volume* vol, int outfd, uint64_t offset, uint64_t size, struct copy_stats* st) {
	if(size == 0)
		return 0;

	char* buf = malloc(COPY_CHUNK);
	if(!buf)
		return -ENOMEM;

	uint64_t copied = 0;
	int ret = 0;
	while(copied < size) {
		uint64_t toread = size - copied;
		if(toread > COPY_CHUNK)
			toread = COPY_CHUNK;

		ret = hfs_read(vol, buf, toread, offset + copied, NULL);
		if(ret)
			break;
		ret = write_all_at(outfd, buf, (size_t)toread, offset + copied);
		if(ret)
			break;
		copied += toread;
	}

	free(buf);
	if(!ret && st) {
		st->regions++;
		st->bytes += size;
	}
	return ret;
}

static int copy_fork_extents(hfs_volume* vol, int outfd, hfs_cnid_t cnid, const char* name, struct copy_stats* st) {
	hfs_extent_descriptor_t* extents = NULL;
	uint16_t nextents = hfslib_get_file_extents(vol, cnid, HFS_DATAFORK, &extents, NULL);
	if(nextents == 0 || !extents)
		return -ENOENT;

	int ret = 0;
	for(uint16_t i = 0; i < nextents; i++) {
		uint32_t start_block = extents[i].start_block;
		uint32_t block_count = extents[i].block_count;
		if(block_count == 0)
			continue;

		uint64_t offset = (uint64_t)start_block * vol->vh.block_size;
		uint64_t size = (uint64_t)block_count * vol->vh.block_size;

		ret = copy_region(vol, outfd, offset, size, st);
		if(ret) {
			fprintf(stderr, "%s copy failed at extent %" PRIu16 ": %s\n", name, i, strerror(-ret));
			break;
		}
	}

	free(extents);
	return ret;
}

static int copy_volume_headers(hfs_volume* vol, int outfd, uint64_t volume_size, struct copy_stats* st) {
	int ret = copy_region(vol, outfd, HFS_VOLUME_HEAD_RESERVE_SIZE, sizeof(hfs_volume_header_t), st);
	if(ret)
		return ret;

	if(volume_size <= HFS_VOLUME_HEAD_RESERVE_SIZE)
		return 0;

	uint64_t alt_off = volume_size - HFS_VOLUME_HEAD_RESERVE_SIZE;
	if(alt_off == HFS_VOLUME_HEAD_RESERVE_SIZE)
		return 0;

	return copy_region(vol, outfd, alt_off, sizeof(hfs_volume_header_t), st);
}

static int copy_journal(hfs_volume* vol, int outfd, struct copy_stats* st) {
	if(!(vol->vh.attributes & (1U << HFS_VOL_JOURNALED)))
		return 0;

	if(vol->vh.journal_info_block) {
		uint64_t jib_off = (uint64_t)vol->vh.journal_info_block * vol->vh.block_size;
		int ret = copy_region(vol, outfd, jib_off, sizeof(hfs_journal_info_t), st);
		if(ret)
			return ret;
	}

	if(vol->jib.size == 0)
		return 0;
	if((vol->jib.flags & HFS_JOURNAL_ON_DISK_MASK) == 0)
		return 0;

	return copy_region(vol, outfd, vol->jib.offset, vol->jib.size, st);
}

int main(int argc, char* argv[]) {
	if(argc < 3) {
		fprintf(stderr,
			"Usage: hfsclone <device> <output_sparse_image>\n\n"
			"hfsclone version " HFSFUSE_VERSION_STRING "\n");
		return 1;
	}

	hfs_volume vol;
	struct hfs_volume_config cfg;
	hfs_volume_config_defaults(&cfg);
	cfg.cache_size = 0;

	int ret = hfs_open_volume(argv[1], &vol, &cfg);
	if(ret) {
		fprintf(stderr, "Couldn't open volume: %s\n", strerror(ret));
		return 1;
	}

	uint64_t volume_size = (uint64_t)vol.vh.total_blocks * vol.vh.block_size;
	if(volume_size == 0) {
		fprintf(stderr, "Invalid volume size\n");
		hfslib_close_volume(&vol, NULL);
		return 1;
	}

	int outfd = open(argv[2], O_CREAT | O_TRUNC | O_WRONLY, 0644);
	if(outfd < 0) {
		fprintf(stderr, "Couldn't open output: %s\n", strerror(errno));
		hfslib_close_volume(&vol, NULL);
		return 1;
	}

	if(ftruncate(outfd, (off_t)volume_size) < 0) {
		fprintf(stderr, "Couldn't resize output: %s\n", strerror(errno));
		close(outfd);
		hfslib_close_volume(&vol, NULL);
		return 1;
	}

	struct copy_stats stats = {0};

	ret = copy_volume_headers(&vol, outfd, volume_size, &stats);
	if(ret)
		goto end;

	ret = copy_fork_extents(&vol, outfd, HFS_CNID_ALLOCATION, "allocation file", &stats);
	if(ret)
		goto end;
	ret = copy_fork_extents(&vol, outfd, HFS_CNID_CATALOG, "catalog file", &stats);
	if(ret)
		goto end;
	ret = copy_fork_extents(&vol, outfd, HFS_CNID_EXTENTS, "extents overflow file", &stats);
	if(ret)
		goto end;
	if(vol.vh.attributes_file.total_blocks > 0) {
		ret = copy_fork_extents(&vol, outfd, HFS_CNID_ATTRIBUTES, "attributes file", &stats);
		if(ret && ret != -ENOENT)
			goto end;
	}

	ret = copy_journal(&vol, outfd, &stats);
	if(ret)
		goto end;

end:
	if(ret)
		fprintf(stderr, "Metadata dump failed: %s\n", strerror(-ret));
	else
		printf("Done. Copied metadata bytes: %" PRIu64 " across %" PRIu64 " regions\n", stats.bytes, stats.regions);

	close(outfd);
	hfslib_close_volume(&vol, NULL);
	return ret ? 1 : 0;
}
