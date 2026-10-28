/* SPDX-License-Identifier: Apache-2.0 */
#include "p11.h"
#include <linux/esp32s31-crypto.h>
#include <openssl/rsa.h>
#include <openssl/rsaerr.h>
#include <openssl/ecdsa.h>
#include <limits.h>
#include <string.h>

static const CK_MECHANISM_TYPE mechanisms[] = {
	CKM_RSA_PKCS, CKM_RSA_X_509, CKM_RSA_PKCS_PSS, CKM_RSA_PKCS_OAEP,
	CKM_SHA1_RSA_PKCS, CKM_SHA224_RSA_PKCS, CKM_SHA256_RSA_PKCS, CKM_SHA384_RSA_PKCS, CKM_SHA512_RSA_PKCS,
	CKM_SHA1_RSA_PKCS_PSS, CKM_SHA224_RSA_PKCS_PSS, CKM_SHA256_RSA_PKCS_PSS, CKM_SHA384_RSA_PKCS_PSS, CKM_SHA512_RSA_PKCS_PSS,
	CKM_SHA_1_HMAC, CKM_SHA224_HMAC, CKM_SHA256_HMAC, CKM_SHA384_HMAC, CKM_SHA512_HMAC,
	CKM_SHA_1, CKM_SHA224, CKM_SHA256, CKM_SHA384, CKM_SHA512, CKM_SHA512_224, CKM_SHA512_256,
	CKM_ECDSA, CKM_ECDSA_SHA1, CKM_ECDSA_SHA224, CKM_ECDSA_SHA256, CKM_ECDSA_SHA384, CKM_ECDSA_SHA512
};

const CK_MECHANISM_TYPE *p11_mechanisms(size_t *count)
{
	*count = sizeof(mechanisms)/sizeof(mechanisms[0]); return mechanisms;
}

static const char *hash_name(CK_MECHANISM_TYPE mechanism)
{
	switch (mechanism) {
	case CKM_ECDSA_SHA1: return "SHA1";
	case CKM_ECDSA_SHA224: return "SHA224";
	case CKM_ECDSA_SHA256: return "SHA256";
	case CKM_ECDSA_SHA384: return "SHA384";
	case CKM_ECDSA_SHA512: return "SHA512";
	case CKM_SHA_1: case CKM_SHA1_RSA_PKCS: case CKM_SHA1_RSA_PKCS_PSS: case CKM_SHA_1_HMAC: return "SHA1";
	case CKM_SHA224: case CKM_SHA224_RSA_PKCS: case CKM_SHA224_RSA_PKCS_PSS: case CKM_SHA224_HMAC: return "SHA224";
	case CKM_SHA256: case CKM_SHA256_RSA_PKCS: case CKM_SHA256_RSA_PKCS_PSS: case CKM_SHA256_HMAC: return "SHA256";
	case CKM_SHA384: case CKM_SHA384_RSA_PKCS: case CKM_SHA384_RSA_PKCS_PSS: case CKM_SHA384_HMAC: return "SHA384";
	case CKM_SHA512: case CKM_SHA512_RSA_PKCS: case CKM_SHA512_RSA_PKCS_PSS: case CKM_SHA512_HMAC: return "SHA512";
	case CKM_SHA512_224: return "SHA512-224";
	case CKM_SHA512_256: return "SHA512-256";
	default: return NULL;
	}
}

static int is_digest(CK_MECHANISM_TYPE m)
{
	return m == CKM_SHA_1 || m == CKM_SHA224 || m == CKM_SHA256 || m == CKM_SHA384 ||
		m == CKM_SHA512 || m == CKM_SHA512_224 || m == CKM_SHA512_256;
}

static CK_KEY_TYPE hmac_type(CK_MECHANISM_TYPE m)
{
	switch (m) {
	case CKM_SHA_1_HMAC: return CKK_SHA_1_HMAC;
	case CKM_SHA224_HMAC: return CKK_SHA224_HMAC;
	case CKM_SHA256_HMAC: return CKK_SHA256_HMAC;
	case CKM_SHA384_HMAC: return CKK_SHA384_HMAC;
	case CKM_SHA512_HMAC: return CKK_SHA512_HMAC;
	default: return CK_UNAVAILABLE_INFORMATION;
	}
}

static int is_pss(CK_MECHANISM_TYPE m)
{
	return m == CKM_RSA_PKCS_PSS || m == CKM_SHA1_RSA_PKCS_PSS || m == CKM_SHA224_RSA_PKCS_PSS ||
		m == CKM_SHA256_RSA_PKCS_PSS || m == CKM_SHA384_RSA_PKCS_PSS || m == CKM_SHA512_RSA_PKCS_PSS;
}

static int is_ec_signature(CK_MECHANISM_TYPE m)
{
	return m == CKM_ECDSA || m == CKM_ECDSA_SHA1 || m == CKM_ECDSA_SHA224 ||
	       m == CKM_ECDSA_SHA256 || m == CKM_ECDSA_SHA384 || m == CKM_ECDSA_SHA512;
}
static int is_signature(CK_MECHANISM_TYPE m)
{
	return is_ec_signature(m) || m == CKM_RSA_PKCS || m == CKM_RSA_X_509 || is_pss(m) ||
		m == CKM_SHA1_RSA_PKCS || m == CKM_SHA224_RSA_PKCS || m == CKM_SHA256_RSA_PKCS ||
		m == CKM_SHA384_RSA_PKCS || m == CKM_SHA512_RSA_PKCS;
}

