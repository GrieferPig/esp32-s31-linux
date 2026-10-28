/* SPDX-License-Identifier: Apache-2.0 */
#include "ec.h"
#include <fcntl.h>
#include <sys/ioctl.h>
#include <linux/esp32s31-crypto.h>
#include <openssl/ecdsa.h>
#include <openssl/x509.h>

struct ec_signature {
	struct s31_ec_key *key;
	const struct s31_hash_kind *kind;
	struct s31_hash hash;
	int fd, signing;
};

static void signature_reset(struct ec_signature *ctx)
{
	if (ctx->fd >= 0) close(ctx->fd);
	ctx->fd = -1; s31_hash_close(&ctx->hash);
	s31_ec_release(ctx->key); ctx->key = NULL; ctx->kind = NULL;
}
static void *signature_new(void *provider, const char *properties)
{
	struct ec_signature *ctx = OPENSSL_zalloc(sizeof(*ctx));
	(void)provider; (void)properties;
	if (ctx) { ctx->fd = ctx->hash.parent = ctx->hash.operation = -1; }
	return ctx;
}
static void signature_free(void *opaque)
{
	struct ec_signature *ctx = opaque;
	if (ctx) { signature_reset(ctx); OPENSSL_clear_free(ctx, sizeof(*ctx)); }
}
static void *signature_dup(void *opaque)
{
	struct ec_signature *from = opaque, *to = signature_new(NULL, NULL);
	if (!to) return NULL;
	to->kind = from->kind; to->signing = from->signing;
	to->key = s31_ec_reference(from->key);
	if (from->fd >= 0 && (to->fd = fcntl(from->fd, F_DUPFD_CLOEXEC, 0)) < 0) goto fail;
	if (from->hash.operation >= 0 && !s31_hash_copy(&to->hash, &from->hash)) goto fail;
	return to;
fail:
	signature_free(to); return NULL;
}
static int signature_params(void *opaque, const OSSL_PARAM params[])
{
	struct ec_signature *ctx = opaque; const OSSL_PARAM *p; const char *name;
	const struct s31_hash_kind *kind;
	if (!params) return 1;
	for (p = params; p->key; p++) {
		if (!strcmp(p->key, OSSL_SIGNATURE_PARAM_DIGEST)) {
			if (!OSSL_PARAM_get_utf8_string_ptr(p, &name) ||
			    !(kind = s31_hash_lookup(name)) ||
			    (ctx->hash.operation >= 0 && ctx->kind != kind)) return 0;
			ctx->kind = kind;
		} else if (!strcmp(p->key, OSSL_SIGNATURE_PARAM_PROPERTIES)) {
			if (!OSSL_PARAM_get_utf8_string_ptr(p, &name) ||
			    (*name && strcmp(name, "provider=s31"))) return 0;
		} else return 0;
	}
	return 1;
}
static const OSSL_PARAM *signature_settable(void *ctx, void *provider)
{
	static const OSSL_PARAM p[] = {
		OSSL_PARAM_utf8_string(OSSL_SIGNATURE_PARAM_DIGEST, NULL, 0),
		OSSL_PARAM_utf8_string(OSSL_SIGNATURE_PARAM_PROPERTIES, NULL, 0), OSSL_PARAM_END
	};
	(void)ctx; (void)provider; return p;
}
static int signature_init(struct ec_signature *ctx, struct s31_ec_key *key,
			   const OSSL_PARAM params[], int signing)
{
	unsigned char point[97]; size_t length;
	struct esp32s31_crypto_import request = { };
	signature_reset(ctx);
	if (!key || !key->parameters || (signing && !key->efuse)) {
		errno = signing ? EOPNOTSUPP : EINVAL; return s31_error("ECDSA key");
	}
	ctx->key = s31_ec_reference(key); ctx->signing = signing;
	if (signing) {
		ctx->fd = fcntl(key->fd, F_DUPFD_CLOEXEC, 0);
		if (ctx->fd < 0) goto fail;
	} else {
		if (!EVP_PKEY_get_octet_string_param(key->parameters, OSSL_PKEY_PARAM_PUB_KEY,
						     point, sizeof(point), &length)) goto fail;
		request.algorithm = key->bits == 192 ? ESP32S31_CRYPTO_ECDSA_P192 :
				    key->bits == 256 ? ESP32S31_CRYPTO_ECDSA_P256 : ESP32S31_CRYPTO_ECDSA_P384;
		request.key = (uintptr_t)point; request.key_len = length;
		ctx->fd = open("/dev/s31-crypto", O_RDWR | O_CLOEXEC);
		if (ctx->fd < 0 || ioctl(ctx->fd, ESP32S31_CRYPTO_IOC_IMPORT, &request)) goto fail;
	}
	if (!signature_params(ctx, params)) goto fail;
	return 1;
fail:
	signature_reset(ctx); return s31_error("ECDSA initialization");
}
static int sign_init(void *ctx, void *key, const OSSL_PARAM params[])
{ return signature_init(ctx, key, params, 1); }
static int verify_init(void *ctx, void *key, const OSSL_PARAM params[])
{ return signature_init(ctx, key, params, 0); }

