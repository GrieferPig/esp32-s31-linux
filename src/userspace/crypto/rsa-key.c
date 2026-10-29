/* SPDX-License-Identifier: Apache-2.0 */
#include "rsa.h"
#include <fcntl.h>
#include <sys/ioctl.h>
#include <linux/esp32s31-crypto.h>

#define S31_RSA_BYTES 512
static int rsa_raw(struct s31_rsa_key *key, unsigned operation,
		   const unsigned char *input, size_t inputlen, unsigned char *output)
{
	unsigned char result[S31_RSA_BYTES]; size_t bytes = RSA_size(key->math), prefix;
	struct esp32s31_crypto_request request = { .operation = operation,
		.input_len = inputlen, .input = (uintptr_t)input,
		.output_len = sizeof(result), .output = (uintptr_t)result };
	int ret = 0;
	if (inputlen > bytes || bytes > sizeof(result)) goto out;
	if (ioctl(key->fd, ESP32S31_CRYPTO_IOC_REQUEST, &request)) {
		/* The native public primitive rejects representatives >= n. */
		if (operation == ESP32S31_CRYPTO_RSA_PUBLIC && errno == EINVAL)
			ERR_raise(ERR_LIB_RSA, RSA_R_DATA_TOO_LARGE_FOR_MODULUS);
		else
			s31_error("RSA exponentiation");
		goto out;
	}
	/* Native MPI rounds its width to a whole 32-bit word. OpenSSL uses
	 * the actual modulus width. Only zero extension may be removed.
	 */
	if (request.output_len < bytes || request.output_len > sizeof(result)) {
		errno = EIO; s31_error("RSA result length"); goto out;
	}
	prefix = request.output_len - bytes;
	for (size_t i = 0; i < prefix; i++) if (result[i]) {
		errno = EIO; s31_error("RSA result extension"); goto out;
	}
	memcpy(output, result + prefix, bytes); ret = 1;
out:
	OPENSSL_cleanse(result, sizeof(result)); return ret;
}
static int rsa_private_modexp(BIGNUM *out, const BIGNUM *input, RSA *math, BN_CTX *ctx)
{
	struct s31_rsa_key *key = RSA_get_app_data(math);
	unsigned char in[S31_RSA_BYTES], result[S31_RSA_BYTES], check[S31_RSA_BYTES];
	int bytes = RSA_size(math), ret = 0; (void)ctx;
	if (!key || !key->private || bytes > S31_RSA_BYTES || BN_bn2binpad(input, in, bytes) != bytes) goto out;
	if (!rsa_raw(key, key->dsdata ? ESP32S31_CRYPTO_DS_PRIVATE : ESP32S31_CRYPTO_RSA_PRIVATE, in, bytes, result) ||
	    !rsa_raw(key, ESP32S31_CRYPTO_RSA_PUBLIC, result, bytes, check)) goto out;
	/* Preserve the private-operation result check as well as OpenSSL's
	 * surrounding blinding. A faulty private result must not be released.
	 */
	if (CRYPTO_memcmp(in, check, bytes)) { errno = EIO; s31_error("RSA private result check"); goto out; }
	ret = BN_bin2bn(result, bytes, out) != NULL;
out:
	OPENSSL_cleanse(in, sizeof(in)); OPENSSL_cleanse(result, sizeof(result)); OPENSSL_cleanse(check, sizeof(check)); return ret;
}
static int rsa_public_encrypt(int inputlen, const unsigned char *input, unsigned char *output, RSA *math, int padding)
{
	unsigned char encoded[S31_RSA_BYTES]; int bytes = RSA_size(math), ret = -1, ok = 0;
	struct s31_rsa_key *key = RSA_get_app_data(math);
	if (!key || bytes > S31_RSA_BYTES || inputlen < 0 || inputlen > bytes) goto out;
	switch (padding) {
	case RSA_PKCS1_PADDING: ok = RSA_padding_add_PKCS1_type_2(encoded, bytes, input, inputlen); break;
	case RSA_PKCS1_OAEP_PADDING: ok = RSA_padding_add_PKCS1_OAEP(encoded, bytes, input, inputlen, NULL, 0); break;
	case RSA_NO_PADDING: ok = RSA_padding_add_none(encoded, bytes, input, inputlen); break;
	default: break;
	}
	if (ok && rsa_raw(key, ESP32S31_CRYPTO_RSA_PUBLIC, encoded, bytes, output)) ret = bytes;
out:
	OPENSSL_cleanse(encoded, sizeof(encoded)); return ret;
}
static int rsa_public_decrypt(int inputlen, const unsigned char *input, unsigned char *output, RSA *math, int padding)
{
	unsigned char encoded[S31_RSA_BYTES]; int bytes = RSA_size(math), ret = -1;
	struct s31_rsa_key *key = RSA_get_app_data(math);
	if (!key || bytes > S31_RSA_BYTES || inputlen < 1 || inputlen > bytes ||
	    !rsa_raw(key, ESP32S31_CRYPTO_RSA_PUBLIC, input, inputlen, encoded)) goto out;
	if (padding == RSA_NO_PADDING) { memcpy(output, encoded, bytes); ret = bytes; }
	else if (padding == RSA_PKCS1_PADDING) ret = RSA_padding_check_PKCS1_type_1(output, bytes, encoded, bytes, bytes);
out:
	OPENSSL_cleanse(encoded, sizeof(encoded)); return ret;
}
int s31_rsa_method_init(struct s31_provider *provider)
{
	RSA_METHOD *method = RSA_meth_dup(RSA_PKCS1_OpenSSL());
	if (!method) return 0;
	if (!RSA_meth_set1_name(method, "S31 native MPI") ||
	    !RSA_meth_set_mod_exp(method, rsa_private_modexp) ||
	    !RSA_meth_set_pub_enc(method, rsa_public_encrypt) ||
	    !RSA_meth_set_pub_dec(method, rsa_public_decrypt)) { RSA_meth_free(method); return 0; }
	provider->rsa_method = method; return 1;
}
void s31_rsa_method_free(struct s31_provider *provider) { RSA_meth_free(provider->rsa_method); }
static void *rsa_new(void *provider)
{
	struct s31_rsa_key *key = OPENSSL_zalloc(sizeof(*key));
	if (key) { key->provider = provider; key->fd = -1; atomic_init(&key->references, 1); }
	return key;
}
struct s31_rsa_key *s31_rsa_ref(struct s31_rsa_key *key)
{
	if (key) atomic_fetch_add_explicit(&key->references, 1, memory_order_relaxed);
	return key;
}
void s31_rsa_free(void *opaque)
{
	struct s31_rsa_key *key = opaque;
	if (!key || atomic_fetch_sub_explicit(&key->references, 1, memory_order_acq_rel) != 1) return;
	if (key->fd >= 0) close(key->fd);
	RSA_free(key->math); EVP_PKEY_free(key->parameters);
	OPENSSL_clear_free(key->dsdata, key->dslen); OPENSSL_clear_free(key, sizeof(*key));
}
static BIGNUM *component(const OSSL_PARAM params[], const char *name, int required)
{
	const OSSL_PARAM *p = OSSL_PARAM_locate_const(params, name); BIGNUM *bn = NULL;
	if (p) { if (!OSSL_PARAM_get_BN(p, &bn)) return NULL; }
	else if (!required) bn = BN_new();
	return bn;
}

