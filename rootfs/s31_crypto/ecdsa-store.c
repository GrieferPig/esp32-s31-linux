/* SPDX-License-Identifier: Apache-2.0 */
#include "ec.h"
#include <openssl/core_object.h>
#include <openssl/store.h>
#include <linux/esp32s31-crypto.h>
#include "key-file.h"

struct ec_store { struct s31_ec_key *key; int done; };
static void *store_open(void *provider, const char *uri)
{
	struct ec_store *ctx; unsigned int bits, low, high = ESP32S31_CRYPTO_SLOT_UNUSED;
	int used = 0, count;
	if (strncmp(uri, "s31-ecdsa:", 10)) return NULL;
	if (!strncmp(uri + 10, "km:", 3)) {
		struct esp32s31_crypto_km_key recovery;
		if (!s31_key_file(uri + 13, &recovery, sizeof(recovery))) return NULL;
		ctx = OPENSSL_zalloc(sizeof(*ctx));
		if (ctx) ctx->key = s31_ec_km_load(provider, &recovery);
		OPENSSL_cleanse(&recovery, sizeof(recovery));
		if (ctx && !ctx->key) { OPENSSL_free(ctx); ctx = NULL; }
		return ctx;
	}
	count = sscanf(uri + 10, "%u:%u:%u%n", &bits, &low, &high, &used);
	if (count != 3) {
		used = 0; high = ESP32S31_CRYPTO_SLOT_UNUSED;
		count = sscanf(uri + 10, "%u:%u%n", &bits, &low, &used);
	}
	if (count < 2 || !used || uri[10 + used] ||
	    (bits != 192 && bits != 256 && bits != 384) ||
	    low >= ESP32S31_CRYPTO_EFUSE_SLOTS ||
	    (bits == 384 ? high >= ESP32S31_CRYPTO_EFUSE_SLOTS || high == low :
			    high != ESP32S31_CRYPTO_SLOT_UNUSED)) {
		errno = EINVAL; s31_error("ECDSA URI"); return NULL;
	}
	ctx = OPENSSL_zalloc(sizeof(*ctx));
	if (!ctx) return NULL;
	ctx->key = s31_ec_efuse_load(provider, bits, low, high);
	if (!ctx->key) { OPENSSL_free(ctx); return NULL; }
	return ctx;
}
static int store_close(void *opaque)
{
	struct ec_store *ctx = opaque;
	if (ctx) { s31_ec_release(ctx->key); OPENSSL_free(ctx); }
	return 1;
}
static int store_set(void *opaque, const OSSL_PARAM params[])
{
	const OSSL_PARAM *p; int expected; const char *properties; (void)opaque;
	if (!params) return 1;
	for (p = params; p->key; p++) {
		if (!strcmp(p->key, OSSL_STORE_PARAM_EXPECT)) {
			if (!OSSL_PARAM_get_int(p, &expected) ||
			    (expected && expected != OSSL_STORE_INFO_PKEY && expected != OSSL_STORE_INFO_PUBKEY)) return 0;
		} else if (!strcmp(p->key, OSSL_STORE_PARAM_PROPERTIES)) {
			if (!OSSL_PARAM_get_utf8_string_ptr(p, &properties)) return 0;
		} else return 0;
	}
	return 1;
}
static const OSSL_PARAM *store_settable(void *provider)
{
	static const OSSL_PARAM p[] = {
		OSSL_PARAM_int(OSSL_STORE_PARAM_EXPECT, NULL),
		OSSL_PARAM_utf8_string(OSSL_STORE_PARAM_PROPERTIES, NULL, 0), OSSL_PARAM_END
	};
	(void)provider; return p;
}
static int store_load(void *opaque, OSSL_CALLBACK *object, void *arg,
		       OSSL_PASSPHRASE_CALLBACK *password, void *password_arg)
{
	struct ec_store *ctx = opaque; int type = OSSL_OBJECT_PKEY;
	OSSL_PARAM params[] = {
		OSSL_PARAM_int(OSSL_OBJECT_PARAM_TYPE, &type),
		OSSL_PARAM_utf8_string(OSSL_OBJECT_PARAM_DATA_TYPE, "EC", 0),
		OSSL_PARAM_octet_string(OSSL_OBJECT_PARAM_REFERENCE, &ctx->key, sizeof(ctx->key)),
		OSSL_PARAM_END
	};
	(void)password; (void)password_arg;
	if (ctx->done) return 0;
	ctx->done = 1; return object(params, arg);
}
static int store_eof(void *opaque) { return ((struct ec_store *)opaque)->done; }
#define S(name, fn) { OSSL_FUNC_STORE_##name, (void (*)(void))fn }
const OSSL_DISPATCH s31_ecdsa_store[] = {
	S(OPEN, store_open), S(CLOSE, store_close), S(LOAD, store_load), S(EOF, store_eof),
	S(SET_CTX_PARAMS, store_set), S(SETTABLE_CTX_PARAMS, store_settable), { 0, NULL }
};