CK_RV p11_mechanism_info(CK_MECHANISM_TYPE m, CK_MECHANISM_INFO *info)
{
	memset(info, 0, sizeof(*info)); info->flags = CKF_HW;
	if (is_digest(m)) info->flags |= CKF_DIGEST;
	else if (hmac_type(m) != CK_UNAVAILABLE_INFORMATION) {
		info->flags |= CKF_SIGN | CKF_VERIFY; info->ulMaxKeySize = P11_MAX_INPUT;
	} else if (is_ec_signature(m)) {
		info->ulMinKeySize = 192; info->ulMaxKeySize = 384;
		info->flags |= CKF_SIGN | CKF_VERIFY | CKF_EC_F_P | CKF_EC_NAMEDCURVE | CKF_EC_UNCOMPRESS;
	} else if (is_signature(m) || m == CKM_RSA_PKCS_OAEP) {
		info->ulMinKeySize = 1024; info->ulMaxKeySize = 4096;
		if (is_signature(m)) info->flags |= CKF_SIGN | CKF_VERIFY;
		if (m == CKM_RSA_PKCS) info->flags |= CKF_ENCRYPT;
		if (m == CKM_RSA_X_509 || m == CKM_RSA_PKCS_OAEP) info->flags |= CKF_ENCRYPT | CKF_DECRYPT;
	} else return CKR_MECHANISM_INVALID;
	return CKR_OK;
}

void p11_operation_clear(struct p11_operation *op)
{
	EVP_PKEY_CTX_free(op->pkey); EVP_MD_CTX_free(op->digest); EVP_MAC_CTX_free(op->mac);
	p11_object_free(op->object); OPENSSL_cleanse(op, sizeof(*op));
}

static const char *mgf_name(CK_RSA_PKCS_MGF_TYPE mgf)
{
	switch (mgf) {
	case CKG_MGF1_SHA1: return "SHA1";
	case CKG_MGF1_SHA224: return "SHA224";
	case CKG_MGF1_SHA256: return "SHA256";
	case CKG_MGF1_SHA384: return "SHA384";
	case CKG_MGF1_SHA512: return "SHA512";
	default: return NULL;
	}
}

