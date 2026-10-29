/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef ESP_SIMD_H
#define ESP_SIMD_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Pin the calling thread to hart 1.  A constructor calls this before main(),
 * and every public entry point repeats it once for each newly-created thread.
 */
int esp_simd_init(void);
int esp_simd_cpu(void);
int esp_simd_active(void);

void *esp_simd_memcpy(void *dst, const void *src, size_t size);
void *esp_simd_memset(void *dst, int value, size_t size);
void *esp_simd_memmove(void *dst, const void *src, size_t size);
void *esp_simd_memchr(const void *src, int value, size_t size);
void *esp_simd_memrchr(const void *src, int value, size_t size);
int esp_simd_memcmp(const void *lhs, const void *rhs, size_t size);
void *esp_simd_memccpy(void *dst, const void *src, int value, size_t size);

size_t esp_simd_strlen(const char *src);
size_t esp_simd_strnlen(const char *src, size_t size);
char *esp_simd_strchrnul(const char *src, int value);
char *esp_simd_strchr(const char *src, int value);
int esp_simd_strcmp(const char *lhs, const char *rhs);
char *esp_simd_stpcpy(char *dst, const char *src);
char *esp_simd_strcpy(char *dst, const char *src);
char *esp_simd_stpncpy(char *dst, const char *src, size_t size);
char *esp_simd_strncpy(char *dst, const char *src, size_t size);
size_t esp_simd_strlcpy(char *dst, const char *src, size_t size);

void esp_simd_eq_u8x16(uint8_t out[16], const uint8_t lhs[16],
		       const uint8_t rhs[16]);
void esp_simd_add_sat_u8(uint8_t *out, const uint8_t *lhs,
			 const uint8_t *rhs, size_t count);

#ifdef __cplusplus
}
#endif

#endif
