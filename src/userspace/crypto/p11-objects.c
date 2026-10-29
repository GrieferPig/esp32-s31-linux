/* SPDX-License-Identifier: Apache-2.0 */
#include "p11.h"
#include <openssl/pem.h>
#include <openssl/bn.h>
#include <openssl/ec.h>
#include <openssl/objects.h>
#include <linux/esp32s31-crypto.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include "key-file.h"

static struct p11_object *object_new(void)
{
	struct p11_object *o = OPENSSL_zalloc(sizeof(*o));
	if (o) { atomic_init(&o->references, 1); o->slot = -1; }
	return o;
}

struct p11_object *p11_object_ref(struct p11_object *o)
{
	if (o) atomic_fetch_add_explicit(&o->references, 1, memory_order_relaxed);
	return o;
}

void p11_object_free(struct p11_object *o)
{
	if (!o || atomic_fetch_sub_explicit(&o->references, 1, memory_order_acq_rel) != 1) return;
	OPENSSL_clear_free(o->km_recovery, sizeof(struct esp32s31_crypto_km_key));
	EVP_PKEY_free(o->key); OPENSSL_clear_free(o->secret, o->secret_size); OPENSSL_clear_free(o, sizeof(*o));
}

struct p11_object *p11_object_find(CK_OBJECT_HANDLE handle)
{
	for (struct p11_object *o = p11.objects; o; o = o->next)
		if (o->handle == handle && !o->deleted && (!o->private || p11.logged_in)) return o;
	return NULL;
}

CK_RV p11_key_get(CK_OBJECT_HANDLE handle, struct p11_object **object)
{
	pthread_mutex_lock(&p11.lock); *object = p11_object_ref(p11_object_find(handle));
	pthread_mutex_unlock(&p11.lock); return *object ? CKR_OK : CKR_KEY_HANDLE_INVALID;
}

CK_RV p11_private_check(struct p11_object *o)
{
	CK_RV rv;
	pthread_mutex_lock(&p11.lock);
	rv = o->deleted ? CKR_KEY_HANDLE_INVALID : o->private && !p11.logged_in ? CKR_USER_NOT_LOGGED_IN : CKR_OK;
	pthread_mutex_unlock(&p11.lock); return rv;
}

static CK_RV object_add(struct p11_object *o)
{
	size_t count = 0;
	for (struct p11_object *q = p11.objects; q; q = q->next) count++;
	if (count >= P11_OBJECTS || !(o->handle = p11_handle())) return CKR_DEVICE_MEMORY;
	o->next = p11.objects; p11.objects = o; return CKR_OK;
}

void p11_objects_clear(CK_SESSION_HANDLE owner)
{
	struct p11_object **link = &p11.objects, *o;
	while ((o = *link)) {
		if (owner && o->owner != owner) { link = &o->next; continue; }
		*link = o->next; o->deleted = 1; p11_object_free(o);
	}
}

static void params_free(OSSL_PARAM *params)
{
	if (params) for (OSSL_PARAM *p = params; p->key; p++) if (p->data) OPENSSL_cleanse(p->data, p->data_size);
	OSSL_PARAM_free(params);
}

static EVP_PKEY *native_key(EVP_PKEY *source, int selection)
{
	OSSL_PARAM *params = NULL; EVP_PKEY *out = NULL;
	EVP_PKEY_CTX *ctx = EVP_PKEY_CTX_new_from_name(p11.libctx,
		EVP_PKEY_is_a(source, "EC") ? "EC" : "RSA", "provider=s31");
	if (ctx && EVP_PKEY_todata(source, selection, &params) && EVP_PKEY_fromdata_init(ctx) > 0)
		EVP_PKEY_fromdata(ctx, &out, selection, params);
	params_free(params); EVP_PKEY_CTX_free(ctx); return out;
}

static int no_password(char *buffer, int size, int writing, void *arg)
{
	(void)buffer; (void)size; (void)writing; (void)arg; return 0;
}

static EVP_PKEY *file_key(const char *path, int *private, int *opaque)
{
	EVP_PKEY *source = NULL, *key = NULL; BIGNUM *d = NULL;
	OSSL_STORE_CTX *store = NULL; OSSL_STORE_INFO *item = NULL;
	BIO *bio = NULL; struct stat st; int fd = -1;
	*private = *opaque = 0;
	if (!strncmp(path, "s31-ds:", 7) || !strncmp(path, "s31-ecdsa:", 10)) {
		store = OSSL_STORE_open_ex(path, p11.libctx, "provider=s31", NULL, NULL, NULL, NULL, NULL);
		if (store) item = OSSL_STORE_load(store);
		if (item && OSSL_STORE_INFO_get_type(item) == OSSL_STORE_INFO_PKEY) key = OSSL_STORE_INFO_get1_PKEY(item);
		OSSL_STORE_INFO_free(item); if (store) OSSL_STORE_close(store);
		if (key) *private = *opaque = 1;
		return key;
	}
	fd = open(path, O_RDONLY | O_CLOEXEC | O_NONBLOCK);
	if (fd < 0 || fstat(fd, &st) || !S_ISREG(st.st_mode) || st.st_size <= 0 || st.st_size > 16384) goto out;
	bio = BIO_new_fd(fd, BIO_CLOSE);
	if (!bio) goto out;
	fd = -1;
	source = PEM_read_bio_PrivateKey_ex(bio, NULL, no_password, NULL, p11.libctx, "provider=default");
	if (source) *private = 1;
	else {
		ERR_clear_error();
		if (BIO_seek(bio, 0) >= 0) source = PEM_read_bio_PUBKEY_ex(bio, NULL, no_password, NULL, p11.libctx, "provider=default");
	}
	if (source && EVP_PKEY_is_a(source, "RSA") && !EVP_PKEY_is_a(source, "RSA-PSS") &&
	    EVP_PKEY_get_bits(source) >= 1024 && EVP_PKEY_get_bits(source) <= 4096) {
		if (*private && (!EVP_PKEY_get_bn_param(source, OSSL_PKEY_PARAM_RSA_D, &d) || !d)) goto out;
		key = native_key(source, *private ? EVP_PKEY_KEYPAIR : EVP_PKEY_PUBLIC_KEY);
	} else if (source && !*private && EVP_PKEY_is_a(source, "EC")) {
		key = native_key(source, EVP_PKEY_PUBLIC_KEY);
	}
out:
	BN_clear_free(d); EVP_PKEY_free(source); BIO_free(bio); if (fd >= 0) close(fd); return key;
}