static int sign_raw(void *opaque, unsigned char *signature, size_t *length, size_t capacity,
		     const unsigned char *digest, size_t size)
{
	struct ec_signature *ctx = opaque; unsigned char raw[96]; unsigned char *out = signature;
	BIGNUM *r = NULL, *s = NULL; ECDSA_SIG *encoded = NULL; int result = 0, n;
	struct esp32s31_crypto_request request = {
		.operation = ESP32S31_CRYPTO_ECDSA_SIGN, .input = (uintptr_t)digest,
		.input_len = size, .output = (uintptr_t)raw, .output_len = sizeof(raw),
	};
	if (!ctx->key || !ctx->signing || !length) return 0;
	if (!signature) { *length = ctx->key->bits / 4 + 8; return 1; }
	*length = 0;
	if (!size || size > 64 || (ctx->kind && size != ctx->kind->size) ||
	    capacity < (size_t)(ctx->key->bits / 4 + 8)) { errno = EINVAL; goto out; }
	if (ioctl(ctx->fd, ESP32S31_CRYPTO_IOC_REQUEST, &request) ||
	    request.output_len != (unsigned)ctx->key->bits / 4) goto out;
	n = ctx->key->bits / 8;
	r = BN_bin2bn(raw, n, NULL); s = BN_bin2bn(raw + n, n, NULL); encoded = ECDSA_SIG_new();
	if (!r || !s || !encoded || !ECDSA_SIG_set0(encoded, r, s)) goto out;
	r = s = NULL; n = i2d_ECDSA_SIG(encoded, NULL);
	if (n <= 0 || (size_t)n > capacity || i2d_ECDSA_SIG(encoded, &out) != n) goto out;
	*length = n; result = 1;
out:
	BN_clear_free(r); BN_clear_free(s); ECDSA_SIG_free(encoded); OPENSSL_cleanse(raw, sizeof(raw));
	if (!result) s31_error("ECDSA sign");
	return result;
}
static int verify_raw(void *opaque, const unsigned char *signature, size_t length,
		       const unsigned char *digest, size_t size)
{
	struct ec_signature *ctx = opaque;
	const unsigned char *cursor = signature; const BIGNUM *r, *s;
	unsigned char input[160], canonical[104], *out = canonical;
	ECDSA_SIG *encoded = NULL; int ret = 0, width, count;
	struct esp32s31_crypto_request request = { .operation = ESP32S31_CRYPTO_ECDSA_VERIFY };
	if (!ctx->key || ctx->signing || !signature || !size || size > 64 ||
	    (ctx->kind && size != ctx->kind->size)) return 0;
	width = ctx->key->bits / 8;
	if (length > (size_t)(2 * width + 8)) return 0;
	encoded = d2i_ECDSA_SIG(NULL, &cursor, length);
	if (!encoded || cursor != signature + length ||
	    (count = i2d_ECDSA_SIG(encoded, NULL)) <= 0 ||
	    (size_t)count > sizeof(canonical) || i2d_ECDSA_SIG(encoded, &out) != count ||
	    (size_t)count != length || memcmp(canonical, signature, length)) goto done;
	ECDSA_SIG_get0(encoded, &r, &s);
	if (BN_is_negative(r) || BN_is_negative(s) ||
	    BN_bn2binpad(r, input + size, width) != width ||
	    BN_bn2binpad(s, input + size + width, width) != width) goto done;
	memcpy(input, digest, size);
	request.input = (uintptr_t)input; request.input_len = size + 2 * width;
	if (!ioctl(ctx->fd, ESP32S31_CRYPTO_IOC_REQUEST, &request))
		ret = 1;
	else if (errno != EBADMSG && errno != EKEYREJECTED) {
		s31_error("ECDSA verification"); ret = -1;
	}
done:
	ECDSA_SIG_free(encoded); OPENSSL_cleanse(input, sizeof(input)); return ret;
}
static int digest_init(struct ec_signature *ctx, const char *name,
			struct s31_ec_key *key, const OSSL_PARAM params[], int signing)
{
	if (!signature_init(ctx, key, params, signing)) return 0;
	ctx->kind = s31_hash_lookup(name ? name : "SHA256");
	return ctx->kind && s31_hash_open(&ctx->hash, ctx->kind, NULL, 0, 0);
}
static int digest_sign_init(void *ctx, const char *name, void *key, const OSSL_PARAM p[])
{ return digest_init(ctx, name, key, p, 1); }
static int digest_verify_init(void *ctx, const char *name, void *key, const OSSL_PARAM p[])
{ return digest_init(ctx, name, key, p, 0); }
static int digest_update(void *opaque, const unsigned char *data, size_t length)
{ return s31_hash_update(&((struct ec_signature *)opaque)->hash, data, length); }
static int digest_copy_final(struct ec_signature *ctx, unsigned char *digest, size_t *length)
{
	struct s31_hash copy = { .parent = -1, .operation = -1 }; int ret;
	ret = s31_hash_copy(&copy, &ctx->hash) && s31_hash_final(&copy, digest, length, 64);
	s31_hash_close(&copy); return ret;
}
static int digest_sign_final(void *opaque, unsigned char *signature, size_t *length, size_t capacity)
{
	struct ec_signature *ctx = opaque; unsigned char digest[64]; size_t size; int ret;
	if (!signature) return sign_raw(ctx, NULL, length, capacity, NULL, 0);
	ret = digest_copy_final(ctx, digest, &size) && sign_raw(ctx, signature, length, capacity, digest, size);
	OPENSSL_cleanse(digest, sizeof(digest)); return ret;
}
static int digest_verify_final(void *opaque, const unsigned char *signature, size_t length)
{
	struct ec_signature *ctx = opaque; unsigned char digest[64]; size_t size; int ret;
	ret = digest_copy_final(ctx, digest, &size) ? verify_raw(ctx, signature, length, digest, size) : -1;
	OPENSSL_cleanse(digest, sizeof(digest)); return ret;
}
static int message_hash(struct ec_signature *ctx, const unsigned char *data, size_t length,
			  unsigned char *digest, size_t *size)
{
	struct s31_hash hash = { .parent = -1, .operation = -1 }; int ret;
	ret = ctx->kind && s31_hash_open(&hash, ctx->kind, NULL, 0, 0) &&
	      s31_hash_update(&hash, data, length) && s31_hash_final(&hash, digest, size, 64);
	s31_hash_close(&hash); return ret;
}
static int digest_sign(void *opaque, unsigned char *signature, size_t *length, size_t capacity,
			const unsigned char *data, size_t input_length)
{
	struct ec_signature *ctx = opaque; unsigned char digest[64]; size_t size; int ret;
	if (!signature) return sign_raw(ctx, NULL, length, capacity, NULL, 0);
	ret = message_hash(ctx, data, input_length, digest, &size) &&
	      sign_raw(ctx, signature, length, capacity, digest, size);
	OPENSSL_cleanse(digest, sizeof(digest)); return ret;
}
static int digest_verify(void *opaque, const unsigned char *signature, size_t length,
			  const unsigned char *data, size_t input_length)
{
	struct ec_signature *ctx = opaque; unsigned char digest[64]; size_t size; int ret;
	ret = message_hash(ctx, data, input_length, digest, &size) ? verify_raw(ctx, signature, length, digest, size) : -1;
	OPENSSL_cleanse(digest, sizeof(digest)); return ret;
}
static int signature_get(void *opaque, OSSL_PARAM params[])
{
	struct ec_signature *ctx = opaque; OSSL_PARAM *p; int nid = NID_undef;
	if ((p = OSSL_PARAM_locate(params, OSSL_SIGNATURE_PARAM_DIGEST)) &&
	    !OSSL_PARAM_set_utf8_string(p, ctx->kind ? ctx->kind->name : "")) return 0;
	if ((p = OSSL_PARAM_locate(params, OSSL_SIGNATURE_PARAM_DIGEST_SIZE)) &&
	    !OSSL_PARAM_set_size_t(p, ctx->kind ? ctx->kind->size : 0)) return 0;
	if ((p = OSSL_PARAM_locate(params, OSSL_SIGNATURE_PARAM_ALGORITHM_ID))) {
		unsigned char der[32], *cursor = der; X509_ALGOR *algorithm; int length;
		if (!ctx->kind) return 0;
		if (ctx->kind->size == 20) nid = NID_ecdsa_with_SHA1;
		else if (!strcmp(ctx->kind->name, "SHA224")) nid = NID_ecdsa_with_SHA224;
		else if (!strcmp(ctx->kind->name, "SHA256")) nid = NID_ecdsa_with_SHA256;
		else if (!strcmp(ctx->kind->name, "SHA384")) nid = NID_ecdsa_with_SHA384;
		else if (!strcmp(ctx->kind->name, "SHA512")) nid = NID_ecdsa_with_SHA512;
		if (nid == NID_undef || !(algorithm = X509_ALGOR_new())) return 0;
		X509_ALGOR_set0(algorithm, OBJ_nid2obj(nid), V_ASN1_UNDEF, NULL);
		length = i2d_X509_ALGOR(algorithm, NULL);
		if (length <= 0 || (size_t)length > sizeof(der)) { X509_ALGOR_free(algorithm); return 0; }
		length = i2d_X509_ALGOR(algorithm, &cursor); X509_ALGOR_free(algorithm);
		if (length <= 0 || !OSSL_PARAM_set_octet_string(p, der, length)) return 0;
	}
	return 1;
}
static const OSSL_PARAM *signature_gettable(void *ctx, void *provider)
{
	static const OSSL_PARAM p[] = {
		OSSL_PARAM_utf8_string(OSSL_SIGNATURE_PARAM_DIGEST, NULL, 0),
		OSSL_PARAM_size_t(OSSL_SIGNATURE_PARAM_DIGEST_SIZE, NULL),
		OSSL_PARAM_octet_string(OSSL_SIGNATURE_PARAM_ALGORITHM_ID, NULL, 0), OSSL_PARAM_END
	};
	(void)ctx; (void)provider; return p;
}
#define E(name, function) { OSSL_FUNC_SIGNATURE_##name, (void (*)(void))function }
const OSSL_DISPATCH s31_ecdsa_signature[] = {
	E(NEWCTX, signature_new), E(FREECTX, signature_free), E(DUPCTX, signature_dup),
	E(SIGN_INIT, sign_init), E(SIGN, sign_raw), E(VERIFY_INIT, verify_init), E(VERIFY, verify_raw),
	E(DIGEST_SIGN_INIT, digest_sign_init), E(DIGEST_VERIFY_INIT, digest_verify_init),
	E(DIGEST_SIGN_UPDATE, digest_update), E(DIGEST_VERIFY_UPDATE, digest_update),
	E(DIGEST_SIGN_FINAL, digest_sign_final), E(DIGEST_VERIFY_FINAL, digest_verify_final),
	E(DIGEST_SIGN, digest_sign), E(DIGEST_VERIFY, digest_verify),
	E(SET_CTX_PARAMS, signature_params), E(SETTABLE_CTX_PARAMS, signature_settable),
	E(GET_CTX_PARAMS, signature_get), E(GETTABLE_CTX_PARAMS, signature_gettable), { 0, NULL }
};
