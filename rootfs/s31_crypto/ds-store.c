/* SPDX-License-Identifier: Apache-2.0 */
#include "rsa.h"
#include <openssl/core_object.h>
#include <openssl/store.h>
#include <fcntl.h>
#include <sys/stat.h>

struct ds_store {
	struct s31_provider *provider;
	struct s31_rsa_key *key;
	unsigned char *data;
	size_t length;
	int done;
};

static int ds_close(void *opaque)
{
	struct ds_store *ctx = opaque;
	if (ctx) { s31_rsa_free(ctx->key); OPENSSL_clear_free(ctx->data, ctx->length); OPENSSL_free(ctx); }
	return 1;
}

static void *ds_open(void *provider, const char *uri)
{
	struct ds_store *ctx = NULL; struct stat st;
	int fd = -1, saved; unsigned char extra; ssize_t n;
	if (strncmp(uri, "s31-ds:", 7) || !uri[7]) { errno = EINVAL; goto fail; }
	fd = open(uri + 7, O_RDONLY | O_CLOEXEC | O_NONBLOCK);
	if (fd < 0 || fstat(fd, &st)) goto fail;
	if (!S_ISREG(st.st_mode) || st.st_size <= 0 || st.st_size > 8192) { errno = EINVAL; goto fail; }
	ctx = OPENSSL_zalloc(sizeof(*ctx));
	if (!ctx || !(ctx->data = OPENSSL_malloc(st.st_size))) { errno = ENOMEM; goto fail; }
	ctx->provider = provider;
	/* Bounded regular files only; no device, FIFO or network-backed URI. */
	while (ctx->length < (size_t)st.st_size) {
		n = read(fd, ctx->data + ctx->length, st.st_size - ctx->length);
		if (n < 0 && errno == EINTR) continue;
		if (n <= 0) { if (!n) errno = EIO; goto fail; }
		ctx->length += n;
	}
	do n = read(fd, &extra, 1); while (n < 0 && errno == EINTR);
	if (n != 0) { if (n > 0) errno = EFBIG; goto fail; }
	close(fd); return ctx;
fail:
	saved = errno; if (fd >= 0) close(fd); ds_close(ctx); errno = saved;
	s31_error("DS store open"); return NULL;
}

static int ds_set(void *opaque, const OSSL_PARAM params[])
{
	const OSSL_PARAM *p; int expected; (void)opaque;
	if (!params) return 1;
	/* Standard STORE discovery can supply properties and optional input
	 * structure hints. A DS bundle has exactly one fixed binary structure.
	 */
	for (p = params; p->key; p++) {
		if (!strcmp(p->key, OSSL_STORE_PARAM_EXPECT)) {
			if (!OSSL_PARAM_get_int(p, &expected) ||
			    (expected && expected != OSSL_STORE_INFO_PKEY && expected != OSSL_STORE_INFO_PUBKEY)) return 0;
		} else if (!strcmp(p->key, OSSL_STORE_PARAM_PROPERTIES)) {
			const char *properties;
			if (!OSSL_PARAM_get_utf8_string_ptr(p, &properties)) return 0;
		} else if (!strcmp(p->key, OSSL_STORE_PARAM_INPUT_TYPE)) {
			const char *type;
			if (!OSSL_PARAM_get_utf8_string_ptr(p, &type) || (*type && strcmp(type, "DER"))) return 0;
		} else return 0;
	}
	return 1;
}

static const OSSL_PARAM *ds_settable(void *provider)
{
	static const OSSL_PARAM p[] = {
		OSSL_PARAM_int(OSSL_STORE_PARAM_EXPECT, NULL),
		OSSL_PARAM_utf8_string(OSSL_STORE_PARAM_PROPERTIES, NULL, 0),
		OSSL_PARAM_utf8_string(OSSL_STORE_PARAM_INPUT_TYPE, NULL, 0), OSSL_PARAM_END
	};
	(void)provider; return p;
}

static int ds_load(void *opaque, OSSL_CALLBACK *object, void *arg,
		   OSSL_PASSPHRASE_CALLBACK *password, void *password_arg)
{
	struct ds_store *ctx = opaque; int type = OSSL_OBJECT_PKEY;
	OSSL_PARAM params[] = {
		OSSL_PARAM_int(OSSL_OBJECT_PARAM_TYPE, &type),
		OSSL_PARAM_utf8_string(OSSL_OBJECT_PARAM_DATA_TYPE, "RSA", 0),
		OSSL_PARAM_octet_string(OSSL_OBJECT_PARAM_REFERENCE, &ctx->key, sizeof(ctx->key)), OSSL_PARAM_END
	};
	(void)password; (void)password_arg;
	if (ctx->done) return 0;
	ctx->done = 1;
	ctx->key = s31_rsa_ds_load(ctx->provider, ctx->data, ctx->length);
	OPENSSL_clear_free(ctx->data, ctx->length); ctx->data = NULL; ctx->length = 0;
	return ctx->key && object(params, arg);
}

static int ds_eof(void *opaque) { return ((struct ds_store *)opaque)->done; }
#define S(name, fn) { OSSL_FUNC_STORE_##name, (void (*)(void))fn }
const OSSL_DISPATCH s31_ds_store[] = {
	S(OPEN, ds_open), S(SETTABLE_CTX_PARAMS, ds_settable), S(SET_CTX_PARAMS, ds_set),
	S(LOAD, ds_load), S(EOF, ds_eof), S(CLOSE, ds_close), { 0, NULL }
};
