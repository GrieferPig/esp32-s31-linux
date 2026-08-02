// SPDX-License-Identifier: GPL-2.0-only
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include "s31_hosted_sram.h"

#define S31_HOSTED_IOC_MEM_STATS \
	_IOR('S', 0x32, struct s31_hosted_mem_stats)
#define RECOMMENDED_HEADROOM	(32U * 1024U)

static void print_size(const char *label, uint32_t bytes)
{
	printf("%-30s %10u bytes (%7.1f KiB)\n", label, bytes,
	       bytes / 1024.0);
}

int main(void)
{
	struct s31_hosted_mem_stats stats;
	int fd;

	fd = open("/dev/esps0", O_RDWR);
	if (fd < 0) {
		perror("open /dev/esps0");
		return 1;
	}
	if (ioctl(fd, S31_HOSTED_IOC_MEM_STATS, &stats)) {
		perror("FreeRTOS memory-statistics ioctl");
		close(fd);
		return 1;
	}
	close(fd);

	puts("FreeRTOS HP-SRAM heap (internal, DMA-capable):");
	print_size("Total heap", stats.total_bytes);
	print_size("Current free", stats.free_bytes);
	print_size("Historical minimum free", stats.minimum_free_bytes);
	print_size("Largest free block", stats.largest_free_block);

	puts("\nActive 64 KiB OpenSBI HP-SRAM carve-out:");
	print_size("Minimum-free headroom", stats.minimum_free_bytes);
	if (stats.minimum_free_bytes < RECOMMENDED_HEADROOM) {
		puts("Verdict: MARGINAL; less than 32 KiB low-water headroom remains.");
		return 2;
	}
	puts("Verdict: PASS based on observed low-water usage with the carve-out active.");
	return 0;
}
