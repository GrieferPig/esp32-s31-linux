// SPDX-License-Identifier: GPL-2.0-only
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define BUFFER_SIZE (512U * 1024U)
#define TOTAL_BYTES (32U * 1024U * 1024U)

extern void *bench_scalar_memcpy(void *, const void *, size_t);
extern void *bench_xespv_memcpy(void *, const void *, size_t);
extern void *bench_scalar_memset(void *, int, size_t);
extern void *bench_xespv_memset(void *, int, size_t);

/* Benchmark inputs are 64-byte aligned and all lengths are multiples of 64. */
__asm__(
".text\n"
".align 2\n"
".global bench_scalar_memcpy\n"
".type bench_scalar_memcpy,@function\n"
"bench_scalar_memcpy:\n"
" mv t6,a0\n"
"1:\n"
" beqz a2,3f\n"
" li t0,16\n"
" bltu a2,t0,2f\n"
" lw t0,0(a1)\n"
" lw t1,4(a1)\n"
" lw t2,8(a1)\n"
" lw t3,12(a1)\n"
" sw t0,0(a0)\n"
" sw t1,4(a0)\n"
" sw t2,8(a0)\n"
" sw t3,12(a0)\n"
" addi a1,a1,16\n"
" addi a0,a0,16\n"
" addi a2,a2,-16\n"
" j 1b\n"
"2:\n"
" lbu t0,0(a1)\n"
" sb t0,0(a0)\n"
" addi a1,a1,1\n"
" addi a0,a0,1\n"
" addi a2,a2,-1\n"
" j 1b\n"
"3:\n"
" mv a0,t6\n"
" ret\n"
".size bench_scalar_memcpy,.-bench_scalar_memcpy\n"

".align 2\n"
".global bench_xespv_memcpy\n"
".type bench_xespv_memcpy,@function\n"
"bench_xespv_memcpy:\n"
" mv t6,a0\n"
"1:\n"
" beqz a2,2f\n"
" esp.vld.128.ip q0,a1,16\n"
" esp.vld.128.ip q1,a1,16\n"
" esp.vld.128.ip q2,a1,16\n"
" esp.vld.128.ip q3,a1,16\n"
" esp.vst.128.ip q0,a0,16\n"
" esp.vst.128.ip q1,a0,16\n"
" esp.vst.128.ip q2,a0,16\n"
" esp.vst.128.ip q3,a0,16\n"
" addi a2,a2,-64\n"
" j 1b\n"
"2:\n"
" mv a0,t6\n"
" ret\n"
".size bench_xespv_memcpy,.-bench_xespv_memcpy\n"

".align 2\n"
".global bench_scalar_memset\n"
".type bench_scalar_memset,@function\n"
"bench_scalar_memset:\n"
" mv t6,a0\n"
" andi a1,a1,255\n"
" li t0,0x01010101\n"
" mul a1,a1,t0\n"
"1:\n"
" beqz a2,2f\n"
" sw a1,0(a0)\n"
" sw a1,4(a0)\n"
" sw a1,8(a0)\n"
" sw a1,12(a0)\n"
" addi a0,a0,16\n"
" addi a2,a2,-16\n"
" j 1b\n"
"2:\n"
" mv a0,t6\n"
" ret\n"
".size bench_scalar_memset,.-bench_scalar_memset\n"

".align 2\n"
".global bench_xespv_memset\n"
".type bench_xespv_memset,@function\n"
"bench_xespv_memset:\n"
" mv t6,a0\n"
" andi a1,a1,255\n"
" li t0,0x01010101\n"
" mul a1,a1,t0\n"
" addi sp,sp,-16\n"
" sw a1,0(sp)\n"
" sw a1,4(sp)\n"
" sw a1,8(sp)\n"
" sw a1,12(sp)\n"
" mv t3,sp\n"
" esp.vld.128.ip q0,t3,0\n"
" addi sp,sp,16\n"
"1:\n"
" beqz a2,2f\n"
" esp.vst.128.ip q0,a0,16\n"
" esp.vst.128.ip q0,a0,16\n"
" esp.vst.128.ip q0,a0,16\n"
" esp.vst.128.ip q0,a0,16\n"
" addi a2,a2,-64\n"
" j 1b\n"
"2:\n"
" mv a0,t6\n"
" ret\n"
".size bench_xespv_memset,.-bench_xespv_memset\n"
);

