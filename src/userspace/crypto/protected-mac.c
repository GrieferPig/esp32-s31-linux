/* SPDX-License-Identifier: Apache-2.0 */
#include <fcntl.h>
#include <linux/esp32s31-crypto.h>
#include <sys/ioctl.h>
#include "provider.h"
#include "key-file.h"

/* The ordinary HMAC implementation accepts application key bytes. This
 * separate EVP_MAC algorithm accepts only an opaque, existing eFuse slot.
 * Buffering is bounded because hardware ownership cannot span application
 * update calls or system suspend. Each final operation is one native job.
 */
struct protected_mac {
	int fd, have_slot, active;
	unsigned int slot;
	int have_km;
	struct esp32s31_crypto_km_key km;
	unsigned char *input;
	size_t length, capacity;
};

static void protected_reset(struct protected_mac *ctx)
{
	if (ctx->fd >= 0) close(ctx->fd);
	ctx->fd = -1; ctx->active = 0;
	OPENSSL_clear_free(ctx->input, ctx->capacity);
	ctx->input = NULL; ctx->length = ctx->capacity = 0;
}

static void *protected_new(void *provider)
{
	struct protected_mac *ctx = OPENSSL_zalloc(sizeof(*ctx));
	(void)provider;
	if (ctx) ctx->fd = -1;
	return ctx;
}

static void protected_free(void *opaque)
{
	struct protected_mac *ctx = opaque;
	if (ctx) { protected_reset(ctx); OPENSSL_clear_free(ctx, sizeof(*ctx)); }
}

static int protected_open(struct protected_mac *ctx)
{
	uint32_t slot = ctx->slot;
	struct esp32s31_crypto_import key = {
		.algorithm = ESP32S31_CRYPTO_HMAC_EFUSE,
		.flags = ESP32S31_CRYPTO_KEY_PRIVATE,
		.key_len = sizeof(slot), .key = (uintptr_t)&slot,
	};
	if (ctx->have_km) {
		key.algorithm = ESP32S31_CRYPTO_HMAC_KEY_MANAGER;
		key.key_len = sizeof(ctx->km); key.key = (uintptr_t)&ctx->km;
	}
	ctx->fd = open("/dev/s31-crypto", O_RDWR | O_CLOEXEC);
	if (ctx->fd < 0 || ioctl(ctx->fd, ESP32S31_CRYPTO_IOC_IMPORT, &key)) {
		s31_error("protected HMAC slot"); protected_reset(ctx); return 0;
	}
	return 1;
}

static int protected_set(void *opaque, const OSSL_PARAM params[])
{
	struct protected_mac *ctx = opaque;
	const OSSL_PARAM *p; const char *name;
	unsigned int slot;
	if (!params) return 1;
	for (p = params; p->key; p++) {
		if (!strcmp(p->key, "s31-km-file")) {
			if (ctx->active || !OSSL_PARAM_get_utf8_string_ptr(p, &name) ||
			    !s31_key_file(name, &ctx->km, sizeof(ctx->km))) goto invalid;
			ctx->have_km = 1; ctx->have_slot = 0;
		} else if (!strcmp(p->key, "s31-km-key")) {
			if (ctx->active || p->data_type != OSSL_PARAM_OCTET_STRING ||
			    !p->data || p->data_size != sizeof(ctx->km)) goto invalid;
			memcpy(&ctx->km, p->data, sizeof(ctx->km));
			ctx->have_km = 1; ctx->have_slot = 0;
		} else if (!strcmp(p->key, "s31-slot")) {
			if (ctx->active || !OSSL_PARAM_get_uint(p, &slot) ||
			    slot >= ESP32S31_CRYPTO_EFUSE_SLOTS) goto invalid;
			ctx->slot = slot; ctx->have_slot = 1; ctx->have_km = 0;
			OPENSSL_cleanse(&ctx->km, sizeof(ctx->km));
		} else if (!strcmp(p->key, OSSL_MAC_PARAM_DIGEST)) {
			if (!OSSL_PARAM_get_utf8_string_ptr(p, &name) ||
			    s31_hash_lookup(name) != &s31_hash_kinds[2]) goto invalid;
		} else if (!strcmp(p->key, OSSL_MAC_PARAM_PROPERTIES)) {
			if (!OSSL_PARAM_get_utf8_string_ptr(p, &name) ||
			    (*name && strcmp(name, "provider=s31"))) goto invalid;
		} else goto invalid;
	}
	return 1;
invalid:
	protected_reset(ctx); ctx->have_slot = ctx->have_km = 0;
	OPENSSL_cleanse(&ctx->km, sizeof(ctx->km));
	errno = EINVAL; return s31_error("protected HMAC parameters");
}

static int protected_init(void *opaque, const unsigned char *key, size_t keylen,
			  const OSSL_PARAM params[])
{
	struct protected_mac *ctx = opaque;
	protected_reset(ctx);
	if (key || keylen || !protected_set(ctx, params) || (!ctx->have_slot && !ctx->have_km)) {
		ctx->have_slot = ctx->have_km = 0; OPENSSL_cleanse(&ctx->km, sizeof(ctx->km));
		errno = EINVAL; return s31_error("protected HMAC initialization");
	}
	if (!protected_open(ctx)) return 0;
	ctx->active = 1; return 1;
}

