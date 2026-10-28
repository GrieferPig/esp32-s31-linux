/* SPDX-License-Identifier: Apache-2.0 */
#include <fcntl.h>
#include <stdatomic.h>
#include <string.h>
#include <sys/ioctl.h>
#include <openssl/bn.h>
#include <openssl/param_build.h>
#include <openssl/ec.h>
#include <linux/esp32s31-crypto.h>
#include "ec.h"

/* Immutable key representations may be shared by independent operation
 * contexts. The kernel serializes an individual file's transform safely.
 * Default-provider objects here supply parameter validation and interchange;
 * every advertised private ECDH operation goes to the native device.
 */
struct s31_ecdh_ctx { struct s31_ec_key *key, *peer; };
struct s31_ec_gen { struct s31_provider *provider; char group[32]; int selection; };
static void *key_new(void *provider)
{
	struct s31_ec_key *key = OPENSSL_zalloc(sizeof(*key));
	if (key) { key->provider = provider; key->fd = -1; atomic_init(&key->references, 1); }
	return key;
}
static void key_free(void *opaque)
{
	struct s31_ec_key *key = opaque;
	if (!key || atomic_fetch_sub_explicit(&key->references, 1, memory_order_acq_rel) != 1) return;
	if (key->fd >= 0) close(key->fd);
	EVP_PKEY_free(key->parameters); OPENSSL_clear_free(key, sizeof(*key));
}
static void *key_dup(const void *opaque, int selection)
{
	struct s31_ec_key *key = (void *)opaque; (void)selection;
	atomic_fetch_add_explicit(&key->references, 1, memory_order_relaxed); return key;
}
struct s31_ec_key *s31_ec_reference(struct s31_ec_key *key)
{
	return key ? key_dup(key, OSSL_KEYMGMT_SELECT_ALL) : NULL;
}
void s31_ec_release(struct s31_ec_key *key) { key_free(key); }
static int native_import(struct s31_ec_key *key)
{
	struct esp32s31_crypto_import request = { .flags = ESP32S31_CRYPTO_KEY_PRIVATE };
	BIGNUM *scalar = NULL;
	unsigned char bytes[48]; int ret = 0;
	if (!EVP_PKEY_get_bn_param(key->parameters, OSSL_PKEY_PARAM_PRIV_KEY, &scalar)) return 1;
	if (key->bits == 192) { BN_clear_free(scalar); errno = EOPNOTSUPP; return s31_error("P-192 ECDH"); }
	key->private = 1;
	request.algorithm = key->bits == 256 ? ESP32S31_CRYPTO_ECDH_P256 : ESP32S31_CRYPTO_ECDH_P384;
	request.key_len = key->bits / 8; request.key = (uintptr_t)bytes;
	if (BN_bn2binpad(scalar, bytes, request.key_len) != (int)request.key_len) goto out;
	key->fd = open("/dev/s31-crypto", O_RDWR | O_CLOEXEC);
	if (key->fd < 0 || ioctl(key->fd, ESP32S31_CRYPTO_IOC_IMPORT, &request)) {
		s31_error("EC key import"); goto out;
	}
	ret = 1;
out:
	BN_clear_free(scalar); OPENSSL_cleanse(bytes, sizeof(bytes)); return ret;
}
static int key_import(void *opaque, int selection, const OSSL_PARAM params[])
{
	struct s31_ec_key *key = opaque;
	EVP_PKEY_CTX *ctx; EVP_PKEY *candidate = NULL;
	char group[32]; size_t length; int ret = 0;
	/* Import replaces the representation, never mutates an object already
	 * referenced by an operation. OpenSSL constructs keys before sharing.
	 */
	if (key->parameters) return 0;
	ctx = EVP_PKEY_CTX_new_from_name(key->provider->libctx, "EC", "provider=default");
	if (!ctx || EVP_PKEY_fromdata_init(ctx) <= 0 ||
	    EVP_PKEY_fromdata(ctx, &candidate, selection | OSSL_KEYMGMT_SELECT_DOMAIN_PARAMETERS, (OSSL_PARAM *)params) <= 0) goto out;
	if (!EVP_PKEY_get_utf8_string_param(candidate, OSSL_PKEY_PARAM_GROUP_NAME, group, sizeof(group), &length)) goto out;
	if (!strcmp(group, "prime192v1") || !strcmp(group, "P-192")) key->bits = 192;
	else if (!strcmp(group, "prime256v1") || !strcmp(group, "P-256")) key->bits = 256;
	else if (!strcmp(group, "secp384r1") || !strcmp(group, "P-384")) key->bits = 384;
	else { errno = EOPNOTSUPP; s31_error("EC group"); goto out; }
	if (!EVP_PKEY_set_utf8_string_param(candidate,
		OSSL_PKEY_PARAM_EC_POINT_CONVERSION_FORMAT, "uncompressed")) goto out;
	key->parameters = candidate; candidate = NULL;
	ret = ((selection & OSSL_KEYMGMT_SELECT_PRIVATE_KEY) &&
	       OSSL_PARAM_locate_const(params, OSSL_PKEY_PARAM_PRIV_KEY)) ? native_import(key) : 1;
	if (!ret) {
		if (key->fd >= 0) close(key->fd);
		key->fd = -1; key->private = 0;
		EVP_PKEY_free(key->parameters); key->parameters = NULL;
	}
out:
	EVP_PKEY_free(candidate); EVP_PKEY_CTX_free(ctx); return ret;
}
static void *key_duplicate(const void *opaque, int selection)
{
	const struct s31_ec_key *source = opaque;
	struct s31_ec_key *copy = key_new(source->provider);
	OSSL_PARAM *params = NULL;
	if (!copy || !source->parameters) return copy;
	if (source->efuse && (selection & OSSL_KEYMGMT_SELECT_PRIVATE_KEY)) {
		key_free(copy);
		return s31_ec_reference((void *)source);
	}
	selection |= OSSL_KEYMGMT_SELECT_DOMAIN_PARAMETERS;
	if (!EVP_PKEY_todata(source->parameters, selection, &params) ||
	    !key_import(copy, selection, params)) {
		key_free(copy); copy = NULL;
	}
	if (params) for (OSSL_PARAM *p = params; p->key; p++)
		if (p->data) OPENSSL_cleanse(p->data, p->data_size);
	OSSL_PARAM_free(params); return copy;
}
static const OSSL_PARAM *key_types(int selection)
{
	static const OSSL_PARAM p[] = {
		OSSL_PARAM_utf8_string(OSSL_PKEY_PARAM_GROUP_NAME, NULL, 0),
		OSSL_PARAM_octet_string(OSSL_PKEY_PARAM_PUB_KEY, NULL, 0),
		OSSL_PARAM_BN(OSSL_PKEY_PARAM_PRIV_KEY, NULL, 0), OSSL_PARAM_END
	};
	(void)selection; return p;
}
static int key_export(void *opaque, int selection, OSSL_CALLBACK *callback, void *arg)
{
	struct s31_ec_key *key = opaque; OSSL_PARAM *params = NULL; int ret;
	if (!key->parameters || (key->efuse && (selection & OSSL_KEYMGMT_SELECT_PRIVATE_KEY)) ||
	    !EVP_PKEY_todata(key->parameters, selection, &params)) return 0;
	ret = callback(params, arg);
	/* Parameters may contain the imported private scalar. */
	for (OSSL_PARAM *p = params; p->key; p++) if (p->data) OPENSSL_cleanse(p->data, p->data_size);
	OSSL_PARAM_free(params); return ret;
}
static int key_get(void *opaque, OSSL_PARAM params[])
{
	struct s31_ec_key *key = opaque;
	return key->parameters && EVP_PKEY_get_params(key->parameters, params);
}
static const OSSL_PARAM *key_gettable(void *provider)
{
	static const OSSL_PARAM p[] = {
		OSSL_PARAM_int(OSSL_PKEY_PARAM_BITS, NULL),
		OSSL_PARAM_int(OSSL_PKEY_PARAM_SECURITY_BITS, NULL),
		OSSL_PARAM_int(OSSL_PKEY_PARAM_MAX_SIZE, NULL),
		OSSL_PARAM_utf8_string(OSSL_PKEY_PARAM_DEFAULT_DIGEST, NULL, 0),
		OSSL_PARAM_utf8_string(OSSL_PKEY_PARAM_GROUP_NAME, NULL, 0),
		OSSL_PARAM_octet_string(OSSL_PKEY_PARAM_PUB_KEY, NULL, 0),
		OSSL_PARAM_octet_string(OSSL_PKEY_PARAM_ENCODED_PUBLIC_KEY, NULL, 0),
		OSSL_PARAM_BN(OSSL_PKEY_PARAM_PRIV_KEY, NULL, 0), OSSL_PARAM_END
	};
	(void)provider; return p;
}
static int key_has(const void *opaque, int selection)
{
	const struct s31_ec_key *key = opaque; unsigned char point[97]; size_t length;
	if (!key->parameters || ((selection & OSSL_KEYMGMT_SELECT_PRIVATE_KEY) && !key->private)) return 0;
	if ((selection & OSSL_KEYMGMT_SELECT_PUBLIC_KEY) &&
	    !EVP_PKEY_get_octet_string_param(key->parameters, OSSL_PKEY_PARAM_PUB_KEY, point, sizeof(point), &length)) return 0;
	return 1;
}
static int key_validate(const void *opaque, int selection, int type)
{
	const struct s31_ec_key *key = opaque;
	EVP_PKEY_CTX *ctx; int ret = 0; (void)type;
	if (!key_has(key, selection)) return 0;
	ctx = EVP_PKEY_CTX_new_from_pkey(key->provider->libctx, key->parameters, "provider=default");
	if (!ctx) return 0;
	ret = EVP_PKEY_param_check(ctx) > 0;
	if (ret && (selection & OSSL_KEYMGMT_SELECT_PUBLIC_KEY)) ret = EVP_PKEY_public_check(ctx) > 0;
	if (ret && (selection & OSSL_KEYMGMT_SELECT_PRIVATE_KEY) && !key->efuse)
		ret = EVP_PKEY_private_check(ctx) > 0;
	EVP_PKEY_CTX_free(ctx); return ret;
}
static int key_match(const void *one, const void *two, int selection)
{
	const struct s31_ec_key *a = one, *b = two;
	if (!a->parameters || !b->parameters || a->bits != b->bits) return 0;
	if (selection & OSSL_KEYMGMT_SELECT_PRIVATE_KEY) {
		BIGNUM *x = NULL, *y = NULL; int ret;
		if (a->efuse || b->efuse)
			return a->efuse && b->efuse && a->slot == b->slot &&
			       a->slot_high == b->slot_high &&
			       EVP_PKEY_eq(a->parameters, b->parameters) == 1;
		ret = a->private && b->private &&
			EVP_PKEY_get_bn_param(a->parameters, OSSL_PKEY_PARAM_PRIV_KEY, &x) &&
			EVP_PKEY_get_bn_param(b->parameters, OSSL_PKEY_PARAM_PRIV_KEY, &y) && BN_cmp(x, y) == 0;
		BN_clear_free(x); BN_clear_free(y); if (!ret) return 0;
	}
	return selection & OSSL_KEYMGMT_SELECT_PUBLIC_KEY ? EVP_PKEY_eq(a->parameters, b->parameters) == 1 : 1;
}
static const char *key_operation(int operation)
{
	return operation == OSSL_OP_KEYEXCH ? "ECDH" :
	       operation == OSSL_OP_SIGNATURE ? "ECDSA" : NULL;
}
static int gen_set(void *opaque, const OSSL_PARAM params[])
{
	struct s31_ec_gen *ctx = opaque; const OSSL_PARAM *p; const char *group;
	if (!params || !(p = OSSL_PARAM_locate_const(params, OSSL_PKEY_PARAM_GROUP_NAME))) return 1;
	if (!OSSL_PARAM_get_utf8_string_ptr(p, &group) || strlen(group) >= sizeof(ctx->group)) return 0;
	strcpy(ctx->group, group); return 1;
}
static void *gen_init(void *provider, int selection, const OSSL_PARAM params[])
{
	struct s31_ec_gen *ctx = OPENSSL_zalloc(sizeof(*ctx));
	if (!ctx) return NULL;
	ctx->provider = provider; ctx->selection = selection;
	strcpy(ctx->group, "prime256v1");
	if (!gen_set(ctx, params)) { OPENSSL_free(ctx); return NULL; }
	return ctx;
}
static const OSSL_PARAM *gen_settable(void *gen, void *provider)
{
	static const OSSL_PARAM p[] = { OSSL_PARAM_utf8_string(OSSL_PKEY_PARAM_GROUP_NAME, NULL, 0), OSSL_PARAM_END };
	(void)gen; (void)provider; return p;
}
static void *gen_run(void *opaque, OSSL_CALLBACK *callback, void *arg)
{
	struct s31_ec_gen *gen = opaque; struct s31_ec_key *key = NULL;
	EVP_PKEY_CTX *ctx; EVP_PKEY *temporary = NULL; OSSL_PARAM *params = NULL;
	OSSL_PARAM group[] = { OSSL_PARAM_utf8_string(OSSL_PKEY_PARAM_GROUP_NAME, gen->group, 0), OSSL_PARAM_END };
	(void)callback; (void)arg;
	ctx = EVP_PKEY_CTX_new_from_name(gen->provider->libctx, "EC", "provider=default");
	if (!ctx || EVP_PKEY_keygen_init(ctx) <= 0 || EVP_PKEY_CTX_set_params(ctx, group) <= 0 ||
	    EVP_PKEY_generate(ctx, &temporary) <= 0 || !EVP_PKEY_todata(temporary, gen->selection | OSSL_KEYMGMT_SELECT_DOMAIN_PARAMETERS, &params)) goto out;
	key = key_new(gen->provider);
	if (key && !key_import(key, gen->selection, params)) { key_free(key); key = NULL; }
out:
	if (params) for (OSSL_PARAM *p = params; p->key; p++) if (p->data) OPENSSL_cleanse(p->data, p->data_size);
	OSSL_PARAM_free(params); EVP_PKEY_free(temporary); EVP_PKEY_CTX_free(ctx); return key;
}
static void gen_free(void *ctx) { OPENSSL_free(ctx); }
static void *exchange_new(void *provider) { (void)provider; return OPENSSL_zalloc(sizeof(struct s31_ecdh_ctx)); }
static void exchange_free(void *opaque)
{
	struct s31_ecdh_ctx *ctx = opaque;
	if (ctx) { key_free(ctx->key); key_free(ctx->peer); OPENSSL_clear_free(ctx, sizeof(*ctx)); }
}
static int exchange_params(void *opaque, const OSSL_PARAM params[])
{
	const OSSL_PARAM *p; const char *s; int cofactor; (void)opaque;
	if (!params) return 1;
	p = OSSL_PARAM_locate_const(params, OSSL_EXCHANGE_PARAM_EC_ECDH_COFACTOR_MODE);
	if (p && (!OSSL_PARAM_get_int(p, &cofactor) || cofactor < -1 || cofactor > 1)) return 0;
	p = OSSL_PARAM_locate_const(params, OSSL_EXCHANGE_PARAM_KDF_TYPE);
	if (p && (!OSSL_PARAM_get_utf8_string_ptr(p, &s) || *s)) return 0;
	return 1;
}
static const OSSL_PARAM *exchange_settable(void *exchange, void *provider)
{
	static const OSSL_PARAM p[] = { OSSL_PARAM_int(OSSL_EXCHANGE_PARAM_EC_ECDH_COFACTOR_MODE, NULL), OSSL_PARAM_utf8_string(OSSL_EXCHANGE_PARAM_KDF_TYPE, NULL, 0), OSSL_PARAM_END };
	(void)exchange; (void)provider; return p;
}
static int exchange_init(void *opaque, void *key, const OSSL_PARAM params[])
{
	struct s31_ecdh_ctx *ctx = opaque;
	if (!key || !((struct s31_ec_key *)key)->private ||
	    ((struct s31_ec_key *)key)->efuse || !exchange_params(ctx, params)) return 0;
	key_free(ctx->key); key_free(ctx->peer); ctx->peer = NULL;
	ctx->key = key_dup(key, OSSL_KEYMGMT_SELECT_ALL); return 1;
}
static int exchange_peer(void *opaque, void *peer)
{
	struct s31_ecdh_ctx *ctx = opaque; struct s31_ec_key *key = peer;
	if (!ctx->key || !key || key->bits != ctx->key->bits ||
	    !key_validate(key, OSSL_KEYMGMT_SELECT_PUBLIC_KEY, OSSL_KEYMGMT_VALIDATE_FULL_CHECK)) return 0;
	key_free(ctx->peer); ctx->peer = key_dup(key, OSSL_KEYMGMT_SELECT_PUBLIC_KEY); return 1;
}
static void *exchange_dup(void *opaque)
{
	struct s31_ecdh_ctx *ctx = opaque, *copy = exchange_new(NULL);
	if (copy) { if (ctx->key) copy->key = key_dup(ctx->key, OSSL_KEYMGMT_SELECT_ALL); if (ctx->peer) copy->peer = key_dup(ctx->peer, OSSL_KEYMGMT_SELECT_PUBLIC_KEY); }
	return copy;
}
static int exchange_derive(void *opaque, unsigned char *output, size_t *length, size_t capacity)
{
	struct s31_ecdh_ctx *ctx = opaque; unsigned char point[97]; size_t len;
	struct esp32s31_crypto_request request = { .operation = ESP32S31_CRYPTO_ECDH_SHARED };
	if (!ctx->key || !ctx->peer || ctx->key->fd < 0) return 0;
	*length = ctx->key->bits / 8;
	if (!output) return 1;
	if (capacity < *length || !EVP_PKEY_get_octet_string_param(ctx->peer->parameters,
		OSSL_PKEY_PARAM_PUB_KEY, point, sizeof(point), &len) || len != 2 * *length + 1 || point[0] != 4) return 0;
	request.input_len = len - 1; request.input = (uintptr_t)(point + 1);
	request.output_len = *length; request.output = (uintptr_t)output;
	if (ioctl(ctx->key->fd, ESP32S31_CRYPTO_IOC_REQUEST, &request) || request.output_len != *length) {
		OPENSSL_cleanse(output, *length); *length = 0; return s31_error("ECDH derive");
	}
	return 1;
}
#define ENTRY(name, fn) { OSSL_FUNC_##name, (void (*)(void))fn }
static void *key_load(const void *reference, size_t length)
{
	struct s31_ec_key *key;
	if (length != sizeof(key)) return NULL;
	memcpy(&key, reference, sizeof(key));
	return s31_ec_reference(key);
}