static CK_RV signature_init(struct p11_operation *op, CK_MECHANISM_PTR mechanism, CK_OBJECT_HANDLE handle, int sign)
{
	struct p11_object *o = NULL; EVP_PKEY_CTX *pk = NULL; EVP_MD *md = NULL, *mgf = NULL;
	EVP_MAC *mac = NULL; CK_RSA_PKCS_PSS_PARAMS pss = {0}; const char *hash, *mask;
	CK_RV rv; unsigned slot;
	if (op->active) return CKR_OPERATION_ACTIVE;
	if (!mechanism) return CKR_ARGUMENTS_BAD;
	if (!is_signature(mechanism->mechanism) && hmac_type(mechanism->mechanism) == CK_UNAVAILABLE_INFORMATION) return CKR_MECHANISM_INVALID;
	rv = p11_key_get(handle, &o); if (rv) return rv;
	op->object = o; op->mechanism = mechanism->mechanism;
	if ((sign && !o->sign) || (!sign && !o->verify)) { rv = CKR_KEY_FUNCTION_NOT_PERMITTED; goto fail; }
	if (hmac_type(op->mechanism) != CK_UNAVAILABLE_INFORMATION) {
		OSSL_PARAM params[3]; unsigned n = 0;
		if (o->class != CKO_SECRET_KEY || (o->type != CKK_GENERIC_SECRET && o->type != hmac_type(op->mechanism))) { rv = CKR_KEY_TYPE_INCONSISTENT; goto fail; }
		if (mechanism->ulParameterLen) { rv = CKR_MECHANISM_PARAM_INVALID; goto fail; }
		hash = hash_name(op->mechanism);
		params[n++] = OSSL_PARAM_construct_utf8_string(OSSL_MAC_PARAM_DIGEST, (char *)hash, 0);
		if (o->slot >= 0 || o->km_recovery) {
			if (op->mechanism != CKM_SHA256_HMAC) { rv = CKR_KEY_TYPE_INCONSISTENT; goto fail; }
			if (o->km_recovery)
				params[n++] = OSSL_PARAM_construct_octet_string("s31-km-key", o->km_recovery,
					 sizeof(struct esp32s31_crypto_km_key));
			else { slot = o->slot; params[n++] = OSSL_PARAM_construct_uint("s31-slot", &slot); }
		}
		params[n] = OSSL_PARAM_construct_end();
		mac = EVP_MAC_fetch(p11.libctx, (o->slot >= 0 || o->km_recovery) ? "S31-HMAC" : "HMAC", "provider=s31");
		op->mac = mac ? EVP_MAC_CTX_new(mac) : NULL;
		if (!op->mac || !EVP_MAC_init(op->mac, (o->slot >= 0 || o->km_recovery) ? NULL : o->secret, (o->slot >= 0 || o->km_recovery) ? 0 : o->secret_size, params)) { rv = CKR_DEVICE_ERROR; goto fail; }
		op->result_size = EVP_MAC_CTX_get_mac_size(op->mac); op->active = 1; EVP_MAC_free(mac); return CKR_OK;
	}
	if (is_ec_signature(op->mechanism)) {
		if (o->type != CKK_EC || (sign ? o->class != CKO_PRIVATE_KEY : o->class != CKO_PUBLIC_KEY)) {
			rv = CKR_KEY_TYPE_INCONSISTENT; goto fail;
		}
		if (mechanism->ulParameterLen) { rv = CKR_MECHANISM_PARAM_INVALID; goto fail; }
		hash = hash_name(op->mechanism); op->raw = op->mechanism == CKM_ECDSA ? 4 : 0;
		if (op->raw) {
			op->pkey = EVP_PKEY_CTX_new_from_pkey(p11.libctx, o->key, "provider=s31");
			if (!op->pkey || (sign ? EVP_PKEY_sign_init(op->pkey) : EVP_PKEY_verify_init(op->pkey)) <= 0) {
				rv = CKR_DEVICE_ERROR; goto fail;
			}
		} else {
			op->digest = EVP_MD_CTX_new();
			if (!op->digest || !(sign ?
			    EVP_DigestSignInit_ex(op->digest, NULL, hash, p11.libctx, "provider=s31", o->key, NULL) :
			    EVP_DigestVerifyInit_ex(op->digest, NULL, hash, p11.libctx, "provider=s31", o->key, NULL))) {
				rv = CKR_DEVICE_ERROR; goto fail;
			}
		}
		op->result_size = 2 * ((EVP_PKEY_get_bits(o->key) + 7) / 8);
		op->active = 1; return CKR_OK;
	}
	if (o->type != CKK_RSA || (sign ? o->class != CKO_PRIVATE_KEY : o->class != CKO_PUBLIC_KEY)) { rv = CKR_KEY_TYPE_INCONSISTENT; goto fail; }
	hash = hash_name(op->mechanism);
	if (is_pss(op->mechanism)) {
		if (!mechanism->pParameter || mechanism->ulParameterLen != sizeof(pss)) { rv = CKR_MECHANISM_PARAM_INVALID; goto fail; }
		memcpy(&pss, mechanism->pParameter, sizeof(pss)); mask = mgf_name(pss.mgf);
		if (!is_digest(pss.hashAlg) || !(hash_name(pss.hashAlg)) || !mask || pss.sLen > INT_MAX ||
		    (hash && strcmp(hash, hash_name(pss.hashAlg)))) { rv = CKR_MECHANISM_PARAM_INVALID; goto fail; }
		hash = hash_name(pss.hashAlg); md = EVP_MD_fetch(p11.libctx, hash, "provider=s31");
		mgf = EVP_MD_fetch(p11.libctx, mask, "provider=s31");
		if (!md || !mgf) { rv = CKR_DEVICE_ERROR; goto fail; }
		op->input_size = EVP_MD_get_size(md);
		if (pss.sLen > (size_t)((EVP_PKEY_get_bits(o->key) - 1 + 7) / 8) - op->input_size - 2) { rv = CKR_MECHANISM_PARAM_INVALID; goto fail; }
	} else if (mechanism->ulParameterLen) { rv = CKR_MECHANISM_PARAM_INVALID; goto fail; }
	op->raw = op->mechanism == CKM_RSA_PKCS ? 1 : op->mechanism == CKM_RSA_X_509 ? 2 : op->mechanism == CKM_RSA_PKCS_PSS ? 3 : 0;
	if (op->raw) {
		op->pkey = EVP_PKEY_CTX_new_from_pkey(p11.libctx, o->key, "provider=s31"); pk = op->pkey;
		if (!pk || (sign ? EVP_PKEY_sign_init(pk) : EVP_PKEY_verify_init(pk)) <= 0) { rv = CKR_DEVICE_ERROR; goto fail; }
	} else {
		op->digest = EVP_MD_CTX_new();
		if (!op->digest || !(sign ? EVP_DigestSignInit_ex(op->digest, &pk, hash, p11.libctx, "provider=s31", o->key, NULL) :
			EVP_DigestVerifyInit_ex(op->digest, &pk, hash, p11.libctx, "provider=s31", o->key, NULL))) { rv = CKR_DEVICE_ERROR; goto fail; }
	}
	if (EVP_PKEY_CTX_set_rsa_padding(pk, is_pss(op->mechanism) ? RSA_PKCS1_PSS_PADDING : op->raw == 2 ? RSA_NO_PADDING : RSA_PKCS1_PADDING) <= 0 ||
	    (is_pss(op->mechanism) && ((op->raw && EVP_PKEY_CTX_set_signature_md(pk, md) <= 0) ||
	     EVP_PKEY_CTX_set_rsa_mgf1_md(pk, mgf) <= 0 || EVP_PKEY_CTX_set_rsa_pss_saltlen(pk, pss.sLen) <= 0))) { rv = CKR_MECHANISM_PARAM_INVALID; goto fail; }
	op->result_size = EVP_PKEY_get_size(o->key); op->active = 1; EVP_MD_free(md); EVP_MD_free(mgf); return CKR_OK;
fail:
	EVP_MD_free(md); EVP_MD_free(mgf); EVP_MAC_free(mac); p11_operation_clear(op); return rv;
}