static CK_RV load_file(const char *path, unsigned index)
{
	if (!strncmp(path, "s31-hmac:", 9)) {
		struct p11_object *o = object_new();
		struct esp32s31_crypto_import key = {
			.algorithm = ESP32S31_CRYPTO_HMAC_KEY_MANAGER,
			.flags = ESP32S31_CRYPTO_KEY_PRIVATE,
			.key_len = sizeof(struct esp32s31_crypto_km_key),
		};
		int fd = -1; CK_RV result = CKR_DEVICE_ERROR;
		if (!o) return CKR_HOST_MEMORY;
		o->km_recovery = OPENSSL_malloc(key.key_len);
		if (!o->km_recovery) { result = CKR_HOST_MEMORY; goto hmac_out; }
		if (!s31_key_file(path + 9, o->km_recovery, key.key_len)) goto hmac_out;
		key.key = (uintptr_t)o->km_recovery;
		fd = open("/dev/s31-crypto", O_RDWR | O_CLOEXEC);
		if (fd < 0 || ioctl(fd, ESP32S31_CRYPTO_IOC_IMPORT, &key)) goto hmac_out;
		o->class = CKO_SECRET_KEY; o->type = CKK_SHA256_HMAC;
		o->token = o->private = o->sensitive = o->hardware = o->sign = o->verify = 1;
		o->secret_size = 32; o->id[0] = index; o->id_size = 1;
		o->label_size = snprintf((char *)o->label, sizeof(o->label), "km-hmac-%u", index);
		result = object_add(o); if (!result) o = NULL;
	hmac_out:
		if (fd >= 0) close(fd);
		p11_object_free(o); return result;
	}
	struct p11_object *priv = NULL, *pub = NULL; int private, opaque;
	EVP_PKEY *key = file_key(path, &private, &opaque), *public = NULL;
	const char *label = strrchr(path, '/'); CK_RV rv = CKR_DEVICE_ERROR;
	if (!key) return rv;
	public = native_key(key, EVP_PKEY_PUBLIC_KEY);
	if (!public) goto out;
	pub = object_new(); if (private) priv = object_new();
	if (!pub || (private && !priv)) { rv = CKR_HOST_MEMORY; goto out; }
	label = label ? label + 1 : path;
	pub->class = CKO_PUBLIC_KEY; pub->type = EVP_PKEY_is_a(key, "EC") ? CKK_EC : CKK_RSA; pub->token = 1;
	pub->key = public; public = NULL; pub->verify = 1; pub->encrypt = pub->type == CKK_RSA;
	pub->label_size = strlen(label) > P11_LABEL ? P11_LABEL : strlen(label); memcpy(pub->label, label, pub->label_size);
	pub->id[0] = index; pub->id_size = 1;
	if (priv) {
		priv->class = CKO_PRIVATE_KEY; priv->type = pub->type; priv->token = priv->private = priv->sensitive = 1;
		priv->hardware = opaque; priv->key = key; key = NULL; priv->sign = 1;
		priv->decrypt = priv->type == CKK_RSA && !opaque;
		memcpy(priv->label, pub->label, pub->label_size); priv->label_size = pub->label_size;
		priv->id[0] = index; priv->id_size = 1;
	}
	rv = object_add(pub); if (rv) goto out; pub = NULL;
	if (priv) { rv = object_add(priv); if (!rv) priv = NULL; }
out:
	p11_object_free(priv); p11_object_free(pub); EVP_PKEY_free(key); EVP_PKEY_free(public); return rv;
}