static struct s31_ec_key *ec_protected_load(struct s31_provider *provider,
		unsigned int bits, unsigned int slot, unsigned int slot_high,
		const void *opaque_key, size_t opaque_size)
{
	struct s31_ec_key *key = key_new(provider);
	struct esp32s31_crypto_ecdsa_key handle = {
		.slot = slot, .slot_high = slot_high,
	};
	struct esp32s31_crypto_import import = {
		.flags = ESP32S31_CRYPTO_KEY_PRIVATE,
		.key_len = opaque_key ? opaque_size : sizeof(handle),
		.key = (uintptr_t)(opaque_key ? opaque_key : &handle),
	};
	unsigned char point[97]; int ret = 0;
	struct esp32s31_crypto_request request = {
		.operation = ESP32S31_CRYPTO_ECDSA_PUBLIC,
		.output = (uintptr_t)point, .output_len = sizeof(point),
	};
	const char *group = bits == 192 ? "prime192v1" : bits == 256 ? "prime256v1" :
			    bits == 384 ? "secp384r1" : NULL;
	OSSL_PARAM params[] = {
		OSSL_PARAM_utf8_string(OSSL_PKEY_PARAM_GROUP_NAME, (char *)group, 0),
		OSSL_PARAM_octet_string(OSSL_PKEY_PARAM_PUB_KEY, point, bits / 4 + 1),
		OSSL_PARAM_END
	};
	if (!key || !group) { errno = EINVAL; goto out; }
	import.algorithm = bits == 192 ? ESP32S31_CRYPTO_ECDSA_P192 :
			   bits == 256 ? ESP32S31_CRYPTO_ECDSA_P256 : ESP32S31_CRYPTO_ECDSA_P384;
	key->fd = open("/dev/s31-crypto", O_RDWR | O_CLOEXEC);
	if (key->fd < 0 || ioctl(key->fd, ESP32S31_CRYPTO_IOC_IMPORT, &import) ||
	    ioctl(key->fd, ESP32S31_CRYPTO_IOC_REQUEST, &request)) goto out;
	if (request.output_len != bits / 4 + 1 ||
	    !key_import(key, OSSL_KEYMGMT_SELECT_PUBLIC_KEY, params) ||
	    !key_validate(key, OSSL_KEYMGMT_SELECT_PUBLIC_KEY, OSSL_KEYMGMT_VALIDATE_FULL_CHECK)) {
		errno = EKEYREJECTED; goto out;
	}
	key->private = key->efuse = 1; key->slot = slot; key->slot_high = slot_high;
	ret = 1;
out:
	if (!ret) { int saved = errno; key_free(key); key = NULL; errno = saved; s31_error("ECDSA eFuse handle"); }
	return key;
}

