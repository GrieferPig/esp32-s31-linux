/* SPDX-License-Identifier: Apache-2.0 */
#include <fcntl.h>
#include <linux/if_alg.h>
#include "provider.h"

#define S31_AES_BATCH 65536
#define S31_AES_WORK 4096
struct s31_cipher {
	int parent, operation, mode, encrypt, key_set, iv_set, failed, final, have_stream;
	unsigned padding, num;
	int control_needed;
	size_t keylen, buffered;
	unsigned char key[32], iv[16], original_iv[16], buffer[16], stream[16];
};
static int cipher_error(struct s31_cipher *ctx, int error, const char *where)
{
	if (ctx->operation >= 0) close(ctx->operation);
	ctx->operation = -1;
	ctx->failed = 1; errno = error; return s31_error(where);
}
static void cipher_free(void *opaque)
{
	struct s31_cipher *ctx = opaque;
	if (ctx) { if (ctx->operation >= 0) close(ctx->operation); if (ctx->parent >= 0) close(ctx->parent); OPENSSL_clear_free(ctx, sizeof(*ctx)); }
}
static void *cipher_new(int mode, size_t keylen)
{
	struct s31_cipher *ctx = OPENSSL_zalloc(sizeof(*ctx));
	if (ctx) { ctx->parent = ctx->operation = -1; ctx->mode = mode; ctx->keylen = keylen; ctx->padding = 1; }
	return ctx;
}
static void *cipher_copy(void *opaque)
{
	struct s31_cipher *ctx = opaque, *copy = OPENSSL_memdup(ctx, sizeof(*ctx));
	if (!copy) return NULL;
	copy->parent = copy->operation = -1;
	/* The keyed parent is immutable. Copies accept their own operation
	 * socket and supply their own IV; an operation is never shared.
	 */
	if (ctx->parent >= 0 && (copy->parent = fcntl(ctx->parent, F_DUPFD_CLOEXEC, 0)) < 0) {
		s31_error("AES context copy"); cipher_free(copy); return NULL;
	}
	return copy;
}
static int cipher_bind(struct s31_cipher *ctx)
{
	struct sockaddr_alg addr = { .salg_family = AF_ALG, .salg_type = "skcipher" };
	const char *name = ctx->mode == EVP_CIPH_ECB_MODE ? "ecb-aes-esp32s31" :
		ctx->mode == EVP_CIPH_CBC_MODE ? "cbc-aes-esp32s31" : "ctr-aes-esp32s31";
	strcpy((char *)addr.salg_name, name);
	ctx->parent = socket(AF_ALG, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
	if (ctx->parent < 0 || bind(ctx->parent, (struct sockaddr *)&addr, sizeof(addr)) ||
	    setsockopt(ctx->parent, SOL_ALG, ALG_SET_KEY, ctx->key, ctx->keylen)) {
		int saved = errno;
		if (ctx->parent >= 0) close(ctx->parent);
		ctx->parent = -1; return cipher_error(ctx, saved, "AES key setup");
	}
	return 1;
}
static void counter_add(unsigned char counter[16], size_t blocks)
{
	for (int i = 15; i >= 0; i--) {
		blocks += counter[i]; counter[i] = blocks; blocks >>= 8;
	}
}
static int cipher_blocks(struct s31_cipher *ctx, unsigned char *output,
			 const unsigned char *input, size_t size)
{
	union {
		struct cmsghdr align;
		unsigned char data[CMSG_SPACE(sizeof(uint32_t)) + CMSG_SPACE(sizeof(struct af_alg_iv) + 16)];
	} control;
	struct iovec iov = { .iov_base = (void *)input, .iov_len = size };
	struct msghdr msg = { .msg_iov = &iov, .msg_iovlen = 1 };
	struct cmsghdr *cmsg; struct af_alg_iv *iv;
	unsigned char last[16]; ssize_t n; int saved;
	uint32_t encrypt = ctx->encrypt || ctx->mode == EVP_CIPH_CTR_MODE ? ALG_OP_ENCRYPT : ALG_OP_DECRYPT;
	if (!size || size > S31_AES_BATCH || size % 16) return cipher_error(ctx, EINVAL, "AES block length");
	if (ctx->mode == EVP_CIPH_CBC_MODE && !ctx->encrypt) memcpy(last, input + size - 16, 16);
	/* Completed requests advance the kernel IV exactly as ctx->iv. Only
	 * initialization or a fresh operation socket needs new control data.
	 */
	if (ctx->control_needed || ctx->operation < 0) {
		memset(&control, 0, sizeof(control));
		msg.msg_control = control.data;
		msg.msg_controllen = CMSG_SPACE(sizeof(encrypt));
		if (ctx->mode != EVP_CIPH_ECB_MODE) msg.msg_controllen += CMSG_SPACE(sizeof(struct af_alg_iv) + 16);
		cmsg = CMSG_FIRSTHDR(&msg); cmsg->cmsg_level = SOL_ALG; cmsg->cmsg_type = ALG_SET_OP;
		cmsg->cmsg_len = CMSG_LEN(sizeof(encrypt)); memcpy(CMSG_DATA(cmsg), &encrypt, sizeof(encrypt));
		if (ctx->mode != EVP_CIPH_ECB_MODE) {
			cmsg = CMSG_NXTHDR(&msg, cmsg); cmsg->cmsg_level = SOL_ALG; cmsg->cmsg_type = ALG_SET_IV;
			cmsg->cmsg_len = CMSG_LEN(sizeof(struct af_alg_iv) + 16);
			iv = (void *)CMSG_DATA(cmsg); iv->ivlen = 16; memcpy(iv->iv, ctx->iv, 16);
		}
	}
	if (ctx->operation < 0)
		ctx->operation = accept4(ctx->parent, NULL, NULL, SOCK_CLOEXEC);
	if (ctx->operation < 0) return cipher_error(ctx, errno, "AES request socket");
	/* Only exact successful send/receive pairs leave a reusable operation.
	 * EINTR or a short result may have consumed data; discard that socket.
	 */
	n = sendmsg(ctx->operation, &msg, MSG_NOSIGNAL);
	if (n != (ssize_t)size) goto fail;
	n = recv(ctx->operation, output, size, MSG_WAITALL);
	if (n != (ssize_t)size) goto fail;
	if (ctx->mode == EVP_CIPH_CBC_MODE) memcpy(ctx->iv, ctx->encrypt ? output + size - 16 : last, 16);
	else if (ctx->mode == EVP_CIPH_CTR_MODE) counter_add(ctx->iv, size / 16);
	ctx->control_needed = 0;
	OPENSSL_cleanse(last, sizeof(last)); return 1;
fail:
	saved = n < 0 ? errno : EIO; OPENSSL_cleanse(last, sizeof(last));
	OPENSSL_cleanse(output, size); return cipher_error(ctx, saved, "AES request");
}
static int cipher_set(void *opaque, const OSSL_PARAM params[])
{
	struct s31_cipher *ctx = opaque; const OSSL_PARAM *p; unsigned value; size_t size;
	if (!params) return 1;
	if ((p = OSSL_PARAM_locate_const(params, OSSL_CIPHER_PARAM_KEYLEN)) &&
	    (!OSSL_PARAM_get_size_t(p, &size) || size != ctx->keylen)) return 0;
	if ((p = OSSL_PARAM_locate_const(params, OSSL_CIPHER_PARAM_IVLEN)) &&
	    (!OSSL_PARAM_get_size_t(p, &size) || size != (ctx->mode == EVP_CIPH_ECB_MODE ? 0 : 16))) return 0;
	if ((p = OSSL_PARAM_locate_const(params, OSSL_CIPHER_PARAM_PADDING))) {
		if (!OSSL_PARAM_get_uint(p, &value)) return 0;
		ctx->padding = !!value;
	}
	if ((p = OSSL_PARAM_locate_const(params, OSSL_CIPHER_PARAM_NUM))) {
		if (!OSSL_PARAM_get_uint(p, &value) || value > 15 ||
		    (value && (ctx->mode != EVP_CIPH_CTR_MODE || !ctx->have_stream))) return 0;
		ctx->num = value;
	}
	/* Generic block padding is implemented here; the distinct TLS record
	 * contract is not silently accepted as ordinary CBC padding.
	 */
	if (OSSL_PARAM_locate_const(params, OSSL_CIPHER_PARAM_TLS_VERSION) ||
	    OSSL_PARAM_locate_const(params, OSSL_CIPHER_PARAM_TLS_MAC_SIZE)) return 0;
	return 1;
}
static int cipher_init(void *opaque, const unsigned char *key, size_t keylen,
		       const unsigned char *iv, size_t ivlen, const OSSL_PARAM params[], int encrypt)
{
	struct s31_cipher *ctx = opaque;
	if (ctx->failed && ctx->operation >= 0) {
		close(ctx->operation); ctx->operation = -1;
	}
	ctx->control_needed = 1;
	ctx->encrypt = encrypt; ctx->buffered = ctx->num = 0; ctx->have_stream = 0;
	ctx->failed = ctx->final = 0; OPENSSL_cleanse(ctx->buffer, sizeof(ctx->buffer));
	/* A repeated identical key keeps its immutable transform and completed
	 * operation. A changed key drops both before replacing key material.
	 */
	if (key && keylen == ctx->keylen && ctx->key_set &&
	    !CRYPTO_memcmp(key, ctx->key, keylen)) key = NULL;
	if (key) {
		if (ctx->operation >= 0) close(ctx->operation);
		ctx->operation = -1;
		if (ctx->parent >= 0) close(ctx->parent);
		ctx->parent = -1; ctx->key_set = 0; OPENSSL_cleanse(ctx->key, sizeof(ctx->key));
	}
	OPENSSL_cleanse(ctx->stream, sizeof(ctx->stream));
	if (iv && ctx->mode != EVP_CIPH_ECB_MODE) {
		if (ivlen != 16) return cipher_error(ctx, EINVAL, "AES IV length");
		memcpy(ctx->iv, iv, 16); memcpy(ctx->original_iv, iv, 16); ctx->iv_set = 1;
	} else if (!iv && ctx->iv_set && ctx->mode == EVP_CIPH_CBC_MODE) {
		memcpy(ctx->iv, ctx->original_iv, 16);
	}
	if (key) {
		if (keylen != ctx->keylen) return cipher_error(ctx, EINVAL, "AES key length");
		memcpy(ctx->key, key, keylen);
		if (!cipher_bind(ctx)) return 0;
		ctx->key_set = 1;
	}
	if (!cipher_set(ctx, params)) return cipher_error(ctx, EINVAL, "AES parameters");
	/* Staged EVP initialisation is valid before a key is supplied. */
	return 1;
}
static int encrypt_init(void *ctx, const unsigned char *key, size_t keylen,
			const unsigned char *iv, size_t ivlen, const OSSL_PARAM params[])
{ return cipher_init(ctx, key, keylen, iv, ivlen, params, 1); }
static int decrypt_init(void *ctx, const unsigned char *key, size_t keylen,
			const unsigned char *iv, size_t ivlen, const OSSL_PARAM params[])
{ return cipher_init(ctx, key, keylen, iv, ivlen, params, 0); }
static int ctr_update(struct s31_cipher *ctx, unsigned char *out, const unsigned char *in, size_t size)
{
	while (size && ctx->num) {
		*out++ = *in++ ^ ctx->stream[ctx->num]; ctx->num = (ctx->num + 1) % 16; size--;
	}
	while (size >= 16) {
		size_t chunk = size > S31_AES_BATCH ? S31_AES_BATCH : size & ~(size_t)15;
		if (!cipher_blocks(ctx, out, in, chunk)) return 0;
		out += chunk; in += chunk; size -= chunk;
	}
	if (size) {
		unsigned char zero[16] = { 0 };
		if (!cipher_blocks(ctx, ctx->stream, zero, 16)) return 0;
		ctx->have_stream = 1;
		for (size_t i = 0; i < size; i++) out[i] = in[i] ^ ctx->stream[i];
		ctx->num = size;
	}
	return 1;
}
static int cipher_update(void *opaque, unsigned char *out, size_t *outlen, size_t capacity,
			 const unsigned char *in, size_t length)
{
	struct s31_cipher *ctx = opaque; size_t expected, total, written = 0, work_used = 0;
	unsigned char work[S31_AES_WORK];
	*outlen = 0;
	if (!ctx->key_set || ctx->failed || ctx->final) return 0;
	if (ctx->mode == EVP_CIPH_CTR_MODE) {
		if (capacity < length) return 0;
		if (!ctr_update(ctx, out, in, length)) return 0;
		*outlen = length; return 1;
	}
	if (length > SIZE_MAX - ctx->buffered) return 0;
	total = length + ctx->buffered; expected = total & ~(size_t)15;
	if (!ctx->encrypt && ctx->padding && total && expected == total) expected -= 16;
	if (capacity < expected || (in == out && (ctx->buffered % 16))) return 0;
	while (length) {
		/* AF_ALG copies the whole submitted input before writing output,
		 * including exact in-place calls. Keep partial-block assembly small.
		 */
		if (!ctx->buffered) {
			size_t direct = length & ~(size_t)15;
			if (!ctx->encrypt && ctx->padding && direct == length && direct)
				direct -= 16;
			if (direct) {
				if (direct > S31_AES_BATCH) direct = S31_AES_BATCH;
				if (!cipher_blocks(ctx, out + written, in, direct)) {
					OPENSSL_cleanse(work, work_used); return 0;
				}
				written += direct; in += direct; length -= direct;
				continue;
			}
		}
		size_t take = sizeof(work) - ctx->buffered, emit;
		if (take > length) take = length;
		memcpy(work, ctx->buffer, ctx->buffered); memcpy(work + ctx->buffered, in, take);
		total = ctx->buffered + take;
		/* Direct requests never populate work. Clear every populated byte
		 * after partial-block assembly, including error paths.
		 */
		if (total > work_used) work_used = total;
		emit = total & ~(size_t)15;
		if (!ctx->encrypt && ctx->padding && emit == total) emit -= 16;
		if (emit && !cipher_blocks(ctx, out + written, work, emit)) {
			OPENSSL_cleanse(work, work_used); return 0;
		}
		ctx->buffered = total - emit; memcpy(ctx->buffer, work + emit, ctx->buffered);
		written += emit; in += take; length -= take;
	}
	OPENSSL_cleanse(work, work_used); *outlen = written; return 1;
}
static int cipher_final(void *opaque, unsigned char *out, size_t *outlen, size_t capacity)
{
	struct s31_cipher *ctx = opaque; unsigned char block[16]; unsigned pad, bad;
	int ret = 0; *outlen = 0;
	if (!ctx->key_set || ctx->failed || ctx->final) return 0;
	ctx->final = 1;
	if (ctx->mode == EVP_CIPH_CTR_MODE) return 1;
	if (!ctx->padding) {
		ret = ctx->buffered == 0;
		OPENSSL_cleanse(ctx->buffer, sizeof(ctx->buffer)); ctx->buffered = 0;
		if (!ret) ctx->failed = 1;
		return ret;
	}
	if (ctx->encrypt) {
		if (capacity < 16 || ctx->buffered >= 16) goto out;
		pad = 16 - ctx->buffered; memcpy(block, ctx->buffer, ctx->buffered);
		memset(block + ctx->buffered, pad, pad);
		ret = cipher_blocks(ctx, out, block, 16); if (ret) *outlen = 16;
	} else {
		if (ctx->buffered != 16 || !cipher_blocks(ctx, block, ctx->buffer, 16)) goto out;
		pad = block[15]; bad = !pad | (pad > 16);
		/* Visit all bytes regardless of the claimed padding length. */
		for (unsigned i = 1; i <= 16; i++) bad |= (block[16 - i] ^ pad) & (0U - (unsigned)(i <= pad));
		if (bad || capacity < 16 - pad) goto out;
		*outlen = 16 - pad; if (*outlen) memcpy(out, block, *outlen); ret = 1;
	}
out:
	OPENSSL_cleanse(block, sizeof(block)); OPENSSL_cleanse(ctx->buffer, sizeof(ctx->buffer)); ctx->buffered = 0;
	if (!ret) ctx->failed = 1;
	return ret;
}
static int cipher_raw(void *opaque, unsigned char *out, size_t *outlen, size_t capacity,
		      const unsigned char *in, size_t length)
{
	struct s31_cipher *ctx = opaque; *outlen = 0;
	if (!ctx->key_set || ctx->failed || ctx->final || ctx->buffered || capacity < length) return 0;
	if (ctx->mode == EVP_CIPH_CTR_MODE) {
		if (!ctr_update(ctx, out, in, length)) return 0;
	} else {
		if (length % 16) return 0;
		for (size_t done = 0; done < length;) {
			size_t chunk = length - done; if (chunk > S31_AES_BATCH) chunk = S31_AES_BATCH;
			if (!cipher_blocks(ctx, out + done, in + done, chunk)) return 0;
			done += chunk;
		}
	}
	*outlen = length; return 1;
}
static int cipher_get_ctx(void *opaque, OSSL_PARAM params[])
{
	struct s31_cipher *ctx = opaque; OSSL_PARAM *p; size_t ivlen = ctx->mode == EVP_CIPH_ECB_MODE ? 0 : 16;
	if ((p = OSSL_PARAM_locate(params, OSSL_CIPHER_PARAM_KEYLEN)) && !OSSL_PARAM_set_size_t(p, ctx->keylen)) return 0;
	if ((p = OSSL_PARAM_locate(params, OSSL_CIPHER_PARAM_IVLEN)) && !OSSL_PARAM_set_size_t(p, ivlen)) return 0;
	if ((p = OSSL_PARAM_locate(params, OSSL_CIPHER_PARAM_PADDING)) && !OSSL_PARAM_set_uint(p, ctx->padding)) return 0;
	if ((p = OSSL_PARAM_locate(params, OSSL_CIPHER_PARAM_NUM)) && !OSSL_PARAM_set_uint(p, ctx->num)) return 0;
	if ((p = OSSL_PARAM_locate(params, OSSL_CIPHER_PARAM_IV)) && !OSSL_PARAM_set_octet_string(p, ctx->original_iv, ivlen)) return 0;
	if ((p = OSSL_PARAM_locate(params, OSSL_CIPHER_PARAM_UPDATED_IV)) && !OSSL_PARAM_set_octet_string(p, ctx->iv, ivlen)) return 0;
	return 1;
}
static const OSSL_PARAM *cipher_gettable_ctx(void *ctx, void *provider)
{
	static const OSSL_PARAM p[] = { OSSL_PARAM_size_t(OSSL_CIPHER_PARAM_KEYLEN,NULL), OSSL_PARAM_size_t(OSSL_CIPHER_PARAM_IVLEN,NULL), OSSL_PARAM_uint(OSSL_CIPHER_PARAM_PADDING,NULL), OSSL_PARAM_uint(OSSL_CIPHER_PARAM_NUM,NULL), OSSL_PARAM_octet_string(OSSL_CIPHER_PARAM_IV,NULL,0), OSSL_PARAM_octet_string(OSSL_CIPHER_PARAM_UPDATED_IV,NULL,0), OSSL_PARAM_END };
	(void)ctx; (void)provider; return p;
}
static const OSSL_PARAM *cipher_settable_ctx(void *ctx, void *provider)
{
	static const OSSL_PARAM p[] = { OSSL_PARAM_size_t(OSSL_CIPHER_PARAM_KEYLEN,NULL), OSSL_PARAM_size_t(OSSL_CIPHER_PARAM_IVLEN,NULL), OSSL_PARAM_uint(OSSL_CIPHER_PARAM_PADDING,NULL), OSSL_PARAM_uint(OSSL_CIPHER_PARAM_NUM,NULL), OSSL_PARAM_END };
	(void)ctx; (void)provider; return p;
}
static int cipher_get(OSSL_PARAM params[], int mode, size_t keylen)
{
	OSSL_PARAM *p;
	if ((p = OSSL_PARAM_locate(params, OSSL_CIPHER_PARAM_MODE)) && !OSSL_PARAM_set_uint(p, mode)) return 0;
	if ((p = OSSL_PARAM_locate(params, OSSL_CIPHER_PARAM_KEYLEN)) && !OSSL_PARAM_set_size_t(p, keylen)) return 0;
	if ((p = OSSL_PARAM_locate(params, OSSL_CIPHER_PARAM_IVLEN)) && !OSSL_PARAM_set_size_t(p, mode == EVP_CIPH_ECB_MODE ? 0 : 16)) return 0;
	if ((p = OSSL_PARAM_locate(params, OSSL_CIPHER_PARAM_BLOCK_SIZE)) && !OSSL_PARAM_set_size_t(p, mode == EVP_CIPH_CTR_MODE ? 1 : 16)) return 0;
	if ((p = OSSL_PARAM_locate(params, OSSL_CIPHER_PARAM_AEAD)) && !OSSL_PARAM_set_int(p, 0)) return 0;
	if ((p = OSSL_PARAM_locate(params, OSSL_CIPHER_PARAM_CUSTOM_IV)) && !OSSL_PARAM_set_int(p, 0)) return 0;
	return 1;
}
static const OSSL_PARAM *cipher_gettable(void *provider)
{
	static const OSSL_PARAM p[] = { OSSL_PARAM_uint(OSSL_CIPHER_PARAM_MODE,NULL), OSSL_PARAM_size_t(OSSL_CIPHER_PARAM_KEYLEN,NULL), OSSL_PARAM_size_t(OSSL_CIPHER_PARAM_IVLEN,NULL), OSSL_PARAM_size_t(OSSL_CIPHER_PARAM_BLOCK_SIZE,NULL), OSSL_PARAM_int(OSSL_CIPHER_PARAM_AEAD,NULL), OSSL_PARAM_int(OSSL_CIPHER_PARAM_CUSTOM_IV,NULL), OSSL_PARAM_END };
	(void)provider; return p;
}
#define E(name,fn) { OSSL_FUNC_##name, (void (*)(void))fn }
#define AES(bits,mode,value) \
static void *new_##bits##_##mode(void *p) { (void)p; return cipher_new(value,bits/8); } \
static int get_##bits##_##mode(OSSL_PARAM p[]) { return cipher_get(p,value,bits/8); } \
static const OSSL_DISPATCH dispatch_##bits##_##mode[] = { \
 E(CIPHER_NEWCTX,new_##bits##_##mode), E(CIPHER_FREECTX,cipher_free), E(CIPHER_DUPCTX,cipher_copy), \
 E(CIPHER_ENCRYPT_INIT,encrypt_init), E(CIPHER_DECRYPT_INIT,decrypt_init), E(CIPHER_UPDATE,cipher_update), \
 E(CIPHER_FINAL,cipher_final), E(CIPHER_CIPHER,cipher_raw), E(CIPHER_GET_PARAMS,get_##bits##_##mode), \
 E(CIPHER_GETTABLE_PARAMS,cipher_gettable), E(CIPHER_GET_CTX_PARAMS,cipher_get_ctx), \
 E(CIPHER_GETTABLE_CTX_PARAMS,cipher_gettable_ctx), E(CIPHER_SET_CTX_PARAMS,cipher_set), \
 E(CIPHER_SETTABLE_CTX_PARAMS,cipher_settable_ctx), {0,NULL} };
AES(128,ecb,EVP_CIPH_ECB_MODE) AES(256,ecb,EVP_CIPH_ECB_MODE)
AES(128,cbc,EVP_CIPH_CBC_MODE) AES(256,cbc,EVP_CIPH_CBC_MODE)
AES(128,ctr,EVP_CIPH_CTR_MODE) AES(256,ctr,EVP_CIPH_CTR_MODE)
const OSSL_ALGORITHM s31_ciphers[] = {
 {"AES-128-ECB:2.16.840.1.101.3.4.1.1","provider=s31",dispatch_128_ecb,"S31 AES128 ECB"},
 {"AES-256-ECB:2.16.840.1.101.3.4.1.41","provider=s31",dispatch_256_ecb,"S31 AES256 ECB"},
 {"AES-128-CBC:AES128:2.16.840.1.101.3.4.1.2","provider=s31",dispatch_128_cbc,"S31 AES128 CBC"},
 {"AES-256-CBC:AES256:2.16.840.1.101.3.4.1.42","provider=s31",dispatch_256_cbc,"S31 AES256 CBC"},
 {"AES-128-CTR","provider=s31",dispatch_128_ctr,"S31 AES128 CTR"},
 {"AES-256-CTR","provider=s31",dispatch_256_ctr,"S31 AES256 CTR"}, {NULL,NULL,NULL,NULL}
};