CK_RV p11_objects_load(void)
{
	struct esp32s31_crypto_caps caps; int fd; CK_RV rv = CKR_DEVICE_ERROR;
	const char *environment; char *paths = NULL, *next, *path; unsigned index = 0;
	fd = open("/dev/s31-crypto", O_RDWR | O_CLOEXEC);
	if (fd < 0) return rv;
	if (ioctl(fd, ESP32S31_CRYPTO_IOC_CAPS, &caps) || caps.version != ESP32S31_CRYPTO_ABI_VERSION ||
	    !(caps.algorithms & (1U << (ESP32S31_CRYPTO_RSA - 1))) ||
	    !(caps.algorithms & (1U << (ESP32S31_CRYPTO_ECDSA_P192 - 1))) ||
	    !(caps.algorithms & (1U << (ESP32S31_CRYPTO_ECDSA_P256 - 1))) ||
	    !(caps.algorithms & (1U << (ESP32S31_CRYPTO_ECDSA_P384 - 1)))) goto out;
	if (caps.algorithms & (1U << (ESP32S31_CRYPTO_HMAC_EFUSE - 1))) {
		for (unsigned i = 0; i < ESP32S31_CRYPTO_EFUSE_SLOTS; i++) {
			struct esp32s31_crypto_slot slot = { .index = i }; struct p11_object *o;
			if (ioctl(fd, ESP32S31_CRYPTO_IOC_SLOT, &slot)) goto out;
			if (slot.purpose != 8) continue;
			o = object_new(); if (!o) { rv = CKR_HOST_MEMORY; goto out; }
			o->class = CKO_SECRET_KEY; o->type = CKK_SHA256_HMAC;
			o->token = o->private = o->sensitive = o->hardware = o->sign = o->verify = 1;
			o->slot = i; o->secret_size = 32; o->id[0] = 0x80; o->id[1] = i; o->id_size = 2;
			o->label_size = snprintf((char *)o->label, sizeof(o->label), "efuse-hmac-%u", i);
			rv = object_add(o); if (rv) { p11_object_free(o); goto out; }
		}
	}
	environment = secure_getenv("S31_PKCS11_KEYS");
	if (environment && *environment) {
		if (strnlen(environment, 4097) > 4096) { rv = CKR_ARGUMENTS_BAD; goto out; }
		paths = OPENSSL_strdup(environment); if (!paths) { rv = CKR_HOST_MEMORY; goto out; }
		next = paths;
		while ((path = strsep(&next, ";"))) {
			if (!*path || ++index > 8 || strlen(path) > 512) { rv = CKR_ARGUMENTS_BAD; goto out; }
			rv = load_file(path, index); if (rv) goto out;
		}
	}
	rv = CKR_OK;
out:
	OPENSSL_free(paths); close(fd); return rv;
}

static CK_RV value(CK_ATTRIBUTE *a, const void *data, size_t size)
{
	CK_ULONG capacity = a->ulValueLen; a->ulValueLen = size;
	if (!a->pValue) return CKR_OK;
	if (capacity < size) return CKR_BUFFER_TOO_SMALL;
	if (size) memcpy(a->pValue, data, size);
	return CKR_OK;
}

static CK_RV number(CK_ATTRIBUTE *a, CK_ULONG n) { return value(a, &n, sizeof(n)); }
static CK_RV boolean(CK_ATTRIBUTE *a, int flag) { CK_BBOOL b = flag ? CK_TRUE : CK_FALSE; return value(a, &b, sizeof(b)); }