struct s31_ec_key *s31_ec_efuse_load(struct s31_provider *provider,
		unsigned int bits, unsigned int slot, unsigned int slot_high)
{
	return ec_protected_load(provider, bits, slot, slot_high, NULL, 0);
}

struct s31_ec_key *s31_ec_km_load(struct s31_provider *provider,
		const struct esp32s31_crypto_km_key *handle)
{
	if (handle->type || handle->length > 2) { errno = EINVAL; return NULL; }
	return ec_protected_load(provider, handle->length == 0 ? 192 :
		handle->length == 1 ? 256 : 384, ESP32S31_CRYPTO_SLOT_UNUSED,
		ESP32S31_CRYPTO_SLOT_UNUSED, handle, sizeof(*handle));
}

const OSSL_DISPATCH s31_ec_keymgmt[] = {
	ENTRY(KEYMGMT_NEW, key_new), ENTRY(KEYMGMT_FREE, key_free), ENTRY(KEYMGMT_DUP, key_duplicate),
	ENTRY(KEYMGMT_LOAD, key_load),
	ENTRY(KEYMGMT_IMPORT, key_import), ENTRY(KEYMGMT_IMPORT_TYPES, key_types),
	ENTRY(KEYMGMT_EXPORT, key_export), ENTRY(KEYMGMT_EXPORT_TYPES, key_types),
	ENTRY(KEYMGMT_GET_PARAMS, key_get), ENTRY(KEYMGMT_GETTABLE_PARAMS, key_gettable),
	ENTRY(KEYMGMT_HAS, key_has), ENTRY(KEYMGMT_VALIDATE, key_validate), ENTRY(KEYMGMT_MATCH, key_match),
	ENTRY(KEYMGMT_QUERY_OPERATION_NAME, key_operation), ENTRY(KEYMGMT_GEN_INIT, gen_init),
	ENTRY(KEYMGMT_GEN_SET_PARAMS, gen_set), ENTRY(KEYMGMT_GEN_SETTABLE_PARAMS, gen_settable),
	ENTRY(KEYMGMT_GEN, gen_run), ENTRY(KEYMGMT_GEN_CLEANUP, gen_free), { 0, NULL }
};
const OSSL_DISPATCH s31_ecdh[] = {
	ENTRY(KEYEXCH_NEWCTX, exchange_new), ENTRY(KEYEXCH_FREECTX, exchange_free), ENTRY(KEYEXCH_DUPCTX, exchange_dup),
	ENTRY(KEYEXCH_INIT, exchange_init), ENTRY(KEYEXCH_SET_PEER, exchange_peer), ENTRY(KEYEXCH_DERIVE, exchange_derive),
	ENTRY(KEYEXCH_SET_CTX_PARAMS, exchange_params), ENTRY(KEYEXCH_SETTABLE_CTX_PARAMS, exchange_settable), { 0, NULL }
};
