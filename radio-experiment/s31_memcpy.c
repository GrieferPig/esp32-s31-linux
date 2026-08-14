/*
 * Word-oriented memcpy/memset for the closed ESP32-S31 radio payload.
 *
 * The precompiled Wi-Fi HAL writes the MAC CCMP key slots with memcpy() into
 * an MMIO register file.  The native IDF build resolves memcpy to the mask
 * ROM's word-copying implementation, while the Linux payload was pulling the
 * byte-oriented picolibc copy and storing the key bytes with SB.  The S31 MAC
 * crypto key slot does not latch byte stores, so the key bytes were corrupted
 * while the 32-bit metadata words around them remained correct.
 */
#include <stddef.h>
#include <stdint.h>

void *memcpy(void *dst, const void *src, size_t n)
{
uint8_t *d = dst;
const uint8_t *s = src;

if (n >= 4 && (((uintptr_t)d & 3) == 0) && (((uintptr_t)s & 3) == 0)) {
uint32_t *dw = (uint32_t *)d;
const uint32_t *sw = (const uint32_t *)s;

while (n >= 4) {
*dw++ = *sw++;
n -= 4;
}
d = (uint8_t *)dw;
s = (const uint8_t *)sw;
}
while (n--)
*d++ = *s++;
return dst;
}

void *memset(void *dst, int c, size_t n)
{
uint8_t *d = dst;

while (n--)
*d++ = (uint8_t)c;
return dst;
}