CK_RV p11_attribute(struct p11_object *o, CK_ATTRIBUTE *a)
{
	const char *component = NULL; BIGNUM *bn = NULL; unsigned char bytes[512]; int length; CK_RV rv;
	switch (a->type) {
	case CKA_CLASS: return number(a, o->class);
	case CKA_KEY_TYPE: return number(a, o->type);
	case CKA_TOKEN: return boolean(a, o->token);
	case CKA_PRIVATE: return boolean(a, o->private);
	case CKA_MODIFIABLE: return boolean(a, o->modifiable);
	case CKA_COPYABLE: return boolean(a, 0);
	case CKA_DESTROYABLE: return boolean(a, !o->token);
	case CKA_LABEL: return value(a, o->label, o->label_size);
	case CKA_ID: return value(a, o->id, o->id_size);
	case CKA_SIGN: return boolean(a, o->sign);
	case CKA_VERIFY: return boolean(a, o->verify);
	case CKA_ENCRYPT: return boolean(a, o->encrypt);
	case CKA_DECRYPT: return boolean(a, o->decrypt);
	case CKA_DERIVE: return boolean(a, o->derive);
	case CKA_WRAP: case CKA_UNWRAP: case CKA_TRUSTED: case CKA_LOCAL:
	case CKA_ALWAYS_AUTHENTICATE: case CKA_SIGN_RECOVER: case CKA_VERIFY_RECOVER: return boolean(a, 0);
	case CKA_KEY_GEN_MECHANISM: return number(a, CK_UNAVAILABLE_INFORMATION);
	case CKA_SENSITIVE: return boolean(a, o->sensitive);
	case CKA_EXTRACTABLE: return boolean(a, o->extractable);
	case CKA_ALWAYS_SENSITIVE: return boolean(a, o->hardware && o->sensitive);
	case CKA_NEVER_EXTRACTABLE: return boolean(a, o->hardware && !o->extractable);
	case CKA_VALUE_LEN:
		if (o->class != CKO_SECRET_KEY) break;
		return number(a, o->secret_size);
	case CKA_VALUE:
		if (o->type == CKK_EC && o->class == CKO_PRIVATE_KEY) goto sensitive;
		if (o->class != CKO_SECRET_KEY) break;
		if (o->hardware || o->sensitive || !o->extractable) goto sensitive;
		return value(a, o->secret, o->secret_size);
	case CKA_MODULUS_BITS:
		if (o->type == CKK_RSA && o->key) return number(a, EVP_PKEY_get_bits(o->key));
		break;
	case CKA_EC_PARAMS:
		if (o->type == CKK_EC && o->key) {
			char group[32]; size_t size; ASN1_OBJECT *oid; unsigned char *out = bytes;
			if (!EVP_PKEY_get_utf8_string_param(o->key, OSSL_PKEY_PARAM_GROUP_NAME,
							  group, sizeof(group), &size)) return CKR_DEVICE_ERROR;
			oid = OBJ_txt2obj(group, 0);
			if (!oid) return CKR_DEVICE_ERROR;
			length = i2d_ASN1_OBJECT(oid, NULL);
			rv = length > 0 && length <= (int)sizeof(bytes) &&
			     i2d_ASN1_OBJECT(oid, &out) == length ? value(a, bytes, length) : CKR_DEVICE_ERROR;
			ASN1_OBJECT_free(oid); return rv;
		}
		break;
	case CKA_EC_POINT:
		if (o->type == CKK_EC && o->class == CKO_PUBLIC_KEY && o->key) {
			unsigned char point[97], *out = bytes; size_t size;
			ASN1_OCTET_STRING *octets = ASN1_OCTET_STRING_new();
			if (!octets) return CKR_HOST_MEMORY;
			if (!EVP_PKEY_get_octet_string_param(o->key, OSSL_PKEY_PARAM_PUB_KEY,
							   point, sizeof(point), &size) ||
			    !ASN1_OCTET_STRING_set(octets, point, size)) {
				ASN1_OCTET_STRING_free(octets); return CKR_DEVICE_ERROR;
			}
			length = i2d_ASN1_OCTET_STRING(octets, &out);
			rv = length > 0 && length <= (int)sizeof(bytes) ? value(a, bytes, length) : CKR_DEVICE_ERROR;
			ASN1_OCTET_STRING_free(octets); return rv;
		}
		break;
	case CKA_MODULUS: component = OSSL_PKEY_PARAM_RSA_N; break;
	case CKA_PUBLIC_EXPONENT: component = OSSL_PKEY_PARAM_RSA_E; break;
	case CKA_PRIVATE_EXPONENT: component = OSSL_PKEY_PARAM_RSA_D; goto private;
	case CKA_PRIME_1: component = OSSL_PKEY_PARAM_RSA_FACTOR1; goto private;
	case CKA_PRIME_2: component = OSSL_PKEY_PARAM_RSA_FACTOR2; goto private;
	case CKA_EXPONENT_1: component = OSSL_PKEY_PARAM_RSA_EXPONENT1; goto private;
	case CKA_EXPONENT_2: component = OSSL_PKEY_PARAM_RSA_EXPONENT2; goto private;
	case CKA_COEFFICIENT: component = OSSL_PKEY_PARAM_RSA_COEFFICIENT1; goto private;
	default: break;
	}
	if (component && o->type == CKK_RSA && o->key && EVP_PKEY_get_bn_param(o->key, component, &bn)) {
		length = BN_num_bytes(bn);
		if (length <= (int)sizeof(bytes) && BN_bn2bin(bn, bytes) == length) rv = value(a, bytes, length);
		else rv = CKR_DEVICE_ERROR;
		BN_clear_free(bn); OPENSSL_cleanse(bytes, sizeof(bytes)); return rv;
	}
	a->ulValueLen = CK_UNAVAILABLE_INFORMATION; return CKR_ATTRIBUTE_TYPE_INVALID;
private:
	if (o->type != CKK_RSA || o->class != CKO_PRIVATE_KEY) { a->ulValueLen = CK_UNAVAILABLE_INFORMATION; return CKR_ATTRIBUTE_TYPE_INVALID; }
	if (o->hardware || o->sensitive || !o->extractable) goto sensitive;
	if (component && o->key && EVP_PKEY_get_bn_param(o->key, component, &bn)) {
		length = BN_num_bytes(bn); rv = length <= (int)sizeof(bytes) && BN_bn2bin(bn, bytes) == length ? value(a, bytes, length) : CKR_DEVICE_ERROR;
		BN_clear_free(bn); OPENSSL_cleanse(bytes, sizeof(bytes)); return rv;
	}
	a->ulValueLen = CK_UNAVAILABLE_INFORMATION; return CKR_ATTRIBUTE_TYPE_INVALID;
sensitive:
	a->ulValueLen = CK_UNAVAILABLE_INFORMATION; return CKR_ATTRIBUTE_SENSITIVE;
}

static CK_RV template_check(CK_ATTRIBUTE_PTR attrs, CK_ULONG count)
{
	if (count > 32 || (count && !attrs)) return CKR_ARGUMENTS_BAD;
	for (CK_ULONG i = 0; i < count; i++) {
		if (attrs[i].ulValueLen && !attrs[i].pValue) return CKR_ATTRIBUTE_VALUE_INVALID;
		for (CK_ULONG j = 0; j < i; j++) if (attrs[i].type == attrs[j].type) return CKR_TEMPLATE_INCONSISTENT;
	}
	return CKR_OK;
}

static const CK_ATTRIBUTE *find_attr(CK_ATTRIBUTE_PTR attrs, CK_ULONG count, CK_ATTRIBUTE_TYPE type)
{
	for (CK_ULONG i = 0; i < count; i++) if (attrs[i].type == type) return &attrs[i];
	return NULL;
}

static int get_number(const CK_ATTRIBUTE *a, CK_ULONG *out)
{
	if (!a || a->ulValueLen != sizeof(*out) || !a->pValue) return 0;
	memcpy(out, a->pValue, sizeof(*out)); return 1;
}