static double elapsed(const struct timespec *start, const struct timespec *end)
{
	return (double)(end->tv_sec - start->tv_sec) +
	       (double)(end->tv_nsec - start->tv_nsec) / 1000000000.0;
}

static double run_copy(void *(*fn)(void *, const void *, size_t),
		       unsigned char *dst, const unsigned char *src,
		       size_t size, unsigned int iterations)
{
	struct timespec start, end;
	unsigned int i;

	clock_gettime(CLOCK_MONOTONIC, &start);
	for (i = 0; i < iterations; i++)
		fn(dst, src, size);
	clock_gettime(CLOCK_MONOTONIC, &end);
	return elapsed(&start, &end);
}

static double run_set(void *(*fn)(void *, int, size_t), unsigned char *dst,
		      size_t size, unsigned int iterations)
{
	struct timespec start, end;
	unsigned int i;

	clock_gettime(CLOCK_MONOTONIC, &start);
	for (i = 0; i < iterations; i++)
		fn(dst, (int)i, size);
	clock_gettime(CLOCK_MONOTONIC, &end);
	return elapsed(&start, &end);
}

static double rate(size_t size, unsigned int iterations, double seconds)
{
	return ((double)size * iterations) / seconds / (1024.0 * 1024.0);
}

int main(void)
{
	static const size_t sizes[] = { 64, 256, 1024, 16384, 262144, 524288 };
	unsigned char *src, *scalar, *vector;
	size_t index;

	if (posix_memalign((void **)&src, 64, BUFFER_SIZE) ||
	    posix_memalign((void **)&scalar, 64, BUFFER_SIZE) ||
	    posix_memalign((void **)&vector, 64, BUFFER_SIZE)) {
		perror("posix_memalign");
		return 1;
	}
	for (index = 0; index < BUFFER_SIZE; index++)
		src[index] = (unsigned char)(index * 131U + 17U);

	puts("aligned buffers; each result transfers 32 MiB");
	puts("size       scalar MiB/s   XespV MiB/s   speedup");
	puts("memcpy");
	for (index = 0; index < sizeof(sizes) / sizeof(sizes[0]); index++) {
		size_t size = sizes[index];
		unsigned int iterations = TOTAL_BYTES / size;
		double st, vt, sr, vr;

		memset(scalar, 0, size);
		memset(vector, 0, size);
		st = run_copy(bench_scalar_memcpy, scalar, src, size, iterations);
		vt = run_copy(bench_xespv_memcpy, vector, src, size, iterations);
		if (memcmp(scalar, src, size) || memcmp(vector, src, size)) {
			fprintf(stderr, "memcpy verification failed at %zu\n", size);
			return 1;
		}
		sr = rate(size, iterations, st);
		vr = rate(size, iterations, vt);
		printf("%7zu %14.2f %13.2f %8.2fx\n", size, sr, vr, vr / sr);
	}

	puts("memset");
	for (index = 0; index < sizeof(sizes) / sizeof(sizes[0]); index++) {
		size_t size = sizes[index];
		unsigned int iterations = TOTAL_BYTES / size;
		unsigned char expected = (unsigned char)(iterations - 1);
		double st, vt, sr, vr;

		st = run_set(bench_scalar_memset, scalar, size, iterations);
		vt = run_set(bench_xespv_memset, vector, size, iterations);
		if (scalar[0] != expected || scalar[size / 2] != expected ||
		    scalar[size - 1] != expected || vector[0] != expected ||
		    vector[size / 2] != expected || vector[size - 1] != expected) {
			fprintf(stderr, "memset verification failed at %zu\n", size);
			return 1;
		}
		sr = rate(size, iterations, st);
		vr = rate(size, iterations, vt);
		printf("%7zu %14.2f %13.2f %8.2fx\n", size, sr, vr, vr / sr);
	}

	free(src);
	free(scalar);
	free(vector);
	return 0;
}