static RSA *ds_public(const void *data, size_t length)
{
	struct esp32s31_crypto_ds_key header;
	const unsigned char *cursor;
	RSA *public = NULL;
	size_t extra;
	if (!data || length <= sizeof(header) || length > 8192) return NULL;
	memcpy(&header, data, sizeof(header));
	extra = header.reserved == ESP32S31_CRYPTO_DS_KEY_MANAGER ?
		sizeof(struct esp32s31_crypto_km_key) : 0;
	if ((header.reserved && !extra) || length <= sizeof(header) + extra ||
	    (extra ? header.slot != ESP32S31_CRYPTO_SLOT_UNUSED :
		     header.slot >= ESP32S31_CRYPTO_EFUSE_SLOTS) ||
	    header.public_key_len != length - sizeof(header) - extra ||
	    (header.rsa_bits != 1024 && header.rsa_bits != 2048 &&
	     header.rsa_bits != 3072 && header.rsa_bits != 4096)) goto out;
	cursor = (const unsigned char *)data + sizeof(header) + extra;
	public = d2i_RSAPublicKey(NULL, &cursor, header.public_key_len);
	if (public && (cursor != (const unsigned char *)data + length ||
		       RSA_bits(public) != (int)header.rsa_bits)) {
		RSA_free(public); public = NULL;
	}
out:
	OPENSSL_cleanse(&header, sizeof(header));
	return public;
}

