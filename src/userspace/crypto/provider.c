/* SPDX-License-Identifier: Apache-2.0 */
#include <string.h>
#include "rsa.h"
#include "ec.h"

static const OSSL_ALGORITHM keys[] = {
	{ "EC:id-ecPublicKey:1.2.840.10045.2.1", "provider=s31", s31_ec_keymgmt,
	  "ESP32-S31 ECDH and existing-slot ECDSA keys" },
	{ "RSA:rsaEncryption:1.2.840.113549.1.1.1", "provider=s31", s31_rsa_keymgmt, "S31 RSA keys" },
	{ NULL, NULL, NULL, NULL }
};
static const OSSL_ALGORITHM signatures[] = {
 { "RSA:rsaEncryption:1.2.840.113549.1.1.1", "provider=s31", s31_rsa_signature, "S31 native RSA signatures" },
 { "ECDSA", "provider=s31", s31_ecdsa_signature, "S31 native verification and eFuse ECDSA signing" },
 { NULL,NULL,NULL,NULL }
};
static const OSSL_ALGORITHM asymciphers[] = {
 { "RSA:rsaEncryption:1.2.840.113549.1.1.1", "provider=s31", s31_rsa_cipher, "S31 native RSA cipher" }, { NULL,NULL,NULL,NULL }
};
static const OSSL_ALGORITHM exchanges[] = {
	{ "ECDH", "provider=s31", s31_ecdh, "ESP32-S31 native queued ECDH" },
	{ NULL, NULL, NULL, NULL }
};
static const OSSL_ALGORITHM stores[] = {
	{ "s31-ds", "provider=s31", s31_ds_store, "S31 encrypted DS RSA key bundle" },
	{ "s31-ecdsa", "provider=s31", s31_ecdsa_store, "S31 existing eFuse ECDSA key" },
	{ NULL, NULL, NULL, NULL }
};
static const OSSL_ALGORITHM *query(void *ctx, int operation, int *no_cache)
{
	(void)ctx; *no_cache = 0;
	switch (operation) {
	case OSSL_OP_KEYMGMT: return keys;
	case OSSL_OP_KEYEXCH: return exchanges;
	case OSSL_OP_DIGEST: return s31_digests;
	case OSSL_OP_MAC: return s31_macs;
	case OSSL_OP_CIPHER: return s31_ciphers;
	case OSSL_OP_SIGNATURE: return signatures;
	case OSSL_OP_ASYM_CIPHER: return asymciphers;
	case OSSL_OP_STORE: return stores;
	default: return NULL;
	}
}
static const OSSL_PARAM *gettable(void *ctx)
{
	static const OSSL_PARAM p[] = {
		OSSL_PARAM_utf8_ptr(OSSL_PROV_PARAM_NAME, NULL, 0),
		OSSL_PARAM_utf8_ptr(OSSL_PROV_PARAM_VERSION, NULL, 0),
		OSSL_PARAM_int(OSSL_PROV_PARAM_STATUS, NULL), OSSL_PARAM_END
	};
	(void)ctx; return p;
}
static int parameters(void *ctx, OSSL_PARAM params[])
{
	OSSL_PARAM *p; (void)ctx;
	if ((p = OSSL_PARAM_locate(params, OSSL_PROV_PARAM_NAME)) &&
	    !OSSL_PARAM_set_utf8_ptr(p, "ESP32-S31 native Linux crypto")) return 0;
	if ((p = OSSL_PARAM_locate(params, OSSL_PROV_PARAM_VERSION)) &&
	    !OSSL_PARAM_set_utf8_ptr(p, "1.0")) return 0;
	if ((p = OSSL_PARAM_locate(params, OSSL_PROV_PARAM_STATUS)) &&
	    !OSSL_PARAM_set_int(p, 1)) return 0;
	return 1;
}
static void teardown(void *opaque)
{
	struct s31_provider *ctx = opaque;
	s31_rsa_method_free(ctx); OSSL_LIB_CTX_free(ctx->libctx); OPENSSL_free(ctx);
}
static const OSSL_DISPATCH functions[] = {
	{ OSSL_FUNC_PROVIDER_QUERY_OPERATION, (void (*)(void))query },
	{ OSSL_FUNC_PROVIDER_GETTABLE_PARAMS, (void (*)(void))gettable },
	{ OSSL_FUNC_PROVIDER_GET_PARAMS, (void (*)(void))parameters },
	{ OSSL_FUNC_PROVIDER_TEARDOWN, (void (*)(void))teardown }, { 0, NULL }
};
int OSSL_provider_init(const OSSL_CORE_HANDLE *handle, const OSSL_DISPATCH *in,
		       const OSSL_DISPATCH **out, void **provider)
{
	struct s31_provider *ctx = OPENSSL_zalloc(sizeof(*ctx));
	if (!ctx) return 0;
	ctx->libctx = OSSL_LIB_CTX_new_child(handle, in);
	if (!ctx->libctx || !s31_rsa_method_init(ctx)) { OSSL_LIB_CTX_free(ctx->libctx); OPENSSL_free(ctx); return 0; }
	*provider = ctx; *out = functions; return 1;
}
