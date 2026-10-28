/* SPDX-License-Identifier: Apache-2.0 */
/* Keep SDK acceleration, with Linux-owned engine locks and clock RMW. */
#include <stdbool.h>
#include <stdint.h>

#include "../../shared/s31_crypto.h"

extern long s31_linux_crypto_operation(unsigned int operation, unsigned int domain);

/* Legacy void lock/clock entry points deliberately have no implementation.
 * The link audit rejects any surviving caller; configured SDK operations use
 * checked, stack-owned leases and return their native failure status.
 */
void __wrap_esp_crypto_common_clk_enable(bool enable)
{
	/* The native provider owns SYS/SEC and the parent clock. An SDK
	 * initialization hint must not gate them underneath another owner.
	 */
	(void)enable;
}

#include "s31_sdk_crypto.h"

static unsigned int s31_sdk_crypto_clocks(unsigned int domain)
{
	switch (domain) {
	case S31_CRYPTO_MPI: return 1U << S31_CRYPTO_MPI;
	case S31_CRYPTO_ECC: return 1U << S31_CRYPTO_ECC;
	case S31_CRYPTO_ECDSA:
		return (1U << S31_CRYPTO_ECDSA) | (1U << S31_CRYPTO_ECC) |
		       (1U << S31_CRYPTO_KEY_MANAGER);
	case S31_CRYPTO_HMAC:
		return (1U << S31_CRYPTO_HMAC) | (1U << S31_CRYPTO_SHA) |
		       (1U << S31_CRYPTO_DS) | (1U << S31_CRYPTO_KEY_MANAGER);
	default: return 0;
	}
}

int s31_sdk_crypto_end(struct s31_sdk_crypto_lease *lease)
{
	int ret = 0, error;
	unsigned int i;

	if (!lease->held)
		return 0;
	/* Match reverse acquisition, with KM reset before dependent clocks. */
	for (unsigned int step = 0; step <= S31_CRYPTO_SHA; step++) {
		i = step ? S31_CRYPTO_SHA + 1 - step : S31_CRYPTO_KEY_MANAGER;
		if (!(lease->clocks & (1U << i)))
			continue;
		error = s31_linux_crypto_operation(S31_CRYPTO_CLOCK_OFF, i);
		if (!ret)
			ret = error;
		lease->clocks &= ~(1U << i);
	}
	error = s31_linux_crypto_operation(S31_CRYPTO_RELEASE, lease->domain);
	if (!ret)
		ret = error;
	lease->held = false;
	return ret;
}

int s31_sdk_crypto_begin(struct s31_sdk_crypto_lease *lease, unsigned int domain)
{
	unsigned int clocks = s31_sdk_crypto_clocks(domain), i;
	int ret;

	if (!clocks || lease->held)
		return -1;
	ret = s31_linux_crypto_operation(S31_CRYPTO_ACQUIRE, domain);
	if (ret)
		return ret;
	lease->domain = domain;
	lease->held = true;
	lease->clocks = 0;
	/* Enable the dependent engines first and KM last: KM reset can reset
	 * ECC. Native clock failure consumes no clock reference.
	 */
	for (i = 1; i <= S31_CRYPTO_SHA; i++) {
		if (!(clocks & (1U << i)))
			continue;
		ret = s31_linux_crypto_operation(S31_CRYPTO_CLOCK_ON, i);
		if (ret)
			goto failed;
		lease->clocks |= 1U << i;
	}
	if (clocks & (1U << S31_CRYPTO_KEY_MANAGER)) {
		ret = s31_linux_crypto_operation(S31_CRYPTO_CLOCK_ON, S31_CRYPTO_KEY_MANAGER);
		if (ret)
			goto failed;
		lease->clocks |= 1U << S31_CRYPTO_KEY_MANAGER;
	}
	return 0;
failed:
	s31_sdk_crypto_end(lease);
	return ret;
}

int s31_sdk_crypto_fail(struct s31_sdk_crypto_lease *lease)
{
	if (lease->held) {
		s31_linux_crypto_operation(S31_CRYPTO_FAULT, lease->domain);
		s31_sdk_crypto_end(lease);
	}
	return S31_SDK_CRYPTO_FAILURE;
}