static int get_bool(const CK_ATTRIBUTE *a, int *out)
{
	CK_BBOOL b;
	if (!a || a->ulValueLen != sizeof(b) || !a->pValue) return 0;
	memcpy(&b, a->pValue, sizeof(b)); if (b != CK_TRUE && b != CK_FALSE) return 0;
	*out = b; return 1;
}

static EVP_PKEY *rsa_from_template(CK_ATTRIBUTE_PTR attrs, CK_ULONG count, int private)
{
	static const CK_ATTRIBUTE_TYPE types[] = { CKA_MODULUS, CKA_PUBLIC_EXPONENT, CKA_PRIVATE_EXPONENT,
		CKA_PRIME_1, CKA_PRIME_2, CKA_EXPONENT_1, CKA_EXPONENT_2, CKA_COEFFICIENT };
	static const char *names[] = { OSSL_PKEY_PARAM_RSA_N, OSSL_PKEY_PARAM_RSA_E, OSSL_PKEY_PARAM_RSA_D,
		OSSL_PKEY_PARAM_RSA_FACTOR1, OSSL_PKEY_PARAM_RSA_FACTOR2, OSSL_PKEY_PARAM_RSA_EXPONENT1,
		OSSL_PKEY_PARAM_RSA_EXPONENT2, OSSL_PKEY_PARAM_RSA_COEFFICIENT1 };
	BIGNUM *bn[8] = {0}; OSSL_PARAM_BLD *build = OSSL_PARAM_BLD_new(); OSSL_PARAM *params = NULL;
	EVP_PKEY_CTX *ctx = NULL; EVP_PKEY *key = NULL; unsigned fields = private ? 8 : 2;
	if (!build) goto out;
	for (unsigned i = 0; i < fields; i++) {
		const CK_ATTRIBUTE *a = find_attr(attrs, count, types[i]);
		if (!a && i >= (private ? 3U : 2U)) continue;
		if (!a || !a->ulValueLen || a->ulValueLen > 512 ||
		    !(bn[i] = BN_bin2bn(a->pValue, a->ulValueLen, NULL)) || !OSSL_PARAM_BLD_push_BN(build, names[i], bn[i])) goto out;
	}
	if (BN_num_bits(bn[0]) < 1024 || BN_num_bits(bn[0]) > 4096) goto out;
	params = OSSL_PARAM_BLD_to_param(build); ctx = EVP_PKEY_CTX_new_from_name(p11.libctx, "RSA", "provider=s31");
	if (params && ctx && EVP_PKEY_fromdata_init(ctx) > 0)
		EVP_PKEY_fromdata(ctx, &key, private ? EVP_PKEY_KEYPAIR : EVP_PKEY_PUBLIC_KEY, params);
out:
	params_free(params); OSSL_PARAM_BLD_free(build); EVP_PKEY_CTX_free(ctx);
	for (unsigned i = 0; i < 8; i++) BN_clear_free(bn[i]);
	return key;
}

static EVP_PKEY *ec_from_template(CK_ATTRIBUTE_PTR attrs, CK_ULONG count)
{
	const CK_ATTRIBUTE *parameters = find_attr(attrs, count, CKA_EC_PARAMS);
	const CK_ATTRIBUTE *point = find_attr(attrs, count, CKA_EC_POINT);
	const unsigned char *cursor; ASN1_OBJECT *oid = NULL; ASN1_OCTET_STRING *octets = NULL;
	EVP_PKEY_CTX *ctx = NULL; EVP_PKEY *key = NULL; int nid;
	if (!parameters || !point || !parameters->ulValueLen || parameters->ulValueLen > 32 ||
	    !point->ulValueLen || point->ulValueLen > 128) goto out;
	cursor = parameters->pValue; oid = d2i_ASN1_OBJECT(NULL, &cursor, parameters->ulValueLen);
	if (!oid || cursor != (unsigned char *)parameters->pValue + parameters->ulValueLen) goto out;
	nid = OBJ_obj2nid(oid);
	if (nid != NID_X9_62_prime192v1 && nid != NID_X9_62_prime256v1 && nid != NID_secp384r1) goto out;
	cursor = point->pValue; octets = d2i_ASN1_OCTET_STRING(NULL, &cursor, point->ulValueLen);
	if (!octets || cursor != (unsigned char *)point->pValue + point->ulValueLen ||
	    ASN1_STRING_length(octets) != (nid == NID_X9_62_prime192v1 ? 49 :
					  nid == NID_X9_62_prime256v1 ? 65 : 97) ||
	    ASN1_STRING_get0_data(octets)[0] != 4) goto out;
	{
		OSSL_PARAM p[] = {
			OSSL_PARAM_utf8_string(OSSL_PKEY_PARAM_GROUP_NAME, (char *)OBJ_nid2sn(nid), 0),
			OSSL_PARAM_octet_string(OSSL_PKEY_PARAM_PUB_KEY, (void *)ASN1_STRING_get0_data(octets),
						ASN1_STRING_length(octets)), OSSL_PARAM_END
		};
		ctx = EVP_PKEY_CTX_new_from_name(p11.libctx, "EC", "provider=s31");
		if (!ctx || EVP_PKEY_fromdata_init(ctx) <= 0 ||
		    EVP_PKEY_fromdata(ctx, &key, EVP_PKEY_PUBLIC_KEY, p) <= 0) goto out;
		EVP_PKEY_CTX_free(ctx); ctx = EVP_PKEY_CTX_new_from_pkey(p11.libctx, key, "provider=s31");
		if (!ctx || EVP_PKEY_public_check(ctx) <= 0) { EVP_PKEY_free(key); key = NULL; }
	}
out:
	ASN1_OBJECT_free(oid); ASN1_OCTET_STRING_free(octets); EVP_PKEY_CTX_free(ctx); return key;
}

