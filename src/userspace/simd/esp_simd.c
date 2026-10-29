// SPDX-License-Identifier: GPL-2.0-only
#define _GNU_SOURCE
#include "esp_simd.h"

#include <errno.h>
#include <sched.h>
#include <string.h>

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
extern void s31_xespv_eq16(uint8_t *, const uint8_t *, const uint8_t *);
extern void s31_xespv_add_sat_u8(uint8_t *, const uint8_t *, const uint8_t *,
				 size_t);

/* Affinity is per-thread.  New pthreads inherit CPU1 and confirm it on their
 * first SIMD call; dlopen() from an existing thread is covered as well. */
static _Thread_local int esp_simd_thread_state;

int esp_simd_init(void)
{
	cpu_set_t set;

	if (esp_simd_thread_state > 0)
		return 0;

	CPU_ZERO(&set);
	CPU_SET(1, &set);
	if (sched_setaffinity(0, sizeof(set), &set)) {
		esp_simd_thread_state = -errno;
		return esp_simd_thread_state;
	}

	esp_simd_thread_state = 1;
	return 0;
}

__attribute__((constructor(101)))
static void esp_simd_constructor(void)
{
	(void)esp_simd_init();
}

int esp_simd_cpu(void)
{
	return sched_getcpu();
}

int esp_simd_active(void)
{
	return esp_simd_thread_state > 0;
}

#define SIMD_OR_RETURN(fallback) \
	do { if (esp_simd_init()) return (fallback); } while (0)

void *esp_simd_memcpy(void *dst, const void *src, size_t size)
{
	SIMD_OR_RETURN(memcpy(dst, src, size));
	return s31_xespv_memcpy(dst, src, size);
}

void *esp_simd_memset(void *dst, int value, size_t size)
{
	SIMD_OR_RETURN(memset(dst, value, size));
	return s31_xespv_memset(dst, value, size);
}

void *esp_simd_memmove(void *dst, const void *src, size_t size)
{
	SIMD_OR_RETURN(memmove(dst, src, size));
	return s31_xespv_memmove(dst, src, size);
}

void *esp_simd_memchr(const void *src, int value, size_t size)
{
	SIMD_OR_RETURN(memchr(src, value, size));
	return s31_xespv_memchr(src, value, size);
}

void *esp_simd_memrchr(const void *src, int value, size_t size)
{
	SIMD_OR_RETURN(memrchr(src, value, size));
	return s31_xespv_memrchr(src, value, size);
}

int esp_simd_memcmp(const void *lhs, const void *rhs, size_t size)
{
	SIMD_OR_RETURN(memcmp(lhs, rhs, size));
	return s31_xespv_memcmp(lhs, rhs, size);
}

void *esp_simd_memccpy(void *dst, const void *src, int value, size_t size)
{
	SIMD_OR_RETURN(memccpy(dst, src, value, size));
	return s31_xespv_memccpy(dst, src, value, size);
}

size_t esp_simd_strlen(const char *src)
{
	SIMD_OR_RETURN(strlen(src));
	return s31_xespv_strlen(src);
}

size_t esp_simd_strnlen(const char *src, size_t size)
{
	SIMD_OR_RETURN(strnlen(src, size));
	return s31_xespv_strnlen(src, size);
}

char *esp_simd_strchrnul(const char *src, int value)
{
	SIMD_OR_RETURN(strchrnul(src, value));
	return s31_xespv_strchrnul(src, value);
}

char *esp_simd_strchr(const char *src, int value)
{
	SIMD_OR_RETURN(strchr(src, value));
	return s31_xespv_strchr(src, value);
}

int esp_simd_strcmp(const char *lhs, const char *rhs)
{
	SIMD_OR_RETURN(strcmp(lhs, rhs));
	return s31_xespv_strcmp(lhs, rhs);
}

char *esp_simd_stpcpy(char *dst, const char *src)
{
	SIMD_OR_RETURN(stpcpy(dst, src));
	return s31_xespv_stpcpy(dst, src);
}

char *esp_simd_strcpy(char *dst, const char *src)
{
	SIMD_OR_RETURN(strcpy(dst, src));
	return s31_xespv_strcpy(dst, src);
}

char *esp_simd_stpncpy(char *dst, const char *src, size_t size)
{
	SIMD_OR_RETURN(stpncpy(dst, src, size));
	return s31_xespv_stpncpy(dst, src, size);
}

char *esp_simd_strncpy(char *dst, const char *src, size_t size)
{
	SIMD_OR_RETURN(strncpy(dst, src, size));
	return s31_xespv_strncpy(dst, src, size);
}

size_t esp_simd_strlcpy(char *dst, const char *src, size_t size)
{
	SIMD_OR_RETURN(strlcpy(dst, src, size));
	return s31_xespv_strlcpy(dst, src, size);
}

void esp_simd_eq_u8x16(uint8_t out[16], const uint8_t lhs[16],
		       const uint8_t rhs[16])
{
	size_t i;

	if (esp_simd_init()) {
		for (i = 0; i < 16; i++)
			out[i] = lhs[i] == rhs[i] ? 0xff : 0;
		return;
	}
	s31_xespv_eq16(out, lhs, rhs);
}

void esp_simd_add_sat_u8(uint8_t *out, const uint8_t *lhs,
			 const uint8_t *rhs, size_t count)
{
	size_t i;

	if (esp_simd_init()) {
		for (i = 0; i < count; i++) {
			unsigned int sum = lhs[i] + rhs[i];

			out[i] = sum > UINT8_MAX ? UINT8_MAX : sum;
		}
		return;
	}
	s31_xespv_add_sat_u8(out, lhs, rhs, count);
}
