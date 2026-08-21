// SPDX-License-Identifier: GPL-2.0-only
#define _GNU_SOURCE
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define SIZE (256U * 1024U)
#define TOTAL (16U * 1024U * 1024U)

extern void *s31_xespv_memcpy(void *, const void *, size_t);
extern void *s31_xespv_memset(void *, int, size_t);
extern void *s31_xespv_memmove(void *, const void *, size_t);
extern void *s31_xespv_memchr(const void *, int, size_t);
extern void *s31_xespv_memrchr(const void *, int, size_t);
extern int s31_xespv_memcmp(const void *, const void *, size_t);
extern void *s31_xespv_memccpy(void *, const void *, int, size_t);
extern size_t s31_xespv_strlen(const char *);
extern size_t s31_xespv_strnlen(const char *, size_t);
extern char *s31_xespv_strchrnul(const char *, int);
extern char *s31_xespv_strchr(const char *, int);
extern int s31_xespv_strcmp(const char *, const char *);
extern char *s31_xespv_stpcpy(char *, const char *);
extern char *s31_xespv_strcpy(char *, const char *);
extern char *s31_xespv_stpncpy(char *, const char *, size_t);
extern char *s31_xespv_strncpy(char *, const char *, size_t);
extern size_t s31_xespv_strlcpy(char *, const char *, size_t);
extern void s31_xespv_eq16(unsigned char *, const unsigned char *,
			   const unsigned char *);

static uint64_t now_ns(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000000ULL + ts.tv_nsec;
}

static int same_sign(int a, int b)
{
	return (a > 0) == (b > 0) && (a < 0) == (b < 0);
}

static int memory_correctness(void)
{
	unsigned char a[640] __attribute__((aligned(64)));
	unsigned char b[640] __attribute__((aligned(64)));
	unsigned char x[640] __attribute__((aligned(64)));
	unsigned char y[640] __attribute__((aligned(64)));
	size_t off, n, i;

	for (i = 0; i < sizeof a; i++) a[i] = (unsigned char)(i * 37U + 11U);
	for (off = 0; off < 32; off++) for (n = 0; n <= 320; n++) {
		memset(x, 0xa5, sizeof x); memset(y, 0xa5, sizeof y);
		memcpy(x + off, a + off, n);
		s31_xespv_memcpy(y + off, a + off, n);
		if (memcmp(x, y, sizeof x)) return 10;
		memset(x + off, 0x6d, n);
		s31_xespv_memset(y + off, 0x6d, n);
		if (memcmp(x, y, sizeof x)) return 11;
		memcpy(b, a, sizeof b);
		if (memchr(a + off, a[off + n / 2], n) !=
		    s31_xespv_memchr(a + off, a[off + n / 2], n)) return 12;
		if (memrchr(a + off, a[off + n / 2], n) !=
		    s31_xespv_memrchr(a + off, a[off + n / 2], n)) return 13;
		{
			int normal = memcmp(a + off, b + off, n);
			int vector = s31_xespv_memcmp(a + off, b + off, n);
			if (!same_sign(normal, vector)) {
				unsigned char mask[16] __attribute__((aligned(16)));
				s31_xespv_eq16(mask, a + off, b + off);
				printf("memcmp equal mismatch: off=%zu n=%zu libc=%d XespV=%d\n",
				       off, n, normal, vector);
				printf("direct eq16 mask:");
				for (i = 0; i < sizeof mask; i++) printf(" %02x", mask[i]);
				putchar('\n');
				return 14;
			}
		}
		if (n) {
			b[off + n / 2] ^= 0x80;
			{
				int normal = memcmp(a + off, b + off, n);
				int vector = s31_xespv_memcmp(a + off, b + off, n);
				if (!same_sign(normal, vector)) {
					printf("memcmp difference mismatch: off=%zu n=%zu libc=%d XespV=%d\n",
					       off, n, normal, vector);
					return 15;
				}
			}
			b[off + n / 2] ^= 0x80;
		}
	}
	for (off = 0; off < 32; off++) for (n = 0; n <= 256; n++) {
		memcpy(x, a, sizeof x); memcpy(y, a, sizeof y);
		memmove(x + off + 17, x + off, n);
		s31_xespv_memmove(y + off + 17, y + off, n);
		if (memcmp(x, y, sizeof x)) return 16;
		memcpy(x, a, sizeof x); memcpy(y, a, sizeof y);
		memmove(x + off, x + off + 17, n);
		s31_xespv_memmove(y + off, y + off + 17, n);
		if (memcmp(x, y, sizeof x)) return 17;
	}
	return 0;
}