static CK_RV object_template(struct p11_object *o, CK_ATTRIBUTE_PTR attrs, CK_ULONG count)
{
	CK_ULONG n; int flag; const CK_ATTRIBUTE *a;
	if (!get_number(find_attr(attrs, count, CKA_CLASS), &o->class) ||
	    !get_number(find_attr(attrs, count, CKA_KEY_TYPE), &o->type)) return CKR_TEMPLATE_INCOMPLETE;
	if ((o->class == CKO_PUBLIC_KEY || o->class == CKO_PRIVATE_KEY) && o->type == CKK_RSA) {
		o->private = o->sensitive = o->class == CKO_PRIVATE_KEY;
		o->sign = o->decrypt = o->class == CKO_PRIVATE_KEY;
		o->verify = o->encrypt = o->class == CKO_PUBLIC_KEY;
	} else if (o->class == CKO_PUBLIC_KEY && o->type == CKK_EC) {
		o->verify = 1;
	} else if (o->class == CKO_SECRET_KEY && (o->type == CKK_GENERIC_SECRET || o->type == CKK_SHA_1_HMAC ||
		   o->type == CKK_SHA224_HMAC || o->type == CKK_SHA256_HMAC || o->type == CKK_SHA384_HMAC || o->type == CKK_SHA512_HMAC)) {
		o->private = o->sensitive = o->sign = o->verify = 1;
	} else return CKR_KEY_TYPE_INCONSISTENT;
	o->modifiable = 1;
	for (CK_ULONG i = 0; i < count; i++) {
		a = &attrs[i];
		switch (a->type) {
		case CKA_CLASS: case CKA_KEY_TYPE: break;
		case CKA_TOKEN:
			if (!get_bool(a, &flag)) return CKR_ATTRIBUTE_VALUE_INVALID;
			if (flag) return CKR_TOKEN_WRITE_PROTECTED;
			break;
		case CKA_PRIVATE: if (!get_bool(a, &o->private)) return CKR_ATTRIBUTE_VALUE_INVALID; break;
		case CKA_SENSITIVE: if (!get_bool(a, &o->sensitive)) return CKR_ATTRIBUTE_VALUE_INVALID; break;
		case CKA_EXTRACTABLE: if (!get_bool(a, &o->extractable)) return CKR_ATTRIBUTE_VALUE_INVALID; break;
		case CKA_MODIFIABLE: if (!get_bool(a, &o->modifiable)) return CKR_ATTRIBUTE_VALUE_INVALID; break;
		case CKA_SIGN: case CKA_VERIFY: case CKA_ENCRYPT: case CKA_DECRYPT:
			if (!get_bool(a, &flag)) return CKR_ATTRIBUTE_VALUE_INVALID;
			if (a->type == CKA_SIGN) { if (flag && !o->sign) return CKR_TEMPLATE_INCONSISTENT; o->sign = flag; }
			if (a->type == CKA_VERIFY) { if (flag && !o->verify) return CKR_TEMPLATE_INCONSISTENT; o->verify = flag; }
			if (a->type == CKA_ENCRYPT) { if (flag && !o->encrypt) return CKR_TEMPLATE_INCONSISTENT; o->encrypt = flag; }
			if (a->type == CKA_DECRYPT) { if (flag && !o->decrypt) return CKR_TEMPLATE_INCONSISTENT; o->decrypt = flag; }
			break;
		case CKA_DERIVE: case CKA_WRAP: case CKA_UNWRAP: case CKA_ALWAYS_AUTHENTICATE:
			if (!get_bool(a, &flag)) return CKR_ATTRIBUTE_VALUE_INVALID;
			if (flag) return CKR_TEMPLATE_INCONSISTENT;
			break;
		case CKA_LABEL: case CKA_ID:
			if (a->ulValueLen > (a->type == CKA_LABEL ? P11_LABEL : P11_ID)) return CKR_ATTRIBUTE_VALUE_INVALID;
			if (a->type == CKA_LABEL) { o->label_size = a->ulValueLen; if (a->ulValueLen) memcpy(o->label, a->pValue, a->ulValueLen); }
			else { o->id_size = a->ulValueLen; if (a->ulValueLen) memcpy(o->id, a->pValue, a->ulValueLen); }
			break;
		case CKA_VALUE:
			if (o->class != CKO_SECRET_KEY) return CKR_ATTRIBUTE_TYPE_INVALID;
			break;
		case CKA_EC_PARAMS: case CKA_EC_POINT:
			if (o->type != CKK_EC || o->class != CKO_PUBLIC_KEY) return CKR_ATTRIBUTE_TYPE_INVALID;
			break;
		case CKA_MODULUS: case CKA_PUBLIC_EXPONENT: case CKA_PRIVATE_EXPONENT: case CKA_PRIME_1: case CKA_PRIME_2:
		case CKA_EXPONENT_1: case CKA_EXPONENT_2: case CKA_COEFFICIENT:
			if (o->type != CKK_RSA || (o->class == CKO_PUBLIC_KEY && a->type != CKA_MODULUS && a->type != CKA_PUBLIC_EXPONENT)) return CKR_ATTRIBUTE_TYPE_INVALID;
			break;
		case CKA_VALUE_LEN:
			if (o->class != CKO_SECRET_KEY || !get_number(a, &n)) return CKR_ATTRIBUTE_VALUE_INVALID;
			a = find_attr(attrs, count, CKA_VALUE); if (!a || n != a->ulValueLen) return CKR_TEMPLATE_INCONSISTENT;
			break;
		default: return CKR_ATTRIBUTE_TYPE_INVALID;
		}
	}
	if (o->type == CKK_RSA) {
		o->key = rsa_from_template(attrs, count, o->class == CKO_PRIVATE_KEY);
		if (!o->key) return CKR_ATTRIBUTE_VALUE_INVALID;
	} else if (o->type == CKK_EC) {
		o->key = ec_from_template(attrs, count);
		if (!o->key) return CKR_ATTRIBUTE_VALUE_INVALID;
	} else {
		a = find_attr(attrs, count, CKA_VALUE);
		if (!a) return CKR_TEMPLATE_INCOMPLETE;
		if (a->ulValueLen > P11_MAX_INPUT) return CKR_KEY_SIZE_RANGE;
		o->secret_size = a->ulValueLen; o->secret = OPENSSL_malloc(o->secret_size ? o->secret_size : 1);
		if (!o->secret) return CKR_HOST_MEMORY;
		if (o->secret_size) memcpy(o->secret, a->pValue, o->secret_size);
	}
	return CKR_OK;
}

