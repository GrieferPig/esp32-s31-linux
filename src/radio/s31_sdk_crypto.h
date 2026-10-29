/* SPDX-License-Identifier: Apache-2.0 */
#ifndef S31_SDK_CRYPTO_H
#define S31_SDK_CRYPTO_H
#include <stdbool.h>
#include <stdint.h>
#include <linux/soc/espressif/esp32s31-crypto.h>

#define S31_SDK_CRYPTO_FAILURE (-1)

/* Stack-owned checked acquisition for SDK operations that can return errors.
 * Cleanup is idempotent, so allocation errors before admission are harmless.
 */
struct s31_sdk_crypto_lease {
	unsigned int domain;
	unsigned int clocks;
	bool held;
};
int s31_sdk_crypto_begin(struct s31_sdk_crypto_lease *lease, unsigned int domain);
int s31_sdk_crypto_end(struct s31_sdk_crypto_lease *lease);
int s31_sdk_crypto_fail(struct s31_sdk_crypto_lease *lease);

extern uint64_t s31_linux_time_ns(void);

/* Preserve the SDK's immediate-ready path. A stalled peripheral is bounded
 * by the MPI driver's existing two-second completion budget. Time is checked
 * every 64 failed reads, avoiding a clock service call on each MMIO poll.
 * These adapters run in a preemptible managed task, never an IRQ handler.
 */
#define S31_SDK_CRYPTO_WAIT_READY(ready) ({ \
	bool s31_ready = !!(ready); \
	if (!s31_ready) { \
		uint64_t s31_deadline = s31_linux_time_ns() + 2000000000ULL; \
		unsigned int s31_spins = 0; \
		do { \
			if (!(++s31_spins & 63) && \
			    s31_linux_time_ns() >= s31_deadline) \
				break; \
			s31_ready = !!(ready); \
		} while (!s31_ready); \
	} \
	s31_ready; \
})
#endif