static int rsa_ds_import(struct s31_rsa_key *key, int selection, const OSSL_PARAM params[])
{
	const OSSL_PARAM *wrapped = OSSL_PARAM_locate_const(params, "s31-ds-key");
	const BIGNUM *n, *e; BIGNUM *supplied_n = NULL, *supplied_e = NULL;
	EVP_PKEY_CTX *ctx = NULL; OSSL_PARAM_BLD *build = NULL; OSSL_PARAM *public = NULL;
	struct esp32s31_crypto_import request = { .algorithm = ESP32S31_CRYPTO_DS_RSA,
		.flags = ESP32S31_CRYPTO_KEY_PRIVATE };
	int ret = 0;
	const char *private_names[] = { OSSL_PKEY_PARAM_RSA_D, OSSL_PKEY_PARAM_RSA_FACTOR1,
		OSSL_PKEY_PARAM_RSA_FACTOR2, OSSL_PKEY_PARAM_RSA_EXPONENT1,
		OSSL_PKEY_PARAM_RSA_EXPONENT2, OSSL_PKEY_PARAM_RSA_COEFFICIENT1 };
	if (!(selection & OSSL_KEYMGMT_SELECT_PRIVATE_KEY) || !wrapped ||
	    wrapped->data_type != OSSL_PARAM_OCTET_STRING) return 0;
	for (unsigned i = 0; i < sizeof(private_names)/sizeof(private_names[0]); i++)
		if (OSSL_PARAM_locate_const(params, private_names[i])) return 0;
	key->math = ds_public(wrapped->data, wrapped->data_size);
	if (!key->math) goto out;
	RSA_get0_key(key->math, &n, &e, NULL);
	supplied_n = component(params, OSSL_PKEY_PARAM_RSA_N, 1);
	supplied_e = component(params, OSSL_PKEY_PARAM_RSA_E, 1);
	if (!supplied_n || !supplied_e || BN_cmp(n, supplied_n) || BN_cmp(e, supplied_e)) goto out;
	build = OSSL_PARAM_BLD_new();
	ctx = EVP_PKEY_CTX_new_from_name(key->provider->libctx, "RSA", "provider=default");
	if (!build || !ctx || !OSSL_PARAM_BLD_push_BN(build, OSSL_PKEY_PARAM_RSA_N, n) ||
	    !OSSL_PARAM_BLD_push_BN(build, OSSL_PKEY_PARAM_RSA_E, e) ||
	    !(public = OSSL_PARAM_BLD_to_param(build)) || EVP_PKEY_fromdata_init(ctx) <= 0 ||
	    EVP_PKEY_fromdata(ctx, &key->parameters, EVP_PKEY_PUBLIC_KEY, public) <= 0) goto out;
	key->dsdata = OPENSSL_memdup(wrapped->data, wrapped->data_size);
	if (!key->dsdata) goto out;
	key->dslen = wrapped->data_size;
	request.key_len = key->dslen; request.key = (uintptr_t)key->dsdata;
	key->fd = open("/dev/s31-crypto", O_RDWR | O_CLOEXEC);
	if (key->fd < 0 || ioctl(key->fd, ESP32S31_CRYPTO_IOC_IMPORT, &request)) {
		s31_error("DS key import"); goto out;
	}
	if (!RSA_set_method(key->math, key->provider->rsa_method) || !RSA_set_app_data(key->math, key)) goto out;
	/* The private exponent never exists in this process. Stock signing still
	 * performs blinding with n/e around our private modular-exponent callback.
	 * Decryption is separately rejected: implicit rejection needs the real d.
	 */
	RSA_set_flags(key->math, RSA_FLAG_EXT_PKEY);
	RSA_clear_flags(key->math, RSA_FLAG_NO_BLINDING);
	key->private = 1; ret = 1;
out:
	EVP_PKEY_CTX_free(ctx); OSSL_PARAM_free(public); OSSL_PARAM_BLD_free(build);
	BN_free(supplied_n); BN_free(supplied_e);
	if (!ret) {
		if (key->fd >= 0) close(key->fd);
		key->fd = -1; key->private = 0;
		RSA_free(key->math); key->math = NULL; EVP_PKEY_free(key->parameters); key->parameters = NULL;
		OPENSSL_clear_free(key->dsdata, key->dslen); key->dsdata = NULL; key->dslen = 0;
	}
	return ret;
}

