/* SPDX-License-Identifier: Apache-2.0 */
#ifndef S31_EC_H
#define S31_EC_H
#include <stdatomic.h>
#include "provider.h"

struct s31_ec_key {
	struct s31_provider *provider;
	EVP_PKEY *parameters;
	atomic_uint references;
	int fd, bits, private, efuse;
	unsigned int slot, slot_high;
};
struct s31_ec_key *s31_ec_reference(struct s31_ec_key *key);
void s31_ec_release(struct s31_ec_key *key);
struct s31_ec_key *s31_ec_efuse_load(struct s31_provider *provider,
				    unsigned int bits, unsigned int slot,
				    unsigned int slot_high);
struct esp32s31_crypto_km_key;
struct s31_ec_key *s31_ec_km_load(struct s31_provider *provider,
		const struct esp32s31_crypto_km_key *handle);
extern const OSSL_DISPATCH s31_ecdsa_signature[], s31_ecdsa_store[];
#endif
