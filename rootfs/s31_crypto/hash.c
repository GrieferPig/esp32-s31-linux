/* SPDX-License-Identifier: Apache-2.0 */
#include <fcntl.h>
#include <limits.h>
#include <linux/if_alg.h>
#include <sys/uio.h>
#include <strings.h>
#include <string.h>
#include "provider.h"

const struct s31_hash_kind s31_hash_kinds[] = {
	{ "SHA1", "sha1-esp32s31", 20, 64 },
	{ "SHA224", "sha224-esp32s31", 28, 64 },
	{ "SHA256", "sha256-esp32s31", 32, 64 },
	{ "SHA384", "sha384-esp32s31", 48, 128 },
	{ "SHA512", "sha512-esp32s31", 64, 128 },
	{ "SHA512-224", "sha512-224-esp32s31", 28, 128 },
	{ "SHA512-256", "sha512-256-esp32s31", 32, 128 }, { NULL, NULL, 0, 0 }
};
const struct s31_hash_kind *s31_hash_lookup(const char *name)
{
	/* OpenSSL accepts the usual SHA-2 aliases; no property fetch or generic
	 * kernel name can redirect a claimed operation to a software driver.
	 */
	char canonical[32]; size_t n = 0;
	for (; *name && n < sizeof(canonical) - 1; name++) if (*name != '-' && *name != '/') canonical[n++] = *name;
	canonical[n] = 0;
	for (unsigned i = 0; s31_hash_kinds[i].name; i++) {
		char candidate[32]; size_t k = 0;
		for (const char *s = s31_hash_kinds[i].name; *s; s++) if (*s != '-') candidate[k++] = *s;
		candidate[k] = 0;
		if (!strcasecmp(canonical, candidate)) return &s31_hash_kinds[i];
	}
	if (!strncasecmp(canonical, "SHA2", 4)) {
		/* SHA2-256 canonicalizes to SHA2256, not SHA256. */
		char alias[32]; snprintf(alias, sizeof(alias), "SHA%s", canonical + 4);
		return s31_hash_lookup(alias);
	}
	return NULL;
}
void s31_hash_close(struct s31_hash *ctx)
{
	if (ctx->operation >= 0) close(ctx->operation);
	if (ctx->parent >= 0) close(ctx->parent);
	ctx->operation = ctx->parent = -1;
	ctx->completed = 0;
	OPENSSL_cleanse(ctx->pending, sizeof(ctx->pending));
	ctx->pending_len = 0;
}
int s31_hash_open(struct s31_hash *ctx, const struct s31_hash_kind *kind,
		  const void *key, size_t keylen, int keyed)
{
	struct sockaddr_alg addr = { .salg_family = AF_ALG, .salg_type = "hash" };
	/* A fully consumed result leaves AF_ALG ready for another message.
	 * Reuse only a successful unkeyed operation of the same algorithm.
	 * Partial/failed streams and keyed reinitialization get fresh sockets.
	 */
	if (!keyed && !ctx->keyed && kind == ctx->kind && !ctx->failed &&
	    ctx->completed && ctx->operation >= 0) {
		ctx->completed = 0;
		return 1;
	}
	s31_hash_close(ctx); ctx->kind = kind; ctx->failed = 1;
	ctx->keyed = keyed;
	if (!kind || keylen > 65536) { errno = EINVAL; return 0; }
	if (keyed) {
		/* The truncated SHA512 variants do not implement the generic HMAC
		 * state contract. Only advertise the five supported templates.
		 */
		if (kind >= &s31_hash_kinds[5]) { errno = EOPNOTSUPP; return 0; }
		if (snprintf((char *)addr.salg_name, sizeof(addr.salg_name), "hmac(%s)", kind->driver) >= (int)sizeof(addr.salg_name)) return 0;
	} else {
		if (strlen(kind->driver) >= sizeof(addr.salg_name)) return 0;
		strcpy((char *)addr.salg_name, kind->driver);
	}
	ctx->parent = socket(AF_ALG, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
	if (ctx->parent < 0 || bind(ctx->parent, (struct sockaddr *)&addr, sizeof(addr)) ||
	    (keyed && setsockopt(ctx->parent, SOL_ALG, ALG_SET_KEY, key, keylen))) goto fail;
	ctx->operation = accept4(ctx->parent, NULL, NULL, SOCK_CLOEXEC);
	if (ctx->operation < 0) goto fail;
	ctx->failed = 0; return 1;
fail:
	s31_error("hash initialization"); s31_hash_close(ctx); return 0;
}
int s31_hash_update(void *opaque, const unsigned char *input, size_t length)
{
	struct s31_hash *ctx = opaque;
	if (ctx->failed || ctx->completed || ctx->operation < 0) return 0;
	/* Keep short unkeyed messages together so AF_ALG can submit one digest
	 * request at final instead of separate update and final requests.  Larger
	 * streams retain the existing bounded streaming path.
	 */
	if (!ctx->keyed && length <= sizeof(ctx->pending) - ctx->pending_len) {
		if (length) memcpy(ctx->pending + ctx->pending_len, input, length);
		ctx->pending_len += length;
		return 1;
	}
	if (ctx->pending_len) {
		size_t take = length > 65536 - ctx->pending_len ?
			      65536 - ctx->pending_len : length;
		size_t total = ctx->pending_len + take;
		struct iovec iov[] = {
			{ .iov_base = ctx->pending, .iov_len = ctx->pending_len },
			{ .iov_base = (void *)input, .iov_len = take },
		};
		struct msghdr msg = { .msg_iov = iov, .msg_iovlen = 2 };
		ssize_t n = sendmsg(ctx->operation, &msg, MSG_MORE | MSG_NOSIGNAL);
		if (n != (ssize_t)total) {
			if (n >= 0) errno = EIO;
			ctx->failed = 1;
			return s31_error("hash pending update");
		}
		OPENSSL_cleanse(ctx->pending, ctx->pending_len);
		ctx->pending_len = 0;
		input += take;
		length -= take;
	}
	while (length) {
		/* A larger AF_ALG send batches admission and worker completion. The
		 * kernel still bounds its scatterlist and the driver DMA staging buffer;
		 * A failed or short send may have discarded the stream, including
		 * on EINTR. Never retry against an unknown hash state.
		 */
		size_t chunk = length > 65536 ? 65536 : length; ssize_t n;
		n = send(ctx->operation, input, chunk, MSG_MORE | MSG_NOSIGNAL);
		if (n != (ssize_t)chunk) {
			if (n >= 0) errno = EIO;
			ctx->failed = 1; return s31_error("hash update");
		}
		input += n; length -= n;
	}
	return 1;
}
int s31_hash_final(void *opaque, unsigned char *output, size_t *length, size_t capacity)
{
	struct s31_hash *ctx = opaque; ssize_t n;
	*length = 0;
	if (ctx->failed || ctx->completed || ctx->operation < 0 || !ctx->kind || capacity < ctx->kind->size) return 0;
	if (ctx->pending_len) {
		n = send(ctx->operation, ctx->pending, ctx->pending_len, MSG_NOSIGNAL);
		if (n != (ssize_t)ctx->pending_len) {
			if (n >= 0) errno = EIO;
			ctx->failed = 1;
			OPENSSL_cleanse(ctx->pending, ctx->pending_len);
			ctx->pending_len = 0;
			return s31_error("hash pending final");
		}
		OPENSSL_cleanse(ctx->pending, ctx->pending_len);
		ctx->pending_len = 0;
	}
	/* A failed receive may have consumed the result and reset the stream. */
	n = recv(ctx->operation, output, ctx->kind->size, MSG_WAITALL);
	if (n != (ssize_t)ctx->kind->size) { ctx->failed = 1; OPENSSL_cleanse(output, ctx->kind->size); return s31_error("hash final"); }
	ctx->completed = 1; *length = n; return 1;
}
int s31_hash_copy(struct s31_hash *to, const struct s31_hash *from)
{
	*to = *from; to->parent = to->operation = -1;
	if (from->parent < 0) return 1;
	to->parent = fcntl(from->parent, F_DUPFD_CLOEXEC, 0);
	if (to->parent < 0) return s31_error("hash parent copy");
	if (from->operation >= 0) {
		/* AF_ALG hash accept clones the operation through the kernel's
		 * export/import contract, including a partial input block.
		 */
		to->operation = accept4(from->operation, NULL, NULL, SOCK_CLOEXEC);
		if (to->operation < 0) { s31_hash_close(to); return s31_error("hash state copy"); }
	}
	return 1;
}
static void hash_free(void *opaque)
{
	struct s31_hash *ctx = opaque;
	if (ctx) { s31_hash_close(ctx); OPENSSL_clear_free(ctx, sizeof(*ctx)); }
}
static void *hash_new(unsigned type)
{
	struct s31_hash *ctx = OPENSSL_zalloc(sizeof(*ctx));
	if (ctx) { ctx->parent = ctx->operation = -1; ctx->kind = &s31_hash_kinds[type]; }
	return ctx;
}
static int hash_init(void *opaque, const OSSL_PARAM params[])
{
	struct s31_hash *ctx = opaque; (void)params;
	return s31_hash_open(ctx, ctx->kind, NULL, 0, 0);
}
static void *hash_dup(void *opaque)
{
	struct s31_hash *copy = OPENSSL_zalloc(sizeof(*copy));
	if (copy && !s31_hash_copy(copy, opaque)) { OPENSSL_clear_free(copy, sizeof(*copy)); return NULL; }
	return copy;
}
static const OSSL_PARAM *hash_gettable(void *provider)
{
	static const OSSL_PARAM p[] = {
		OSSL_PARAM_size_t(OSSL_DIGEST_PARAM_SIZE, NULL),
		OSSL_PARAM_size_t(OSSL_DIGEST_PARAM_BLOCK_SIZE, NULL),
		OSSL_PARAM_int(OSSL_DIGEST_PARAM_XOF, NULL),
		OSSL_PARAM_int(OSSL_DIGEST_PARAM_ALGID_ABSENT, NULL), OSSL_PARAM_END
	}; (void)provider; return p;
}
static int hash_get(OSSL_PARAM params[], unsigned kind)
{
	OSSL_PARAM *p;
	if ((p = OSSL_PARAM_locate(params, OSSL_DIGEST_PARAM_SIZE)) && !OSSL_PARAM_set_size_t(p, s31_hash_kinds[kind].size)) return 0;
	if ((p = OSSL_PARAM_locate(params, OSSL_DIGEST_PARAM_BLOCK_SIZE)) && !OSSL_PARAM_set_size_t(p, s31_hash_kinds[kind].block)) return 0;
	if ((p = OSSL_PARAM_locate(params, OSSL_DIGEST_PARAM_XOF)) && !OSSL_PARAM_set_int(p, 0)) return 0;
	if ((p = OSSL_PARAM_locate(params, OSSL_DIGEST_PARAM_ALGID_ABSENT)) && !OSSL_PARAM_set_int(p, 1)) return 0;
	return 1;
}
#define E(name,fn) { OSSL_FUNC_##name, (void (*)(void))fn }
#define HASH(number) \
static void *new_##number(void *p) { (void)p; return hash_new(number); } \
static int get_##number(OSSL_PARAM p[]) { return hash_get(p, number); } \
static const OSSL_DISPATCH digest_##number[] = { \
 E(DIGEST_NEWCTX,new_##number), E(DIGEST_FREECTX,hash_free), E(DIGEST_DUPCTX,hash_dup), \
 E(DIGEST_INIT,hash_init), E(DIGEST_UPDATE,s31_hash_update), E(DIGEST_FINAL,s31_hash_final), \
 E(DIGEST_GET_PARAMS,get_##number), E(DIGEST_GETTABLE_PARAMS,hash_gettable), {0,NULL} };
HASH(0) HASH(1) HASH(2) HASH(3) HASH(4) HASH(5) HASH(6)
const OSSL_ALGORITHM s31_digests[] = {
	{ "SHA1:SHA-1:SSL3-SHA1:1.3.14.3.2.26", "provider=s31", digest_0, "S31 SHA1" },
	{ "SHA2-224:SHA-224:SHA224:2.16.840.1.101.3.4.2.4", "provider=s31", digest_1, "S31 SHA224" },
	{ "SHA2-256:SHA-256:SHA256:2.16.840.1.101.3.4.2.1", "provider=s31", digest_2, "S31 SHA256" },
	{ "SHA2-384:SHA-384:SHA384:2.16.840.1.101.3.4.2.2", "provider=s31", digest_3, "S31 SHA384" },
	{ "SHA2-512:SHA-512:SHA512:2.16.840.1.101.3.4.2.3", "provider=s31", digest_4, "S31 SHA512" },
	{ "SHA2-512/224:SHA-512/224:SHA512-224:2.16.840.1.101.3.4.2.5", "provider=s31", digest_5, "S31 SHA512/224" },
	{ "SHA2-512/256:SHA-512/256:SHA512-256:2.16.840.1.101.3.4.2.6", "provider=s31", digest_6, "S31 SHA512/256" }, {NULL,NULL,NULL,NULL}
};
struct s31_mac { struct s31_hash hash; unsigned char *key; size_t keylen; int have_key; };
static void *mac_new(void *provider)
{
	struct s31_mac *ctx = OPENSSL_zalloc(sizeof(*ctx)); (void)provider;
	if (ctx) { ctx->hash.parent = ctx->hash.operation = -1; ctx->hash.kind = &s31_hash_kinds[2]; }
	return ctx;
}
static void mac_free(void *opaque)
{
	struct s31_mac *ctx = opaque;
	if (ctx) { s31_hash_close(&ctx->hash); OPENSSL_clear_free(ctx->key, ctx->keylen); OPENSSL_clear_free(ctx, sizeof(*ctx)); }
}
static int mac_key(struct s31_mac *ctx, const unsigned char *key, size_t size)
{
	unsigned char *copy;
	if (size > 65536) return 0;
	copy = size ? OPENSSL_memdup(key, size) : NULL;
	if (size && !copy) return 0;
	OPENSSL_clear_free(ctx->key, ctx->keylen); ctx->key = copy; ctx->keylen = size; ctx->have_key = 1; return 1;
}
static int mac_set(void *opaque, const OSSL_PARAM params[])
{
	struct s31_mac *ctx = opaque; const OSSL_PARAM *p; const char *name;
	const struct s31_hash_kind *kind;
	if (!params) return 1;
	if ((p = OSSL_PARAM_locate_const(params, OSSL_MAC_PARAM_PROPERTIES))) {
		if (!OSSL_PARAM_get_utf8_string_ptr(p, &name) ||
		    (*name && strcmp(name, "provider=s31"))) return 0;
	}
	/* The generic TLS record HMAC contract needs additional constant-time
	 * record handling. Do not silently accept that security parameter.
	 */
	if (OSSL_PARAM_locate_const(params, OSSL_MAC_PARAM_TLS_DATA_SIZE)) return 0;
	if (OSSL_PARAM_locate_const(params, OSSL_MAC_PARAM_DIGEST) ||
	    OSSL_PARAM_locate_const(params, OSSL_MAC_PARAM_KEY)) {
		s31_hash_close(&ctx->hash); ctx->hash.failed = 1;
	}
	if ((p = OSSL_PARAM_locate_const(params, OSSL_MAC_PARAM_DIGEST))) {
		if (!OSSL_PARAM_get_utf8_string_ptr(p, &name) || !(kind = s31_hash_lookup(name)) || kind >= &s31_hash_kinds[5]) return 0;
		ctx->hash.kind = kind;
	}
	if ((p = OSSL_PARAM_locate_const(params, OSSL_MAC_PARAM_KEY)) &&
	    (p->data_type != OSSL_PARAM_OCTET_STRING || !mac_key(ctx, p->data, p->data_size))) return 0;
	return 1;
}
static int mac_init(void *opaque, const unsigned char *key, size_t keylen, const OSSL_PARAM params[])
{
	struct s31_mac *ctx = opaque;
	if (!mac_set(ctx, params) || (key && !mac_key(ctx, key, keylen)) || !ctx->have_key) return 0;
	return s31_hash_open(&ctx->hash, ctx->hash.kind, ctx->key, ctx->keylen, 1);
}
static void *mac_dup(void *opaque)
{
	struct s31_mac *ctx = opaque, *copy = mac_new(NULL);
	if (!copy) return NULL;
	if (!s31_hash_copy(&copy->hash, &ctx->hash) ||
	    (ctx->have_key && !mac_key(copy, ctx->key, ctx->keylen))) { mac_free(copy); return NULL; }
	return copy;
}
static int mac_get(void *opaque, OSSL_PARAM params[])
{
	struct s31_mac *ctx = opaque; OSSL_PARAM *p;
	if ((p = OSSL_PARAM_locate(params, OSSL_MAC_PARAM_SIZE)) && !OSSL_PARAM_set_size_t(p, ctx->hash.kind->size)) return 0;
	if ((p = OSSL_PARAM_locate(params, OSSL_MAC_PARAM_BLOCK_SIZE)) && !OSSL_PARAM_set_size_t(p, ctx->hash.kind->block)) return 0;
	return 1;
}
static const OSSL_PARAM *mac_gettable(void *ctx, void *provider)
{
	static const OSSL_PARAM p[] = { OSSL_PARAM_size_t(OSSL_MAC_PARAM_SIZE,NULL), OSSL_PARAM_size_t(OSSL_MAC_PARAM_BLOCK_SIZE,NULL), OSSL_PARAM_END };
	(void)ctx; (void)provider; return p;
}
static const OSSL_PARAM *mac_settable(void *ctx, void *provider)
{
	static const OSSL_PARAM p[] = { OSSL_PARAM_utf8_string(OSSL_MAC_PARAM_DIGEST,NULL,0), OSSL_PARAM_octet_string(OSSL_MAC_PARAM_KEY,NULL,0), OSSL_PARAM_utf8_string(OSSL_MAC_PARAM_PROPERTIES,NULL,0), OSSL_PARAM_END };
	(void)ctx; (void)provider; return p;
}
static const OSSL_DISPATCH mac_dispatch[] = {
	E(MAC_NEWCTX,mac_new), E(MAC_FREECTX,mac_free), E(MAC_DUPCTX,mac_dup), E(MAC_INIT,mac_init),
	/* hash is the first field, so these callbacks share the streaming code. */
	E(MAC_UPDATE,s31_hash_update), E(MAC_FINAL,s31_hash_final), E(MAC_GET_CTX_PARAMS,mac_get),
	E(MAC_GETTABLE_CTX_PARAMS,mac_gettable), E(MAC_SET_CTX_PARAMS,mac_set), E(MAC_SETTABLE_CTX_PARAMS,mac_settable), {0,NULL}
};
const OSSL_ALGORITHM s31_macs[] = {
	{"HMAC", "provider=s31", mac_dispatch, "Native HMAC using S31 SHA"},
	{"S31-HMAC", "provider=s31", s31_protected_mac, "S31 eFuse-key HMAC-SHA256"},
	{NULL,NULL,NULL,NULL}
};
