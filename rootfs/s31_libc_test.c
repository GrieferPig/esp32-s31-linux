// SPDX-License-Identifier: GPL-2.0-only
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/auxv.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#define HWCAP_ISA_F (1UL << ('F' - 'A'))
#define TEST_SIZE 1024

static void ref_copy(volatile unsigned char *d,
		     const volatile unsigned char *s, size_t n)
{
	while (n--)
		*d++ = *s++;
}

static void ref_set(volatile unsigned char *d, unsigned char c, size_t n)
{
	while (n--)
		*d++ = c;
}

static int correctness_tests(void)
{
	unsigned char *src;
	unsigned char *dst;
	unsigned char *ref;
	unsigned int so, doff;
	size_t n;

	if (posix_memalign((void **)&src, 64, TEST_SIZE) ||
	    posix_memalign((void **)&dst, 64, TEST_SIZE) ||
	    posix_memalign((void **)&ref, 64, TEST_SIZE)) {
		perror("posix_memalign");
		return -1;
	}

	for (n = 0; n < TEST_SIZE; n++)
		src[n] = (unsigned char)(n * 131U + 17U);

	for (so = 0; so < 16; so++) {
		for (doff = 0; doff < 16; doff++) {
			for (n = 0; n <= 192; n++) {
				ref_set(dst, 0xa5, TEST_SIZE);
				ref_set(ref, 0xa5, TEST_SIZE);
				ref_copy(ref + doff, src + so, n);
				if (memcpy(dst + doff, src + so, n) != dst + doff ||
				    memcmp(dst, ref, TEST_SIZE)) {
					fprintf(stderr,
						"memcpy failed so=%u do=%u n=%zu\n",
						so, doff, n);
					return -1;
				}

				ref_set(dst, 0x5a, TEST_SIZE);
				ref_set(ref, 0x5a, TEST_SIZE);
				ref_set(ref + doff, (unsigned char)(n + so), n);
				if (memset(dst + doff, (int)(n + so), n) !=
				    dst + doff || memcmp(dst, ref, TEST_SIZE)) {
					fprintf(stderr,
						"memset failed do=%u n=%zu\n",
						doff, n);
					return -1;
				}
			}
		}
	}

	for (n = 0; n <= 192; n++) {
		int delta;

		for (delta = -48; delta <= 48; delta++) {
			unsigned char *d = dst + 256 + delta;
			unsigned char *s = dst + 256;

			ref_copy(dst, src, TEST_SIZE);
			ref_copy(ref, src, TEST_SIZE);
			if (delta < 0)
				ref_copy(ref + 256 + delta, ref + 256, n);
			else {
				size_t i = n;
				while (i--)
					ref[256 + delta + i] = ref[256 + i];
			}
			if (memmove(d, s, n) != d ||
			    memcmp(dst, ref, TEST_SIZE)) {
				fprintf(stderr,
					"memmove failed delta=%d n=%zu\n",
					delta, n);
				return -1;
			}
		}
	}

	free(src);
	free(dst);
	free(ref);
	return 0;
}

static int guard_page_tests(void)
{
	long page_size = sysconf(_SC_PAGESIZE);
	unsigned char *src;
	unsigned char *dst;
	size_t n;

	src = mmap(NULL, page_size * 2, PROT_READ | PROT_WRITE,
		   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	dst = mmap(NULL, page_size * 2, PROT_READ | PROT_WRITE,
		   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (src == MAP_FAILED || dst == MAP_FAILED) {
		perror("mmap");
		return -1;
	}
	if (mprotect(src + page_size, page_size, PROT_NONE) ||
	    mprotect(dst + page_size, page_size, PROT_NONE)) {
		perror("mprotect");
		return -1;
	}

	for (n = 0; n <= 256; n++) {
		unsigned char *s = src + page_size - n;
		unsigned char *d = dst + page_size - n;
		size_t i;

		for (i = 0; i < n; i++)
			s[i] = (unsigned char)(i ^ n);
		memcpy(d, s, n);
		if (memcmp(d, s, n)) {
			fprintf(stderr, "guard memcpy failed n=%zu\n", n);
			return -1;
		}
		memset(d, 0x69, n);
		for (i = 0; i < n; i++)
			if (d[i] != 0x69) {
				fprintf(stderr, "guard memset failed n=%zu\n", n);
				return -1;
			}
	}

	munmap(src, page_size * 2);
	munmap(dst, page_size * 2);
	return 0;
}

__attribute__((noinline)) static float addf(float a, float b)
{
	return a + b;
}

__attribute__((noinline)) static double addd(double a, double b)
{
	return a + b;
}

static double elapsed(const struct timespec *a, const struct timespec *b)
{
	return (double)(b->tv_sec - a->tv_sec) +
	       (double)(b->tv_nsec - a->tv_nsec) / 1000000000.0;
}

static int benchmark(void)
{
	const size_t bytes = 512 * 1024;
	const unsigned int iterations = 128;
	unsigned char *src;
	unsigned char *dst;
	struct timespec begin, end;
	double seconds;
	unsigned int i;

	if (posix_memalign((void **)&src, 64, bytes) ||
	    posix_memalign((void **)&dst, 64, bytes))
		return -1;
	memset(src, 0x35, bytes);

	clock_gettime(CLOCK_MONOTONIC, &begin);
	for (i = 0; i < iterations; i++)
		memcpy(dst, src, bytes);
	clock_gettime(CLOCK_MONOTONIC, &end);
	seconds = elapsed(&begin, &end);
	printf("memcpy: %.2f MiB/s\n",
	       (bytes * (double)iterations) / seconds / (1024.0 * 1024.0));

	clock_gettime(CLOCK_MONOTONIC, &begin);
	for (i = 0; i < iterations; i++)
		memset(dst, i, bytes);
	clock_gettime(CLOCK_MONOTONIC, &end);
	seconds = elapsed(&begin, &end);
	printf("memset: %.2f MiB/s\n",
	       (bytes * (double)iterations) / seconds / (1024.0 * 1024.0));

	if (dst[bytes - 1] != (unsigned char)(iterations - 1))
		return -1;
	free(src);
	free(dst);
	return 0;
}

int main(void)
{
	unsigned long hwcap = getauxval(AT_HWCAP);
	volatile float fa = 1.5f, fb = 2.25f;
	volatile double da = 1.5, db = 2.25;

	printf("AT_HWCAP=%08lx F=%s\n", hwcap,
	       hwcap & HWCAP_ISA_F ? "yes" : "no");
	if (!(hwcap & HWCAP_ISA_F)) {
		fprintf(stderr, "kernel did not advertise RV32F\n");
		return 1;
	}
	if (addf(fa, fb) != 3.75f) {
		fprintf(stderr, "hardware float test failed\n");
		return 1;
	}
	if (addd(da, db) != 3.75) {
		fprintf(stderr, "software double test failed\n");
		return 1;
	}
	if (correctness_tests() || guard_page_tests() || benchmark())
		return 1;
	puts("S31 libc Xespv/F-only tests: PASS");
	return 0;
}