CK_RV C_SignInit(CK_SESSION_HANDLE handle, CK_MECHANISM_PTR mechanism, CK_OBJECT_HANDLE key)
{
	struct p11_session *s; CK_RV rv = p11_session_begin(handle, &s); if (rv) return rv;
	return p11_session_end(s, signature_init(&s->sign, mechanism, key, 1));
}

CK_RV C_VerifyInit(CK_SESSION_HANDLE handle, CK_MECHANISM_PTR mechanism, CK_OBJECT_HANDLE key)
{
	struct p11_session *s; CK_RV rv = p11_session_begin(handle, &s); if (rv) return rv;
	return p11_session_end(s, signature_init(&s->verify, mechanism, key, 0));
}

static CK_RV update(struct p11_operation *op, const unsigned char *data, CK_ULONG length, int sign)
{
	CK_RV rv;
	if (!op->active) return CKR_OPERATION_NOT_INITIALIZED;
	rv = p11_private_check(op->object); if (rv) goto fail;
	if (!data && length) { rv = CKR_ARGUMENTS_BAD; goto fail; }
	if (op->raw) { rv = CKR_FUNCTION_NOT_SUPPORTED; goto fail; }
	if (!(op->mac ? EVP_MAC_update(op->mac, data, length) : sign ? EVP_DigestSignUpdate(op->digest, data, length) :
	      EVP_DigestVerifyUpdate(op->digest, data, length))) { rv = CKR_DEVICE_ERROR; goto fail; }
	op->multipart = 1; return CKR_OK;
fail:
	p11_operation_clear(op); return rv;
}

CK_RV C_SignUpdate(CK_SESSION_HANDLE handle, CK_BYTE_PTR input, CK_ULONG length)
{
	struct p11_session *s; CK_RV rv = p11_session_begin(handle, &s); if (rv) return rv;
	return p11_session_end(s, update(&s->sign, input, length, 1));
}

CK_RV C_VerifyUpdate(CK_SESSION_HANDLE handle, CK_BYTE_PTR input, CK_ULONG length)
{
	struct p11_session *s; CK_RV rv = p11_session_begin(handle, &s); if (rv) return rv;
	return p11_session_end(s, update(&s->verify, input, length, 0));
}

static CK_RV raw_input(struct p11_operation *op, const unsigned char **input, size_t *length)
{
	if ((!*input && *length) || *length > sizeof(op->input)) return CKR_DATA_LEN_RANGE;
	if (op->raw == 4 && (!*length || *length > 64)) return CKR_DATA_LEN_RANGE;
	if ((op->raw == 1 && *length > op->result_size - 11) ||
	    (op->raw == 2 && *length > op->result_size) || (op->raw == 3 && *length != op->input_size)) return CKR_DATA_LEN_RANGE;
	if (op->raw == 2) {
		memset(op->input, 0, op->result_size); if (*length) memcpy(op->input + op->result_size - *length, *input, *length);
		*input = op->input; *length = op->result_size;
	}
	return CKR_OK;
}

static CK_RV ec_sign(struct p11_operation *op, const unsigned char *input, size_t input_size,
		      unsigned char *output, int single)
{
	unsigned char der[104]; const unsigned char *cursor = der;
	size_t length = sizeof(der), width = op->result_size / 2;
	ECDSA_SIG *sig = NULL; const BIGNUM *r, *s; CK_RV rv = CKR_DEVICE_ERROR;
	if (op->raw) {
		if (EVP_PKEY_sign(op->pkey, der, &length, input, input_size) <= 0) goto out;
	} else if ((single && !EVP_DigestSignUpdate(op->digest, input, input_size)) ||
		   !EVP_DigestSignFinal(op->digest, der, &length)) goto out;
	if (length > sizeof(der) || !(sig = d2i_ECDSA_SIG(NULL, &cursor, length)) ||
	    cursor != der + length) goto out;
	ECDSA_SIG_get0(sig, &r, &s);
	if (BN_is_negative(r) || BN_is_negative(s) ||
	    BN_bn2binpad(r, output, width) != (int)width ||
	    BN_bn2binpad(s, output + width, width) != (int)width) goto out;
	rv = CKR_OK;
out:
	ECDSA_SIG_free(sig); OPENSSL_cleanse(der, sizeof(der)); return rv;
}

static int ec_verify(struct p11_operation *op, const unsigned char *input, size_t input_size,
		      const unsigned char *signature, int single)
{
	size_t width = op->result_size / 2; unsigned char der[104], *cursor = der;
	ECDSA_SIG *sig = ECDSA_SIG_new(); int length, result = -1;
	BIGNUM *r = BN_bin2bn(signature, width, NULL), *s = BN_bin2bn(signature + width, width, NULL);
	if (!r || !s || !sig || !ECDSA_SIG_set0(sig, r, s)) goto out;
	r = s = NULL; length = i2d_ECDSA_SIG(sig, NULL);
	if (length <= 0 || (size_t)length > sizeof(der) || i2d_ECDSA_SIG(sig, &cursor) != length) goto out;
	if (op->raw) result = EVP_PKEY_verify(op->pkey, der, length, input, input_size);
	else if (!single || EVP_DigestVerifyUpdate(op->digest, input, input_size))
		result = EVP_DigestVerifyFinal(op->digest, der, length);
out:
	BN_free(r); BN_free(s); ECDSA_SIG_free(sig); OPENSSL_cleanse(der, sizeof(der)); return result;
}