static int string_correctness(void)
{
	char a[640] __attribute__((aligned(64)));
	char b[640] __attribute__((aligned(64)));
	char x[640] __attribute__((aligned(64)));
	char y[640] __attribute__((aligned(64)));
	size_t off, len, i, n;

	for (off = 0; off < 32; off++) for (len = 0; len <= 320; len++) {
		for (i = 0; i < sizeof a; i++) a[i] = (char)('a' + i % 23);
		a[off + len] = 0;
		memcpy(b, a, sizeof b);
		if (strlen(a + off) != s31_xespv_strlen(a + off)) return 20;
		n = len / 2;
		if (strnlen(a + off, 0) != s31_xespv_strnlen(a + off, 0) ||
		    strnlen(a + off, n) != s31_xespv_strnlen(a + off, n) ||
		    strnlen(a + off, len) != s31_xespv_strnlen(a + off, len) ||
		    strnlen(a + off, len + 8) !=
		    s31_xespv_strnlen(a + off, len + 8)) return 21;
		if (strchrnul(a + off, 'z') != s31_xespv_strchrnul(a + off, 'z')) return 22;
		if (strchr(a + off, a[off + len / 2]) !=
		    s31_xespv_strchr(a + off, a[off + len / 2])) return 23;
		if (!same_sign(strcmp(a + off, b + off),
		               s31_xespv_strcmp(a + off, b + off))) return 24;
		if (len) {
			b[off + len / 2] = 'z';
			if (!same_sign(strcmp(a + off, b + off),
			               s31_xespv_strcmp(a + off, b + off))) return 25;
		}
		memset(x, 0xa5, sizeof x); memset(y, 0xa5, sizeof y);
		if (stpcpy(x + off, a + off) - (x + off) !=
		    s31_xespv_stpcpy(y + off, a + off) - (y + off) ||
		    memcmp(x, y, sizeof x)) return 26;
		memset(x, 0xa5, sizeof x); memset(y, 0xa5, sizeof y);
		if (strcpy(x + off, a + off) != x + off ||
		    s31_xespv_strcpy(y + off, a + off) != y + off ||
		    memcmp(x, y, sizeof x)) return 27;
		n = len + 8;
		memset(x, 0xa5, sizeof x); memset(y, 0xa5, sizeof y);
		if (stpncpy(x + off, a + off, n) - (x + off) !=
		    s31_xespv_stpncpy(y + off, a + off, n) - (y + off) ||
		    memcmp(x, y, sizeof x)) return 28;
		memset(x, 0xa5, sizeof x); memset(y, 0xa5, sizeof y);
		strncpy(x + off, a + off, n);
		s31_xespv_strncpy(y + off, a + off, n);
		if (memcmp(x, y, sizeof x)) return 29;
		memset(x, 0xa5, sizeof x); memset(y, 0xa5, sizeof y);
		if (strlcpy(x + off, a + off, len / 2 + 1) !=
		    s31_xespv_strlcpy(y + off, a + off, len / 2 + 1) ||
		    memcmp(x, y, sizeof x)) return 30;
	}
	return 0;
}

static int memccpy_correctness(void)
{
	unsigned char a[384] __attribute__((aligned(64)));
	unsigned char x[384] __attribute__((aligned(64)));
	unsigned char y[384] __attribute__((aligned(64)));
	size_t n, i;
	for (i = 0; i < sizeof a; i++) a[i] = (unsigned char)(i * 29U + 3U);
	for (n = 0; n <= 320; n++) {
		void *p, *q;
		memset(x, 0, sizeof x); memset(y, 0, sizeof y);
		p = memccpy(x, a, n ? a[n / 2] : 7, n);
		q = s31_xespv_memccpy(y, a, n ? a[n / 2] : 7, n);
		if ((p ? (unsigned char *)p - x : -1) !=
		    (q ? (unsigned char *)q - y : -1) || memcmp(x, y, sizeof x)) return 31;
	}
	return 0;
}

static volatile uintptr_t sink;

static void report(const char *name, uint64_t normal, uint64_t xespv)
{
	double normal_rate = (double)TOTAL * 1e9 / normal / 1048576.0;
	double xespv_rate = (double)TOTAL * 1e9 / xespv / 1048576.0;
	printf("%-10s %12.2f %12.2f %8.2fx\n", name, normal_rate, xespv_rate,
	       (double)normal / xespv);
}

#define RUN3(label, normal_fn, vector_fn, arg0, arg1, arg2) do { \
	unsigned loops_ = TOTAL / SIZE; uint64_t a_, b_, t_; \
	t_ = now_ns(); while (loops_--) sink ^= (uintptr_t)(normal_fn(arg0,arg1,arg2)); a_ = now_ns()-t_; \
	loops_ = TOTAL / SIZE; t_ = now_ns(); while (loops_--) sink ^= (uintptr_t)(vector_fn(arg0,arg1,arg2)); b_ = now_ns()-t_; \
	report(label,a_,b_); \
} while (0)

