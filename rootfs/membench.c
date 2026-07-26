#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static uint64_t monotonic_ns(void)
{
	struct timespec ts;

	if (clock_gettime(CLOCK_MONOTONIC, &ts)) {
		perror("clock_gettime");
		exit(1);
	}
	return (uint64_t)ts.tv_sec * 1000000000ULL + ts.tv_nsec;
}

static void run_copy(size_t size, unsigned int iterations)
{
	unsigned char *src;
	unsigned char *dst;
	uint64_t start, elapsed;
	uint32_t checksum = 0;
	unsigned int i;
	double mib_s;

	if (posix_memalign((void **)&src, 128, size) ||
	    posix_memalign((void **)&dst, 128, size)) {
		fprintf(stderr, "allocation of two %zu-byte buffers failed: %s\n",
			size, strerror(errno));
		exit(1);
	}
	for (i = 0; i < size; i++)
		src[i] = (unsigned char)(i * 13U + 7U);
	memset(dst, 0, size);
	memcpy(dst, src, size);

	start = monotonic_ns();
	for (i = 0; i < iterations; i++)
		memcpy(dst, src, size);
	elapsed = monotonic_ns() - start;

	for (i = 0; i < size; i += 4096)
		checksum = checksum * 33U + dst[i];
	checksum = checksum * 33U + dst[size - 1];
	mib_s = ((double)size * iterations * 1000000000.0) /
		((double)elapsed * 1024.0 * 1024.0);
	printf("software memcpy: size=%7zu iterations=%5u time=%8.3f ms "
	       "throughput=%8.2f MiB/s checksum=%08x\n",
	       size, iterations, elapsed / 1000000.0, mib_s, checksum);
	free(dst);
	free(src);
}

int main(void)
{
	/* 128 MiB moved in each case; 512 KiB is well beyond the data cache. */
	run_copy(64 * 1024, 2048);
	run_copy(512 * 1024, 256);
	return 0;
}