static CK_RV sign_finish(struct p11_operation *op, const unsigned char *input, size_t input_size,
			 CK_BYTE_PTR output, CK_ULONG_PTR length, int single)
{
	CK_RV rv; size_t size;
	if (!op->active) return CKR_OPERATION_NOT_INITIALIZED;
	rv = p11_private_check(op->object); if (rv) goto fail;
	if (!length || (single && !input && input_size)) { rv = CKR_ARGUMENTS_BAD; goto fail; }
	if (single && op->multipart) { rv = CKR_OPERATION_ACTIVE; goto fail; }
	if (!single && op->raw) { rv = CKR_FUNCTION_NOT_SUPPORTED; goto fail; }
	if (single && op->raw) { rv = raw_input(op, &input, &input_size); if (rv) goto fail; }
	size = op->result_size;
	if (!output) { *length = size; return CKR_OK; }
	if (*length < size) { *length = size; return CKR_BUFFER_TOO_SMALL; }
	if (op->object->type == CKK_EC) rv = ec_sign(op, input, input_size, output, single);
	else if (op->raw) rv = EVP_PKEY_sign(op->pkey, output, &size, input, input_size) > 0 ? CKR_OK : CKR_DEVICE_ERROR;
	else {
		if (single && !(op->mac ? EVP_MAC_update(op->mac, input, input_size) : EVP_DigestSignUpdate(op->digest, input, input_size))) { rv = CKR_DEVICE_ERROR; goto wipe; }
		rv = (op->mac ? EVP_MAC_final(op->mac, output, &size, op->result_size) : EVP_DigestSignFinal(op->digest, output, &size)) ? CKR_OK : CKR_DEVICE_ERROR;
	}
	if (!rv && size != op->result_size) rv = CKR_DEVICE_ERROR;
wipe:
	if (rv) OPENSSL_cleanse(output, op->result_size);
	*length = rv ? 0 : size;
fail:
	p11_operation_clear(op); return rv;
}

CK_RV C_Sign(CK_SESSION_HANDLE handle, CK_BYTE_PTR input, CK_ULONG input_size, CK_BYTE_PTR output, CK_ULONG_PTR length)
{
	struct p11_session *s; CK_RV rv = p11_session_begin(handle, &s); if (rv) return rv;
	return p11_session_end(s, sign_finish(&s->sign, input, input_size, output, length, 1));
}

CK_RV C_SignFinal(CK_SESSION_HANDLE handle, CK_BYTE_PTR output, CK_ULONG_PTR length)
{
	struct p11_session *s; CK_RV rv = p11_session_begin(handle, &s); if (rv) return rv;
	return p11_session_end(s, sign_finish(&s->sign, NULL, 0, output, length, 0));
}

static CK_RV verify_finish(struct p11_operation *op, const unsigned char *input, size_t input_size,
			   const unsigned char *signature, size_t signature_size, int single)
{
	unsigned char actual[EVP_MAX_MD_SIZE]; size_t size = sizeof(actual); int result; CK_RV rv;
	if (!op->active) return CKR_OPERATION_NOT_INITIALIZED;
	rv = p11_private_check(op->object); if (rv) goto out;
	if ((!signature && signature_size) || (single && !input && input_size)) { rv = CKR_ARGUMENTS_BAD; goto out; }
	if (signature_size != op->result_size) { rv = CKR_SIGNATURE_LEN_RANGE; goto out; }
	if (single && op->multipart) { rv = CKR_OPERATION_ACTIVE; goto out; }
	if (!single && op->raw) { rv = CKR_FUNCTION_NOT_SUPPORTED; goto out; }
	if (op->object->type == CKK_EC) {
		if (op->raw) { rv = raw_input(op, &input, &input_size); if (rv) goto out; }
		result = ec_verify(op, input, input_size, signature, single);
		rv = result > 0 ? CKR_OK : result == 0 ? CKR_SIGNATURE_INVALID : CKR_DEVICE_ERROR;
		goto out;
	}
	if (op->raw) {
		rv = raw_input(op, &input, &input_size); if (rv) goto out;
		result = EVP_PKEY_verify(op->pkey, signature, signature_size, input, input_size);
	} else if (op->mac) {
		if ((single && !EVP_MAC_update(op->mac, input, input_size)) || !EVP_MAC_final(op->mac, actual, &size, sizeof(actual)) || size != signature_size) { rv = CKR_DEVICE_ERROR; goto out; }
		result = !CRYPTO_memcmp(actual, signature, size);
	} else {
		if (single && !EVP_DigestVerifyUpdate(op->digest, input, input_size)) { rv = CKR_DEVICE_ERROR; goto out; }
		result = EVP_DigestVerifyFinal(op->digest, signature, signature_size);
	}
	rv = result > 0 ? CKR_OK : result == 0 ? CKR_SIGNATURE_INVALID : CKR_DEVICE_ERROR;
out:
	OPENSSL_cleanse(actual, sizeof(actual)); p11_operation_clear(op); return rv;
}

CK_RV C_Verify(CK_SESSION_HANDLE handle, CK_BYTE_PTR input, CK_ULONG input_size, CK_BYTE_PTR signature, CK_ULONG signature_size)
{
	struct p11_session *s; CK_RV rv = p11_session_begin(handle, &s); if (rv) return rv;
	return p11_session_end(s, verify_finish(&s->verify, input, input_size, signature, signature_size, 1));
}