CK_RV C_CreateObject(CK_SESSION_HANDLE handle, CK_ATTRIBUTE_PTR attrs, CK_ULONG count, CK_OBJECT_HANDLE_PTR result)
{
	struct p11_session *session; struct p11_object *o = NULL; CK_RV rv = p11_session_begin(handle, &session);
	if (rv) return rv;
	if (!result) { rv = CKR_ARGUMENTS_BAD; goto out; }
	rv = template_check(attrs, count); if (rv) goto out;
	o = object_new(); if (!o) { rv = CKR_HOST_MEMORY; goto out; }
	rv = object_template(o, attrs, count); if (rv) goto out;
	pthread_mutex_lock(&p11.lock);
	rv = session->closing ? CKR_SESSION_HANDLE_INVALID : o->private && !p11.logged_in ? CKR_USER_NOT_LOGGED_IN : CKR_OK;
	if (!rv) { o->owner = handle; rv = object_add(o); }
	if (!rv) { *result = o->handle; o = NULL; }
	pthread_mutex_unlock(&p11.lock);
out:
	p11_object_free(o); return p11_session_end(session, rv);
}

CK_RV C_DestroyObject(CK_SESSION_HANDLE session, CK_OBJECT_HANDLE handle)
{
	struct p11_object *o, **link; CK_RV rv = p11_global_begin(); if (rv) return rv;
	if (!p11_session_find(session)) return p11_global_end(CKR_SESSION_HANDLE_INVALID);
	o = p11_object_find(handle); if (!o) return p11_global_end(CKR_OBJECT_HANDLE_INVALID);
	if (o->token) return p11_global_end(CKR_ACTION_PROHIBITED);
	for (link = &p11.objects; *link != o; link = &(*link)->next) { }
	*link = o->next; o->deleted = 1; p11_object_free(o); return p11_global_end(CKR_OK);
}

CK_RV C_GetObjectSize(CK_SESSION_HANDLE session, CK_OBJECT_HANDLE handle, CK_ULONG_PTR size)
{
	CK_RV rv = p11_global_begin(); if (rv) return rv;
	if (!p11_session_find(session)) return p11_global_end(CKR_SESSION_HANDLE_INVALID);
	if (!p11_object_find(handle)) return p11_global_end(CKR_OBJECT_HANDLE_INVALID);
	if (!size) return p11_global_end(CKR_ARGUMENTS_BAD);
	*size = CK_UNAVAILABLE_INFORMATION; return p11_global_end(CKR_OK);
}

CK_RV C_GetAttributeValue(CK_SESSION_HANDLE session, CK_OBJECT_HANDLE handle, CK_ATTRIBUTE_PTR attrs, CK_ULONG count)
{
	CK_RV rv = p11_global_begin(), error; struct p11_object *o; if (rv) return rv;
	if (!p11_session_find(session)) return p11_global_end(CKR_SESSION_HANDLE_INVALID);
	o = p11_object_find(handle); if (!o) return p11_global_end(CKR_OBJECT_HANDLE_INVALID);
	if (count > 32 || (count && !attrs)) return p11_global_end(CKR_ARGUMENTS_BAD);
	for (CK_ULONG i = 0; i < count; i++) {
		error = p11_attribute(o, &attrs[i]);
		if (error && (!rv || error == CKR_ATTRIBUTE_SENSITIVE || (error == CKR_ATTRIBUTE_TYPE_INVALID && rv == CKR_BUFFER_TOO_SMALL))) rv = error;
	}
	return p11_global_end(rv);
}

