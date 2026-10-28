/* SPDX-License-Identifier: Apache-2.0 */
#ifndef S31_RSA_H
#define S31_RSA_H
#define OPENSSL_SUPPRESS_DEPRECATED
#include <openssl/rsa.h>
#include <openssl/bn.h>
#include <openssl/param_build.h>
#include <stdatomic.h>
#include "provider.h"
struct s31_rsa_key {
	struct s31_provider *provider;
	EVP_PKEY *parameters;
	RSA *math;
	atomic_uint references;
	int fd, private;
	unsigned char *dsdata;
	size_t dslen;
};
void s31_rsa_free(void *key);
struct s31_rsa_key *s31_rsa_ref(struct s31_rsa_key *key);
struct s31_rsa_key *s31_rsa_ds_load(struct s31_provider *provider, const void *data, size_t length);
extern const OSSL_DISPATCH s31_ds_store[];
extern const OSSL_DISPATCH s31_rsa_keymgmt[];
extern const OSSL_DISPATCH s31_rsa_signature[];
extern const OSSL_DISPATCH s31_rsa_cipher[];
int s31_rsa_method_init(struct s31_provider *provider);
void s31_rsa_method_free(struct s31_provider *provider);
#endif