CK_RV C_VerifyFinal(CK_SESSION_HANDLE handle, CK_BYTE_PTR signature, CK_ULONG signature_size)
{
	struct p11_session *s; CK_RV rv = p11_session_begin(handle, &s); if (rv) return rv;
	return p11_session_end(s, verify_finish(&s->verify, NULL, 0, signature, signature_size, 0));
}

static void digest_clear(struct p11_session *s)
{
	EVP_MD_CTX_free(s->digest); s->digest = NULL;
	s->digest_active = s->digest_multipart = s->digest_secret = 0;
}

CK_RV C_DigestInit(CK_SESSION_HANDLE handle, CK_MECHANISM_PTR mechanism)
{
	struct p11_session *s; EVP_MD *md = NULL; CK_RV rv = p11_session_begin(handle, &s); if (rv) return rv;
	if (s->digest_active) { rv = CKR_OPERATION_ACTIVE; goto out; }
	if (!mechanism) { rv = CKR_ARGUMENTS_BAD; goto out; }
	if (!is_digest(mechanism->mechanism)) { rv = CKR_MECHANISM_INVALID; goto out; }
	if (mechanism->ulParameterLen) { rv = CKR_MECHANISM_PARAM_INVALID; goto out; }
	md = EVP_MD_fetch(p11.libctx, hash_name(mechanism->mechanism), "provider=s31");
	if (!s->digest) s->digest = EVP_MD_CTX_new();
	if (!md || !s->digest || !EVP_DigestInit_ex2(s->digest, md, NULL)) { digest_clear(s); rv = CKR_DEVICE_ERROR; }
	else s->digest_active = 1;
	s->digest_multipart = s->digest_secret = 0;
out:
	EVP_MD_free(md); return p11_session_end(s, rv);
}

CK_RV C_DigestUpdate(CK_SESSION_HANDLE handle, CK_BYTE_PTR input, CK_ULONG size)
{
	struct p11_session *s; CK_RV rv = p11_session_begin(handle, &s); if (rv) return rv;
	if (!s->digest_active) rv = CKR_OPERATION_NOT_INITIALIZED;
	else if (!input && size) rv = CKR_ARGUMENTS_BAD;
	else if (!EVP_DigestUpdate(s->digest, input, size)) rv = CKR_DEVICE_ERROR;
	else s->digest_multipart = 1;
	if (rv) digest_clear(s);
	return p11_session_end(s, rv);
}

static CK_RV digest_finish(struct p11_session *s, const unsigned char *input, size_t input_size,
			   CK_BYTE_PTR output, CK_ULONG_PTR length, int single)
{
	CK_RV rv = CKR_OK; unsigned int size; int needed;
	if (!s->digest_active) return CKR_OPERATION_NOT_INITIALIZED;
	if (!length || (single && !input && input_size)) { rv = CKR_ARGUMENTS_BAD; goto out; }
	if (single && s->digest_multipart) { rv = CKR_OPERATION_ACTIVE; goto out; }
	needed = EVP_MD_CTX_get_size(s->digest);
	if (needed <= 0) { rv = CKR_DEVICE_ERROR; goto out; }
	if (!output) { *length = needed; return CKR_OK; }
	if (*length < (unsigned)needed) { *length = needed; return CKR_BUFFER_TOO_SMALL; }
	if ((single && !EVP_DigestUpdate(s->digest, input, input_size)) || !EVP_DigestFinal_ex(s->digest, output, &size) || size != (unsigned)needed) {
		OPENSSL_cleanse(output, needed); *length = 0; rv = CKR_DEVICE_ERROR;
	} else *length = size;
out:
	/* A consumed ordinary digest can reuse the provider's completed AF_ALG
	 * sockets. Never retain failed/partial operations or DigestKey state.
	 * Session teardown still owns and frees the cached context.
	 */
	if (rv || s->digest_secret) digest_clear(s);
	else s->digest_active = s->digest_multipart = 0;
	return rv;
}

CK_RV C_Digest(CK_SESSION_HANDLE handle, CK_BYTE_PTR input, CK_ULONG input_size, CK_BYTE_PTR output, CK_ULONG_PTR length)
{
	struct p11_session *s; CK_RV rv = p11_session_begin(handle, &s); if (rv) return rv;
	return p11_session_end(s, digest_finish(s, input, input_size, output, length, 1));
}

CK_RV C_DigestFinal(CK_SESSION_HANDLE handle, CK_BYTE_PTR output, CK_ULONG_PTR length)
{
	struct p11_session *s; CK_RV rv = p11_session_begin(handle, &s); if (rv) return rv;
	return p11_session_end(s, digest_finish(s, NULL, 0, output, length, 0));
}

CK_RV C_DigestKey(CK_SESSION_HANDLE handle, CK_OBJECT_HANDLE key)
{
	struct p11_session *s; struct p11_object *o = NULL; CK_RV rv = p11_session_begin(handle, &s); if (rv) return rv;
	if (!s->digest_active) { rv = CKR_OPERATION_NOT_INITIALIZED; goto out; }
	rv = p11_key_get(key, &o); if (rv) goto out;
	if (o->class != CKO_SECRET_KEY || o->hardware) { rv = CKR_KEY_INDIGESTIBLE; goto out; }
	rv = p11_private_check(o); if (rv) goto out;
	s->digest_secret = 1;
	if (!EVP_DigestUpdate(s->digest, o->secret, o->secret_size)) rv = CKR_DEVICE_ERROR;
	else s->digest_multipart = 1;
out:
	p11_object_free(o);
	if (rv) digest_clear(s);
	return p11_session_end(s, rv);
}