CK_RV C_SetAttributeValue(CK_SESSION_HANDLE session, CK_OBJECT_HANDLE handle, CK_ATTRIBUTE_PTR attrs, CK_ULONG count)
{
	struct p11_object *o; unsigned char label[P11_LABEL], id[P11_ID]; size_t label_size, id_size;
	int sensitive, extractable, modifiable, b; CK_RV rv = p11_global_begin(); if (rv) return rv;
	if (!p11_session_find(session)) return p11_global_end(CKR_SESSION_HANDLE_INVALID);
	o = p11_object_find(handle); if (!o) return p11_global_end(CKR_OBJECT_HANDLE_INVALID);
	if (!o->modifiable) return p11_global_end(CKR_ATTRIBUTE_READ_ONLY);
	rv = template_check(attrs, count); if (rv) return p11_global_end(rv);
	memcpy(label, o->label, sizeof(label)); memcpy(id, o->id, sizeof(id));
	label_size = o->label_size; id_size = o->id_size; sensitive = o->sensitive; extractable = o->extractable; modifiable = o->modifiable;
	for (CK_ULONG i = 0; i < count; i++) {
		CK_ATTRIBUTE *a = &attrs[i];
		switch (a->type) {
		case CKA_LABEL: case CKA_ID:
			if (a->ulValueLen > (a->type == CKA_LABEL ? sizeof(label) : sizeof(id))) return p11_global_end(CKR_ATTRIBUTE_VALUE_INVALID);
			if (a->type == CKA_LABEL) { label_size = a->ulValueLen; if (label_size) memcpy(label, a->pValue, label_size); }
			else { id_size = a->ulValueLen; if (id_size) memcpy(id, a->pValue, id_size); }
			break;
		case CKA_SENSITIVE: case CKA_EXTRACTABLE: case CKA_MODIFIABLE:
			if (!get_bool(a, &b)) return p11_global_end(CKR_ATTRIBUTE_VALUE_INVALID);
			if (a->type == CKA_SENSITIVE) { if (sensitive && !b) return p11_global_end(CKR_ATTRIBUTE_READ_ONLY); sensitive = b; }
			if (a->type == CKA_EXTRACTABLE) { if (!extractable && b) return p11_global_end(CKR_ATTRIBUTE_READ_ONLY); extractable = b; }
			if (a->type == CKA_MODIFIABLE) modifiable = b;
			break;
		default: return p11_global_end(CKR_ATTRIBUTE_READ_ONLY);
		}
	}
	memcpy(o->label, label, sizeof(label)); memcpy(o->id, id, sizeof(id)); o->label_size = label_size; o->id_size = id_size;
	o->sensitive = sensitive; o->extractable = extractable; o->modifiable = modifiable; return p11_global_end(CKR_OK);
}

CK_RV C_FindObjectsInit(CK_SESSION_HANDLE handle, CK_ATTRIBUTE_PTR attrs, CK_ULONG count)
{
	struct p11_session *session; CK_RV rv = p11_session_begin(handle, &session); unsigned char bytes[512];
	if (rv) return rv;
	if (session->finding) return p11_session_end(session, CKR_OPERATION_ACTIVE);
	rv = template_check(attrs, count); if (rv) return p11_session_end(session, rv);
	session->found_count = session->found_position = 0;
	pthread_mutex_lock(&p11.lock);
	for (struct p11_object *o = p11.objects; o; o = o->next) {
		int match = !o->deleted && (!o->private || p11.logged_in);
		for (CK_ULONG i = 0; i < count && match; i++) {
			CK_ATTRIBUTE a = { attrs[i].type, NULL, 0 }; unsigned char *buffer = bytes; size_t capacity;
			match = !p11_attribute(o, &a) && a.ulValueLen == attrs[i].ulValueLen && a.ulValueLen <= P11_MAX_INPUT;
			if (!match) continue;
			capacity = a.ulValueLen;
			if (a.ulValueLen > sizeof(bytes)) buffer = OPENSSL_malloc(a.ulValueLen);
			if (!buffer) { rv = CKR_HOST_MEMORY; break; }
			a.pValue = buffer;
			match = !p11_attribute(o, &a) && (!a.ulValueLen || !CRYPTO_memcmp(buffer, attrs[i].pValue, a.ulValueLen));
			if (buffer != bytes) OPENSSL_clear_free(buffer, capacity);
		}
		if (rv) break;
		if (match) session->found[session->found_count++] = o->handle;
	}
	pthread_mutex_unlock(&p11.lock); OPENSSL_cleanse(bytes, sizeof(bytes)); session->finding = !rv;
	return p11_session_end(session, rv);
}

CK_RV C_FindObjects(CK_SESSION_HANDLE handle, CK_OBJECT_HANDLE_PTR objects, CK_ULONG maximum, CK_ULONG_PTR count)
{
	struct p11_session *session; CK_RV rv = p11_session_begin(handle, &session); if (rv) return rv;
	if (!count || (maximum && !objects)) return p11_session_end(session, CKR_ARGUMENTS_BAD);
	if (!session->finding) return p11_session_end(session, CKR_OPERATION_NOT_INITIALIZED);
	*count = 0; pthread_mutex_lock(&p11.lock);
	while (session->found_position < session->found_count && *count < maximum) {
		CK_OBJECT_HANDLE object = session->found[session->found_position++];
		if (p11_object_find(object)) objects[(*count)++] = object;
	}
	pthread_mutex_unlock(&p11.lock); return p11_session_end(session, CKR_OK);
}

CK_RV C_FindObjectsFinal(CK_SESSION_HANDLE handle)
{
	struct p11_session *session; CK_RV rv = p11_session_begin(handle, &session); if (rv) return rv;
	if (!session->finding) return p11_session_end(session, CKR_OPERATION_NOT_INITIALIZED);
	session->finding = 0; session->found_count = session->found_position = 0;
	return p11_session_end(session, CKR_OK);
}