struct s31_rsa_key *s31_rsa_ds_load(struct s31_provider *provider, const void *data, size_t length)
{
	struct s31_rsa_key *key = NULL;
	RSA *public = ds_public(data, length); const BIGNUM *n, *e;
	OSSL_PARAM_BLD *build = NULL; OSSL_PARAM *params = NULL;
	if (!public) { errno = EINVAL; s31_error("DS bundle"); return NULL; }
	RSA_get0_key(public, &n, &e, NULL); build = OSSL_PARAM_BLD_new();
	if (!build || !OSSL_PARAM_BLD_push_BN(build, OSSL_PKEY_PARAM_RSA_N, n) ||
	    !OSSL_PARAM_BLD_push_BN(build, OSSL_PKEY_PARAM_RSA_E, e) ||
	    !OSSL_PARAM_BLD_push_octet_string(build, "s31-ds-key", data, length) ||
	    !(params = OSSL_PARAM_BLD_to_param(build))) goto out;
	key = rsa_new(provider);
	if (key && !rsa_ds_import(key, EVP_PKEY_KEYPAIR, params)) { s31_rsa_free(key); key = NULL; }
out:
	if (params) for (OSSL_PARAM *p = params; p->key; p++) if (p->data) OPENSSL_cleanse(p->data, p->data_size);
	OSSL_PARAM_free(params); OSSL_PARAM_BLD_free(build); RSA_free(public); return key;
}