static CK_RV cipher_init(struct p11_operation *op, CK_MECHANISM_PTR mechanism, CK_OBJECT_HANDLE handle, int decrypt)
{
	struct p11_object *o; CK_RSA_PKCS_OAEP_PARAMS oaep = {0}; const char *hash, *mask;
	EVP_MD *md = NULL, *mgf = NULL; unsigned char *label = NULL; CK_RV rv;
	if (op->active) return CKR_OPERATION_ACTIVE;
	if (!mechanism) return CKR_ARGUMENTS_BAD;
	/* Private v1.5 decryption is deliberately absent. Its PKCS#11 padding-
	 * error contract must not quietly disable OpenSSL implicit rejection.
	 */
	if (mechanism->mechanism != CKM_RSA_X_509 && mechanism->mechanism != CKM_RSA_PKCS_OAEP &&
	    (decrypt || mechanism->mechanism != CKM_RSA_PKCS)) return CKR_MECHANISM_INVALID;
	rv = p11_key_get(handle, &o); if (rv) return rv;
	op->object = o; op->mechanism = mechanism->mechanism;
	if (o->type != CKK_RSA || (decrypt ? o->class != CKO_PRIVATE_KEY : o->class != CKO_PUBLIC_KEY)) { rv = CKR_KEY_TYPE_INCONSISTENT; goto fail; }
	if (decrypt ? !o->decrypt : !o->encrypt) { rv = CKR_KEY_FUNCTION_NOT_PERMITTED; goto fail; }
	op->pkey = EVP_PKEY_CTX_new_from_pkey(p11.libctx, o->key, "provider=s31");
	if (!op->pkey || (decrypt ? EVP_PKEY_decrypt_init(op->pkey) : EVP_PKEY_encrypt_init(op->pkey)) <= 0) { rv = CKR_DEVICE_ERROR; goto fail; }
	if (EVP_PKEY_CTX_set_rsa_padding(op->pkey, op->mechanism == CKM_RSA_X_509 ? RSA_NO_PADDING : op->mechanism == CKM_RSA_PKCS_OAEP ? RSA_PKCS1_OAEP_PADDING : RSA_PKCS1_PADDING) <= 0) { rv = CKR_MECHANISM_PARAM_INVALID; goto fail; }
	op->result_size = EVP_PKEY_get_size(o->key);
	if (op->mechanism == CKM_RSA_PKCS_OAEP) {
		if (!mechanism->pParameter || mechanism->ulParameterLen != sizeof(oaep)) { rv = CKR_MECHANISM_PARAM_INVALID; goto fail; }
		memcpy(&oaep, mechanism->pParameter, sizeof(oaep));
		hash = is_digest(oaep.hashAlg) ? hash_name(oaep.hashAlg) : NULL; mask = mgf_name(oaep.mgf);
		if (!hash || !mask || oaep.source != CKZ_DATA_SPECIFIED || oaep.ulSourceDataLen > P11_MAX_INPUT ||
		    (oaep.ulSourceDataLen && !oaep.pSourceData)) { rv = CKR_MECHANISM_PARAM_INVALID; goto fail; }
		md = EVP_MD_fetch(p11.libctx, hash, "provider=s31"); mgf = EVP_MD_fetch(p11.libctx, mask, "provider=s31");
		if (!md || !mgf) { rv = CKR_DEVICE_ERROR; goto fail; }
		if (op->result_size < 2 * (size_t)EVP_MD_get_size(md) + 2) { rv = CKR_MECHANISM_PARAM_INVALID; goto fail; }
		op->input_size = op->result_size - 2 * (size_t)EVP_MD_get_size(md) - 2;
		if (EVP_PKEY_CTX_set_rsa_oaep_md(op->pkey, md) <= 0 || EVP_PKEY_CTX_set_rsa_mgf1_md(op->pkey, mgf) <= 0) { rv = CKR_MECHANISM_PARAM_INVALID; goto fail; }
		if (oaep.ulSourceDataLen) {
			label = OPENSSL_memdup(oaep.pSourceData, oaep.ulSourceDataLen);
			if (!label) { rv = CKR_HOST_MEMORY; goto fail; }
			if (EVP_PKEY_CTX_set0_rsa_oaep_label(op->pkey, label, oaep.ulSourceDataLen) <= 0) { rv = CKR_MECHANISM_PARAM_INVALID; goto fail; }
			label = NULL;
		}
	} else {
		if (mechanism->ulParameterLen) { rv = CKR_MECHANISM_PARAM_INVALID; goto fail; }
		op->input_size = op->result_size - (op->mechanism == CKM_RSA_PKCS ? 11 : 0);
	}
	op->active = 1; EVP_MD_free(md); EVP_MD_free(mgf); return CKR_OK;
fail:
	OPENSSL_clear_free(label, oaep.ulSourceDataLen); EVP_MD_free(md); EVP_MD_free(mgf); p11_operation_clear(op); return rv;
}

