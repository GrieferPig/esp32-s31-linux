/* SPDX-License-Identifier: Apache-2.0 */
#ifndef S31_PROVIDER_H
#define S31_PROVIDER_H
#include <openssl/core.h>
#include <openssl/core_dispatch.h>
#include <openssl/core_names.h>
#include <openssl/crypto.h>
#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/params.h>
#include <errno.h>
#include <string.h>
#include <stdio.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/socket.h>
#include <unistd.h>

struct s31_provider { OSSL_LIB_CTX *libctx; struct rsa_meth_st *rsa_method; };
int s31_rsa_method_init(struct s31_provider *ctx);
void s31_rsa_method_free(struct s31_provider *ctx);
extern const OSSL_DISPATCH s31_rsa_keymgmt[], s31_rsa_signature[], s31_rsa_cipher[];
extern const OSSL_DISPATCH s31_ec_keymgmt[];
extern const OSSL_DISPATCH s31_ecdh[];
extern const OSSL_ALGORITHM s31_digests[];
extern const OSSL_ALGORITHM s31_macs[];
extern const OSSL_DISPATCH s31_protected_mac[];
extern const OSSL_ALGORITHM s31_ciphers[];

static inline int s31_error(const char *operation)
{
	ERR_raise_data(ERR_LIB_USER, ERR_R_OPERATION_FAIL, "s31 %s: %s", operation, strerror(errno));
	return 0;
}

struct s31_hash_kind { const char *name, *driver; size_t size, block; };
extern const struct s31_hash_kind s31_hash_kinds[];
const struct s31_hash_kind *s31_hash_lookup(const char *name);
struct s31_hash {
	int parent, operation;
	const struct s31_hash_kind *kind;
	int failed, completed, keyed;
	unsigned char pending[512];
	size_t pending_len;
};
void s31_hash_close(struct s31_hash *ctx);
int s31_hash_open(struct s31_hash *ctx, const struct s31_hash_kind *kind,
		  const void *key, size_t keylen, int keyed);
int s31_hash_update(void *ctx, const unsigned char *input, size_t length);
int s31_hash_final(void *ctx, unsigned char *output, size_t *length, size_t capacity);
int s31_hash_copy(struct s31_hash *to, const struct s31_hash *from);
#endif