static int rsa_import(void *opaque, int selection, const OSSL_PARAM params[])
{
	struct s31_rsa_key *key = opaque; EVP_PKEY_CTX *ctx = NULL; unsigned char *der = NULL;
	BIGNUM *bn[8] = { 0 }; int ret = 0, length;
	const char *names[] = { OSSL_PKEY_PARAM_RSA_N, OSSL_PKEY_PARAM_RSA_E, OSSL_PKEY_PARAM_RSA_D,
		OSSL_PKEY_PARAM_RSA_FACTOR1, OSSL_PKEY_PARAM_RSA_FACTOR2, OSSL_PKEY_PARAM_RSA_EXPONENT1,
		OSSL_PKEY_PARAM_RSA_EXPONENT2, OSSL_PKEY_PARAM_RSA_COEFFICIENT1 };
	struct esp32s31_crypto_import request = { .algorithm = ESP32S31_CRYPTO_RSA };
	if (key->parameters) return 0;
	if (OSSL_PARAM_locate_const(params, "s31-ds-key")) return rsa_ds_import(key, selection, params);
	key->private = !!((selection & OSSL_KEYMGMT_SELECT_PRIVATE_KEY) && OSSL_PARAM_locate_const(params, OSSL_PKEY_PARAM_RSA_D));
	ctx = EVP_PKEY_CTX_new_from_name(key->provider->libctx, "RSA", "provider=default");
	if (!ctx || EVP_PKEY_fromdata_init(ctx) <= 0 || EVP_PKEY_fromdata(ctx, &key->parameters, selection, (OSSL_PARAM *)params) <= 0) goto out;
	for (unsigned i = 0; i < (key->private ? 8U : 2U); i++) {
		bn[i] = component(params, names[i], i < 3);
		if (!bn[i]) goto out;
	}
	key->math = RSA_new();
	if (!key->math || !RSA_set0_key(key->math, bn[0], bn[1], key->private ? bn[2] : NULL)) goto out;
	bn[0] = bn[1] = bn[2] = NULL;
	if (key->private) {
		if (!RSA_set0_factors(key->math, bn[3], bn[4])) goto out;
		bn[3] = bn[4] = NULL;
		if (!RSA_set0_crt_params(key->math, bn[5], bn[6], bn[7])) goto out;
		bn[5] = bn[6] = bn[7] = NULL;
	}
	if (RSA_size(key->math) > S31_RSA_BYTES) { errno = EOPNOTSUPP; s31_error("RSA modulus size"); goto out; }
	length = key->private ? i2d_RSAPrivateKey(key->math, &der) : i2d_RSAPublicKey(key->math, &der);
	if (length <= 0) goto out;
	request.flags = key->private ? ESP32S31_CRYPTO_KEY_PRIVATE : 0;
	request.key_len = length; request.key = (uintptr_t)der;
	key->fd = open("/dev/s31-crypto", O_RDWR | O_CLOEXEC);
	if (key->fd < 0 || ioctl(key->fd, ESP32S31_CRYPTO_IOC_IMPORT, &request)) { s31_error("RSA key import"); goto out; }
	/* These are internal math objects, never exported as a caller's RSA*.
	 * Retain actual d and CRT fields so stock private decrypt keeps its
	 * implicit rejection. Even n/e/d-only keys use the native callback:
	 * missing CRT components are present as zero BNs, not an EXT_PKEY flag.
	 */
	if (!RSA_set_method(key->math, key->provider->rsa_method) || !RSA_set_app_data(key->math, key)) goto out;
	RSA_clear_flags(key->math, RSA_FLAG_EXT_PKEY | RSA_FLAG_NO_BLINDING);
	ret = 1;
out:
	if (der) OPENSSL_clear_free(der, request.key_len);
	for (unsigned i = 0; i < 8; i++) BN_clear_free(bn[i]);
	EVP_PKEY_CTX_free(ctx);
	if (!ret) {
		if (key->fd >= 0) close(key->fd);
		key->fd = -1; key->private = 0; RSA_free(key->math); key->math = NULL;
		EVP_PKEY_free(key->parameters); key->parameters = NULL;
	}
	return ret;
}
static void clear_params(OSSL_PARAM *params)
{
	if (params) for (OSSL_PARAM *p = params; p->key; p++) if (p->data) OPENSSL_cleanse(p->data, p->data_size);
	OSSL_PARAM_free(params);
}
static int rsa_export(void *opaque, int selection, OSSL_CALLBACK *callback, void *arg)
{
	struct s31_rsa_key *key = opaque; OSSL_PARAM *params = NULL; int ret;
	if (!key->parameters || (key->dsdata && (selection & OSSL_KEYMGMT_SELECT_PRIVATE_KEY)) ||
	    !EVP_PKEY_todata(key->parameters, selection, &params)) return 0;
	ret = callback(params, arg); clear_params(params); return ret;
}
static void *rsa_copy(const void *opaque, int selection)
{
	const struct s31_rsa_key *source = opaque; struct s31_rsa_key *copy = rsa_new(source->provider);
	OSSL_PARAM *params = NULL;
	if (source->dsdata && (selection & OSSL_KEYMGMT_SELECT_PRIVATE_KEY)) {
		s31_rsa_free(copy); return s31_rsa_ds_load(source->provider, source->dsdata, source->dslen);
	}
	if (!copy || !source->parameters) return copy;
	if (!EVP_PKEY_todata(source->parameters, selection, &params) || !rsa_import(copy, selection, params)) { s31_rsa_free(copy); copy = NULL; }
	clear_params(params); return copy;
}
static const OSSL_PARAM *rsa_types(int selection)
{
	static const OSSL_PARAM p[] = {
		OSSL_PARAM_BN(OSSL_PKEY_PARAM_RSA_N,NULL,0), OSSL_PARAM_BN(OSSL_PKEY_PARAM_RSA_E,NULL,0),
		OSSL_PARAM_BN(OSSL_PKEY_PARAM_RSA_D,NULL,0), OSSL_PARAM_BN(OSSL_PKEY_PARAM_RSA_FACTOR1,NULL,0),
		OSSL_PARAM_BN(OSSL_PKEY_PARAM_RSA_FACTOR2,NULL,0), OSSL_PARAM_BN(OSSL_PKEY_PARAM_RSA_EXPONENT1,NULL,0),
		OSSL_PARAM_BN(OSSL_PKEY_PARAM_RSA_EXPONENT2,NULL,0), OSSL_PARAM_BN(OSSL_PKEY_PARAM_RSA_COEFFICIENT1,NULL,0),
		OSSL_PARAM_octet_string("s31-ds-key",NULL,0), OSSL_PARAM_END
	}; (void)selection; return p;
}
static int rsa_get(void *opaque, OSSL_PARAM params[])
{
	struct s31_rsa_key *key = opaque; return key->parameters && EVP_PKEY_get_params(key->parameters, params);
}
static const OSSL_PARAM *rsa_gettable(void *provider)
{
	static const OSSL_PARAM p[] = {
		OSSL_PARAM_int(OSSL_PKEY_PARAM_BITS,NULL), OSSL_PARAM_int(OSSL_PKEY_PARAM_SECURITY_BITS,NULL),
		OSSL_PARAM_int(OSSL_PKEY_PARAM_MAX_SIZE,NULL), OSSL_PARAM_utf8_string(OSSL_PKEY_PARAM_DEFAULT_DIGEST,NULL,0),
		OSSL_PARAM_BN(OSSL_PKEY_PARAM_RSA_N,NULL,0), OSSL_PARAM_BN(OSSL_PKEY_PARAM_RSA_E,NULL,0),
		OSSL_PARAM_BN(OSSL_PKEY_PARAM_RSA_D,NULL,0), OSSL_PARAM_BN(OSSL_PKEY_PARAM_RSA_FACTOR1,NULL,0),
		OSSL_PARAM_BN(OSSL_PKEY_PARAM_RSA_FACTOR2,NULL,0), OSSL_PARAM_BN(OSSL_PKEY_PARAM_RSA_EXPONENT1,NULL,0),
		OSSL_PARAM_BN(OSSL_PKEY_PARAM_RSA_EXPONENT2,NULL,0), OSSL_PARAM_BN(OSSL_PKEY_PARAM_RSA_COEFFICIENT1,NULL,0), OSSL_PARAM_END
	}; (void)provider; return p;
}
static int rsa_has(const void *opaque, int selection)
{
	const struct s31_rsa_key *key = opaque;
	return key->parameters && (!(selection & OSSL_KEYMGMT_SELECT_PRIVATE_KEY) || key->private);
}
static int rsa_validate(const void *opaque, int selection, int checktype)
{
	const struct s31_rsa_key *key = opaque; EVP_PKEY_CTX *ctx; int ret;
	if (!rsa_has(key, selection)) return 0;
	ctx = EVP_PKEY_CTX_new_from_pkey(key->provider->libctx, key->parameters, "provider=default");
	if (!ctx) return 0;
	ret = EVP_PKEY_public_check(ctx) > 0;
	if (ret && key->dsdata) {
		if ((selection & OSSL_KEYMGMT_SELECT_PRIVATE_KEY) && checktype == OSSL_KEYMGMT_VALIDATE_FULL_CHECK) {
			unsigned char input[512] = {0}, output[512]; int size = RSA_size(key->math);
			input[size - 1] = 2;
			ret = RSA_private_encrypt(size, input, output, key->math, RSA_NO_PADDING) == size;
			OPENSSL_cleanse(input, sizeof(input)); OPENSSL_cleanse(output, sizeof(output));
		}
		EVP_PKEY_CTX_free(ctx); return ret;
	}
	if (ret && (selection & OSSL_KEYMGMT_SELECT_PRIVATE_KEY)) ret = EVP_PKEY_private_check(ctx) > 0;
	if (ret && (selection & OSSL_KEYMGMT_SELECT_KEYPAIR) == OSSL_KEYMGMT_SELECT_KEYPAIR && checktype == OSSL_KEYMGMT_VALIDATE_FULL_CHECK) ret = EVP_PKEY_pairwise_check(ctx) > 0;
	EVP_PKEY_CTX_free(ctx); return ret;
}
static int rsa_match(const void *one, const void *two, int selection)
{
	const struct s31_rsa_key *a = one, *b = two; BIGNUM *x = NULL, *y = NULL; int ret = 1;
	if (!a->parameters || !b->parameters) return 0;
	if (selection & OSSL_KEYMGMT_SELECT_PRIVATE_KEY) {
		if (a->dsdata || b->dsdata) ret = a->dsdata && b->dsdata && a->dslen == b->dslen &&
			!CRYPTO_memcmp(a->dsdata, b->dsdata, a->dslen);
		else ret = a->private && b->private &&
			EVP_PKEY_get_bn_param(a->parameters,OSSL_PKEY_PARAM_RSA_D,&x) &&
			EVP_PKEY_get_bn_param(b->parameters,OSSL_PKEY_PARAM_RSA_D,&y) && BN_cmp(x,y) == 0;
	}
	BN_clear_free(x); BN_clear_free(y);
	return ret && (!(selection & OSSL_KEYMGMT_SELECT_PUBLIC_KEY) || EVP_PKEY_eq(a->parameters,b->parameters) == 1);
}
static const char *rsa_operation(int operation)
{ return operation == OSSL_OP_SIGNATURE || operation == OSSL_OP_ASYM_CIPHER ? "RSA" : NULL; }
struct rsa_gen { struct s31_provider *provider; unsigned bits; BIGNUM *exponent; };
static int rsa_gen_set(void *opaque, const OSSL_PARAM params[])
{
	struct rsa_gen *ctx = opaque; const OSSL_PARAM *p; unsigned bits;
	if (!params) return 1;
	if ((p = OSSL_PARAM_locate_const(params, OSSL_PKEY_PARAM_RSA_BITS))) {
		if (!OSSL_PARAM_get_uint(p,&bits) || bits < 512 || bits > 4096) return 0;
		ctx->bits = bits;
	}
	if ((p = OSSL_PARAM_locate_const(params, OSSL_PKEY_PARAM_RSA_E))) {
		BIGNUM *n = NULL; if (!OSSL_PARAM_get_BN(p,&n)) return 0;
		BN_free(ctx->exponent); ctx->exponent = n;
	}
	if ((p = OSSL_PARAM_locate_const(params, OSSL_PKEY_PARAM_RSA_PRIMES))) {
		if (!OSSL_PARAM_get_uint(p,&bits) || bits != 2) return 0;
	}
	return 1;
}
static void rsa_gen_free(void *opaque)
{ struct rsa_gen *ctx=opaque; if(ctx){BN_free(ctx->exponent);OPENSSL_free(ctx);} }
static void *rsa_gen_init(void *provider, int selection, const OSSL_PARAM params[])
{
	struct rsa_gen *ctx=OPENSSL_zalloc(sizeof(*ctx));
	if(!ctx || !(selection & OSSL_KEYMGMT_SELECT_KEYPAIR)){OPENSSL_free(ctx);return NULL;}
	ctx->provider=provider;ctx->bits=2048;
	if(!rsa_gen_set(ctx,params)){rsa_gen_free(ctx);return NULL;}return ctx;
}
static void *rsa_gen_run(void *opaque, OSSL_CALLBACK *callback, void *arg)
{
	struct rsa_gen *gen=opaque;EVP_PKEY_CTX *ctx=NULL;EVP_PKEY *temporary=NULL;OSSL_PARAM_BLD *build=NULL;OSSL_PARAM *params=NULL,*exported=NULL;struct s31_rsa_key *key=NULL;
	(void)callback;(void)arg;
	build=OSSL_PARAM_BLD_new();ctx=EVP_PKEY_CTX_new_from_name(gen->provider->libctx,"RSA","provider=default");
	if(!build || !ctx || !OSSL_PARAM_BLD_push_uint(build,OSSL_PKEY_PARAM_RSA_BITS,gen->bits) ||
	   (gen->exponent && !OSSL_PARAM_BLD_push_BN(build,OSSL_PKEY_PARAM_RSA_E,gen->exponent)) ||
	   !(params=OSSL_PARAM_BLD_to_param(build)) || EVP_PKEY_keygen_init(ctx)<=0 || EVP_PKEY_CTX_set_params(ctx,params)<=0 ||
	   EVP_PKEY_generate(ctx,&temporary)<=0 || !EVP_PKEY_todata(temporary,EVP_PKEY_KEYPAIR,&exported))goto out;
	key=rsa_new(gen->provider);if(key && !rsa_import(key,EVP_PKEY_KEYPAIR,exported)){s31_rsa_free(key);key=NULL;}
out:
	clear_params(exported);OSSL_PARAM_free(params);OSSL_PARAM_BLD_free(build);EVP_PKEY_free(temporary);EVP_PKEY_CTX_free(ctx);return key;
}
static const OSSL_PARAM *rsa_gen_settable(void *gen, void *provider)
{
	static const OSSL_PARAM p[]={OSSL_PARAM_uint(OSSL_PKEY_PARAM_RSA_BITS,NULL),OSSL_PARAM_BN(OSSL_PKEY_PARAM_RSA_E,NULL,0),OSSL_PARAM_uint(OSSL_PKEY_PARAM_RSA_PRIMES,NULL),OSSL_PARAM_END};(void)gen;(void)provider;return p;
}
#define E(name,fn) { OSSL_FUNC_##name,(void(*)(void))fn }
static void *rsa_load(const void *reference, size_t length)
{
	struct s31_rsa_key *key;
	if (length != sizeof(key)) return NULL;
	memcpy(&key, reference, sizeof(key)); return s31_rsa_ref(key);
}
const OSSL_DISPATCH s31_rsa_keymgmt[]={
 E(KEYMGMT_NEW,rsa_new),E(KEYMGMT_FREE,s31_rsa_free),E(KEYMGMT_LOAD,rsa_load),E(KEYMGMT_DUP,rsa_copy),E(KEYMGMT_IMPORT,rsa_import),E(KEYMGMT_IMPORT_TYPES,rsa_types),
 E(KEYMGMT_EXPORT,rsa_export),E(KEYMGMT_EXPORT_TYPES,rsa_types),E(KEYMGMT_GET_PARAMS,rsa_get),E(KEYMGMT_GETTABLE_PARAMS,rsa_gettable),
 E(KEYMGMT_HAS,rsa_has),E(KEYMGMT_VALIDATE,rsa_validate),E(KEYMGMT_MATCH,rsa_match),E(KEYMGMT_QUERY_OPERATION_NAME,rsa_operation),
 E(KEYMGMT_GEN_INIT,rsa_gen_init),E(KEYMGMT_GEN_SET_PARAMS,rsa_gen_set),E(KEYMGMT_GEN_SETTABLE_PARAMS,rsa_gen_settable),E(KEYMGMT_GEN,rsa_gen_run),E(KEYMGMT_GEN_CLEANUP,rsa_gen_free),{0,NULL}};