CK_RV C_EncryptInit(CK_SESSION_HANDLE handle, CK_MECHANISM_PTR mechanism, CK_OBJECT_HANDLE key)
{
	struct p11_session *s; CK_RV rv = p11_session_begin(handle, &s); if (rv) return rv;
	return p11_session_end(s, cipher_init(&s->encrypt, mechanism, key, 0));
}

CK_RV C_DecryptInit(CK_SESSION_HANDLE handle, CK_MECHANISM_PTR mechanism, CK_OBJECT_HANDLE key)
{
	struct p11_session *s; CK_RV rv = p11_session_begin(handle, &s); if (rv) return rv;
	return p11_session_end(s, cipher_init(&s->decrypt, mechanism, key, 1));
}

static CK_RV cipher_run(struct p11_operation *op, const unsigned char *input, size_t input_size,
			 CK_BYTE_PTR output, CK_ULONG_PTR length, int decrypt)
{
	CK_RV rv; size_t size; int result;
	if (!op->active) return CKR_OPERATION_NOT_INITIALIZED;
	rv = p11_private_check(op->object); if (rv) goto out;
	if (!length || (!input && input_size)) { rv = CKR_ARGUMENTS_BAD; goto out; }
	if ((decrypt && (!input_size || input_size > op->result_size ||
	    (op->mechanism == CKM_RSA_PKCS_OAEP && input_size != op->result_size))) ||
	    (!decrypt && input_size > op->input_size)) { rv = decrypt ? CKR_ENCRYPTED_DATA_LEN_RANGE : CKR_DATA_LEN_RANGE; goto out; }
	size = op->result_size;
	if (!output) { *length = size; return CKR_OK; }
	if (*length < size) { *length = size; return CKR_BUFFER_TOO_SMALL; }
	if (op->mechanism == CKM_RSA_X_509) {
		memset(op->input, 0, op->result_size); if (input_size) memcpy(op->input + op->result_size - input_size, input, input_size);
		input = op->input; input_size = op->result_size;
	}
	ERR_clear_error();
	result = decrypt ? EVP_PKEY_decrypt(op->pkey, output, &size, input, input_size) : EVP_PKEY_encrypt(op->pkey, output, &size, input, input_size);
	if (result > 0 && size <= op->result_size) { *length = size; rv = CKR_OK; }
	else {
		unsigned long error = ERR_peek_last_error();
		rv = decrypt && ERR_GET_LIB(error) == ERR_LIB_RSA &&
			(ERR_GET_REASON(error) == RSA_R_OAEP_DECODING_ERROR || ERR_GET_REASON(error) == RSA_R_DATA_TOO_LARGE_FOR_MODULUS) ?
			CKR_ENCRYPTED_DATA_INVALID : CKR_DEVICE_ERROR;
		OPENSSL_cleanse(output, op->result_size); *length = 0;
	}
out:
	p11_operation_clear(op); return rv;
}

CK_RV C_Encrypt(CK_SESSION_HANDLE handle, CK_BYTE_PTR input, CK_ULONG input_size, CK_BYTE_PTR output, CK_ULONG_PTR length)
{
	struct p11_session *s; CK_RV rv = p11_session_begin(handle, &s); if (rv) return rv;
	return p11_session_end(s, cipher_run(&s->encrypt, input, input_size, output, length, 0));
}

CK_RV C_Decrypt(CK_SESSION_HANDLE handle, CK_BYTE_PTR input, CK_ULONG input_size, CK_BYTE_PTR output, CK_ULONG_PTR length)
{
	struct p11_session *s; CK_RV rv = p11_session_begin(handle, &s); if (rv) return rv;
	return p11_session_end(s, cipher_run(&s->decrypt, input, input_size, output, length, 1));
}

static CK_RV cipher_multipart(CK_SESSION_HANDLE handle, CK_ULONG_PTR length, int decrypt)
{
	struct p11_session *s; struct p11_operation *op; CK_RV rv = p11_session_begin(handle, &s); if (rv) return rv;
	op = decrypt ? &s->decrypt : &s->encrypt;
	rv = op->active ? CKR_FUNCTION_NOT_SUPPORTED : CKR_OPERATION_NOT_INITIALIZED;
	if (length) *length = 0;
	p11_operation_clear(op); return p11_session_end(s, rv);
}

CK_RV C_EncryptUpdate(CK_SESSION_HANDLE handle, CK_BYTE_PTR input, CK_ULONG size, CK_BYTE_PTR output, CK_ULONG_PTR length)
{
	(void)input; (void)size; (void)output; return cipher_multipart(handle, length, 0);
}
CK_RV C_DecryptUpdate(CK_SESSION_HANDLE handle, CK_BYTE_PTR input, CK_ULONG size, CK_BYTE_PTR output, CK_ULONG_PTR length)
{
	(void)input; (void)size; (void)output; return cipher_multipart(handle, length, 1);
}
CK_RV C_EncryptFinal(CK_SESSION_HANDLE handle, CK_BYTE_PTR output, CK_ULONG_PTR length)
{
	(void)output; return cipher_multipart(handle, length, 0);
}
CK_RV C_DecryptFinal(CK_SESSION_HANDLE handle, CK_BYTE_PTR output, CK_ULONG_PTR length)
{
	(void)output; return cipher_multipart(handle, length, 1);
}