#define RUN4(label, normal_fn, vector_fn, arg0, arg1, arg2, arg3) do { \
	unsigned loops_ = TOTAL / SIZE; uint64_t a_, b_, t_; \
	t_ = now_ns(); while (loops_--) sink ^= (uintptr_t)(normal_fn(arg0,arg1,arg2,arg3)); a_ = now_ns()-t_; \
	loops_ = TOTAL / SIZE; t_ = now_ns(); while (loops_--) sink ^= (uintptr_t)(vector_fn(arg0,arg1,arg2,arg3)); b_ = now_ns()-t_; \
	report(label,a_,b_); \
} while (0)

#define RUN2(label, normal_fn, vector_fn, arg0, arg1) do { \
	unsigned loops_ = TOTAL / SIZE; uint64_t a_, b_, t_; \
	t_ = now_ns(); while (loops_--) sink ^= (uintptr_t)(normal_fn(arg0,arg1)); a_ = now_ns()-t_; \
	loops_ = TOTAL / SIZE; t_ = now_ns(); while (loops_--) sink ^= (uintptr_t)(vector_fn(arg0,arg1)); b_ = now_ns()-t_; \
	report(label,a_,b_); \
} while (0)

#define RUN1(label, normal_fn, vector_fn, arg0) do { \
	unsigned loops_ = TOTAL / SIZE; uint64_t a_, b_, t_; \
	t_ = now_ns(); while (loops_--) sink ^= (uintptr_t)(normal_fn(arg0)); a_ = now_ns()-t_; \
	loops_ = TOTAL / SIZE; t_ = now_ns(); while (loops_--) sink ^= (uintptr_t)(vector_fn(arg0)); b_ = now_ns()-t_; \
	report(label,a_,b_); \
} while (0)

int main(void)
{
	unsigned char *a, *b, *d;
	int rc;
	if (posix_memalign((void **)&a, 64, SIZE + 128) ||
	    posix_memalign((void **)&b, 64, SIZE + 128) ||
	    posix_memalign((void **)&d, 64, SIZE + 128)) return 2;
	if ((rc = memory_correctness()) || (rc = string_correctness()) ||
	    (rc = memccpy_correctness())) {
		printf("XespV correctness: FAIL (%d)\n", rc);
		return 1;
	}
	puts("XespV correctness: PASS (17 routines, aligned/misaligned/tails/overlap)");
	memset(a, 'a', SIZE); memset(b, 'a', SIZE); memset(d, 0, SIZE);
	a[SIZE-1] = b[SIZE-1] = 0;
	puts("aligned 256 KiB ranges, 16 MiB per result");
	puts("routine      libc MiB/s  XespV MiB/s  speedup");
	RUN3("memcpy", memcpy, s31_xespv_memcpy, d, a, SIZE);
	RUN3("memset", memset, s31_xespv_memset, d, 0x5a, SIZE);
	RUN3("memmove", memmove, s31_xespv_memmove, d, a, SIZE);
	RUN3("memchr", memchr, s31_xespv_memchr, a, 0, SIZE);
	RUN3("memrchr", memrchr, s31_xespv_memrchr, a, '!', SIZE);
	RUN3("memcmp", memcmp, s31_xespv_memcmp, a, b, SIZE);
	RUN1("strlen", strlen, s31_xespv_strlen, (char *)a);
	RUN2("strnlen", strnlen, s31_xespv_strnlen, (char *)a, SIZE);
	RUN2("strchrnul", strchrnul, s31_xespv_strchrnul, (char *)a, '!');
	RUN2("strchr", strchr, s31_xespv_strchr, (char *)a, '!');
	RUN2("strcmp", strcmp, s31_xespv_strcmp, (char *)a, (char *)b);
	RUN2("stpcpy", stpcpy, s31_xespv_stpcpy, (char *)d, (char *)a);
	RUN2("strcpy", strcpy, s31_xespv_strcpy, (char *)d, (char *)a);
	RUN3("stpncpy", stpncpy, s31_xespv_stpncpy, (char *)d, (char *)a, SIZE);
	RUN3("strncpy", strncpy, s31_xespv_strncpy, (char *)d, (char *)a, SIZE);
	RUN4("memccpy", memccpy, s31_xespv_memccpy, d, a, 0, SIZE);
	/* strlcpy has the same three-argument shape as memcpy for the macro. */
	RUN3("strlcpy", strlcpy, s31_xespv_strlcpy, (char *)d, (char *)a, SIZE);
	free(d); free(b); free(a);
	return sink == ~(uintptr_t)0;
}