static int protected_update(void *opaque, const unsigned char *input, size_t length)
{
	struct protected_mac *ctx = opaque;
	unsigned char *bytes;
	size_t capacity;
	if (!ctx->active) return 0;
	if (length > ESP32S31_CRYPTO_HMAC_MAX_MESSAGE - ctx->length || (length && !input)) {
		protected_reset(ctx); errno = EMSGSIZE; return s31_error("protected HMAC input");
	}
	if (ctx->length + length > ctx->capacity) {
		capacity = ctx->capacity ? ctx->capacity * 2 : 1024;
		if (capacity < ctx->length + length) capacity = ctx->length + length;
		if (capacity > ESP32S31_CRYPTO_HMAC_MAX_MESSAGE) capacity = ESP32S31_CRYPTO_HMAC_MAX_MESSAGE;
		if (capacity < 1024) capacity = 1024;
		bytes = OPENSSL_malloc(capacity);
		if (!bytes) { protected_reset(ctx); return 0; }
		if (ctx->length) memcpy(bytes, ctx->input, ctx->length);
		OPENSSL_clear_free(ctx->input, ctx->capacity);
		ctx->input = bytes; ctx->capacity = capacity;
	}
	if (length) memcpy(ctx->input + ctx->length, input, length);
	ctx->length += length; return 1;
}

static int protected_final(void *opaque, unsigned char *output, size_t *length, size_t capacity)
{
	struct protected_mac *ctx = opaque;
	struct esp32s31_crypto_request job = {
		.operation = ESP32S31_CRYPTO_HMAC_SHA256,
		.input_len = ctx->length, .input = (uintptr_t)ctx->input,
		.output_len = 32, .output = (uintptr_t)output,
	};
	*length = 0;
	if (!output) { *length = 32; return 1; }
	if (!ctx->active || ctx->fd < 0 || capacity < 32) return 0;
	if (ioctl(ctx->fd, ESP32S31_CRYPTO_IOC_REQUEST, &job) || job.output_len != 32) {
		s31_error("protected HMAC final"); OPENSSL_cleanse(output, 32); protected_reset(ctx); return 0;
	}
	*length = 32; protected_reset(ctx); return 1;
}

static void *protected_dup(void *opaque)
{
	struct protected_mac *ctx = opaque, *copy = protected_new(NULL);
	if (!copy) return NULL;
	copy->slot = ctx->slot; copy->have_slot = ctx->have_slot;
	copy->have_km = ctx->have_km; copy->km = ctx->km;
	if (ctx->active) {
		if (!protected_open(copy)) goto fail;
		copy->active = 1;
		if (!protected_update(copy, ctx->input, ctx->length)) goto fail;
	}
	return copy;
fail:
	protected_free(copy); return NULL;
}

static int protected_get(void *opaque, OSSL_PARAM params[])
{
	OSSL_PARAM *p; (void)opaque;
	if ((p = OSSL_PARAM_locate(params, OSSL_MAC_PARAM_SIZE)) && !OSSL_PARAM_set_size_t(p, 32)) return 0;
	if ((p = OSSL_PARAM_locate(params, OSSL_MAC_PARAM_BLOCK_SIZE)) && !OSSL_PARAM_set_size_t(p, 64)) return 0;
	return 1;
}

static const OSSL_PARAM *protected_gettable(void *ctx, void *provider)
{
	static const OSSL_PARAM p[] = {
		OSSL_PARAM_size_t(OSSL_MAC_PARAM_SIZE, NULL),
		OSSL_PARAM_size_t(OSSL_MAC_PARAM_BLOCK_SIZE, NULL), OSSL_PARAM_END
	};
	(void)ctx; (void)provider; return p;
}

static const OSSL_PARAM *protected_settable(void *ctx, void *provider)
{
	static const OSSL_PARAM p[] = {
		OSSL_PARAM_uint("s31-slot", NULL),
		OSSL_PARAM_utf8_string("s31-km-file", NULL, 0),
		OSSL_PARAM_octet_string("s31-km-key", NULL, 0),
		OSSL_PARAM_utf8_string(OSSL_MAC_PARAM_DIGEST, NULL, 0),
		OSSL_PARAM_utf8_string(OSSL_MAC_PARAM_PROPERTIES, NULL, 0), OSSL_PARAM_END
	};
	(void)ctx; (void)provider; return p;
}

#define P(name, fn) { OSSL_FUNC_MAC_##name, (void (*)(void))fn }
const OSSL_DISPATCH s31_protected_mac[] = {
	P(NEWCTX, protected_new), P(FREECTX, protected_free), P(DUPCTX, protected_dup),
	P(INIT, protected_init), P(UPDATE, protected_update), P(FINAL, protected_final),
	P(SET_CTX_PARAMS, protected_set), P(SETTABLE_CTX_PARAMS, protected_settable),
	P(GET_CTX_PARAMS, protected_get), P(GETTABLE_CTX_PARAMS, protected_gettable), { 0, NULL }
};
