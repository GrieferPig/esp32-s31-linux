/* SPDX-License-Identifier: Apache-2.0 */
#include "rsa.h"
#include <openssl/x509.h>
#include <limits.h>

struct rsa_op {
	struct s31_provider *provider;
	struct s31_rsa_key *key;
	EVP_MD *md, *mgf;
	EVP_MD_CTX *hash;
	char *properties;
	unsigned char *label;
	size_t label_size;
	int padding, saltlen, private, implicit_rejection;
};
static void *op_new(void *provider, const char *properties)
{
	struct rsa_op *ctx=OPENSSL_zalloc(sizeof(*ctx));
	if(!ctx)return NULL;
	ctx->provider=provider;ctx->padding=RSA_PKCS1_PADDING;ctx->saltlen=RSA_PSS_SALTLEN_AUTO_DIGEST_MAX;ctx->implicit_rejection=1;
	if(properties && !(ctx->properties=OPENSSL_strdup(properties))){OPENSSL_free(ctx);return NULL;}return ctx;
}
static void *cipher_new(void *provider){return op_new(provider,NULL);}
static void op_free(void *opaque)
{
	struct rsa_op *ctx=opaque;if(!ctx)return;
	s31_rsa_free(ctx->key);EVP_MD_free(ctx->md);EVP_MD_free(ctx->mgf);EVP_MD_CTX_free(ctx->hash);
	OPENSSL_free(ctx->properties);OPENSSL_clear_free(ctx->label,ctx->label_size);OPENSSL_clear_free(ctx,sizeof(*ctx));
}
static void *op_copy(void *opaque)
{
	struct rsa_op *ctx=opaque,*copy=OPENSSL_zalloc(sizeof(*copy));if(!copy)return NULL;
	*copy=*ctx;copy->key=NULL;copy->md=copy->mgf=NULL;copy->hash=NULL;copy->label=NULL;copy->properties=NULL;
	copy->key=s31_rsa_ref(ctx->key);
	if(ctx->md){if(!EVP_MD_up_ref(ctx->md))goto fail;copy->md=ctx->md;}
	if(ctx->mgf){if(!EVP_MD_up_ref(ctx->mgf))goto fail;copy->mgf=ctx->mgf;}
	if(ctx->hash && (!(copy->hash=EVP_MD_CTX_new()) || !EVP_MD_CTX_copy_ex(copy->hash,ctx->hash)))goto fail;
	if(ctx->properties && !(copy->properties=OPENSSL_strdup(ctx->properties)))goto fail;
	if(ctx->label_size && !(copy->label=OPENSSL_memdup(ctx->label,ctx->label_size)))goto fail;
	return copy;
fail:op_free(copy);return NULL;
}
static int key_init(struct rsa_op *ctx,void *key,int private)
{
	struct s31_rsa_key *rsa=key?key:ctx->key;
	if(!rsa || !rsa->math || (private && !rsa->private))return 0;
	s31_rsa_ref(rsa);s31_rsa_free(ctx->key);ctx->key=rsa;ctx->private=private;
	ctx->padding=RSA_PKCS1_PADDING;ctx->saltlen=RSA_PSS_SALTLEN_AUTO_DIGEST_MAX;
	EVP_MD_CTX_free(ctx->hash);ctx->hash=NULL;return 1;
}
static int set_digest(struct rsa_op *ctx,EVP_MD **slot,const char *name,const char *properties)
{
	EVP_MD *md=EVP_MD_fetch(ctx->provider->libctx,name,properties?properties:ctx->properties);
	if(!md)return 0;
	if(EVP_MD_get_size(md)<=0 || EVP_MD_get_size(md)>EVP_MAX_MD_SIZE || (EVP_MD_get_flags(md)&EVP_MD_FLAG_XOF)) {EVP_MD_free(md);return 0;}
	if(slot==&ctx->md && ctx->hash && (!ctx->md || !EVP_MD_is_a(ctx->md,name) || properties)) {EVP_MD_free(md);return 0;}
	EVP_MD_free(*slot);*slot=md;return 1;
}
static int get_text(const OSSL_PARAM *p,const char **out){return OSSL_PARAM_get_utf8_string_ptr(p,out);}
static int padding_value(const OSSL_PARAM *p,int signature,int *padding)
{
	const char *s;
	if(p->data_type!=OSSL_PARAM_UTF8_STRING)return OSSL_PARAM_get_int(p,padding);
	if(!get_text(p,&s))return 0;
	if(!strcmp(s,"pkcs1"))*padding=RSA_PKCS1_PADDING;
	else if(!strcmp(s,"none"))*padding=RSA_NO_PADDING;
	else if(signature && !strcmp(s,"pss"))*padding=RSA_PKCS1_PSS_PADDING;
	else if(!signature && !strcmp(s,"oaep"))*padding=RSA_PKCS1_OAEP_PADDING;
	else return 0;
	return 1;
}
static const char *padding_name(int padding)
{
	switch(padding){case RSA_PKCS1_PADDING:return "pkcs1";case RSA_PKCS1_PSS_PADDING:return "pss";case RSA_PKCS1_OAEP_PADDING:return "oaep";case RSA_NO_PADDING:return "none";default:return "";}
}
static int digest_parameters(struct rsa_op *ctx,const OSSL_PARAM params[],int signature)
{
	const OSSL_PARAM *p;const char *name=NULL,*properties=NULL;
	const char *digest=signature?OSSL_SIGNATURE_PARAM_DIGEST:OSSL_ASYM_CIPHER_PARAM_OAEP_DIGEST;
	const char *props=signature?OSSL_SIGNATURE_PARAM_PROPERTIES:OSSL_ASYM_CIPHER_PARAM_OAEP_DIGEST_PROPS;
	if((p=OSSL_PARAM_locate_const(params,props)) && !get_text(p,&properties))return 0;
	if((p=OSSL_PARAM_locate_const(params,digest)) && !get_text(p,&name))return 0;
	if(name || properties){
		if(!name)name=ctx->md?EVP_MD_get0_name(ctx->md):(signature?"SHA256":"SHA1");
		if(!set_digest(ctx,&ctx->md,name,properties))return 0;
	}
	name=properties=NULL;
	if((p=OSSL_PARAM_locate_const(params,OSSL_PKEY_PARAM_MGF1_PROPERTIES)) && !get_text(p,&properties))return 0;
	if((p=OSSL_PARAM_locate_const(params,OSSL_PKEY_PARAM_MGF1_DIGEST)) && !get_text(p,&name))return 0;
	if(name || properties){
		if(!name)name=ctx->mgf?EVP_MD_get0_name(ctx->mgf):(ctx->md?EVP_MD_get0_name(ctx->md):(signature?"SHA256":"SHA1"));
		if(!set_digest(ctx,&ctx->mgf,name,properties))return 0;
	}
	return 1;
}
static int signature_set(void *opaque,const OSSL_PARAM params[])
{
	struct rsa_op *ctx=opaque;const OSSL_PARAM *p;int value;const char *name;
	if(!params)return 1;
	if((p=OSSL_PARAM_locate_const(params,OSSL_SIGNATURE_PARAM_PAD_MODE))){
		if(!padding_value(p,1,&value) || (value!=RSA_PKCS1_PADDING && value!=RSA_PKCS1_PSS_PADDING && value!=RSA_NO_PADDING))return 0;
		ctx->padding=value;
	}
	if(!digest_parameters(ctx,params,1))return 0;
	if(ctx->padding==RSA_NO_PADDING && ctx->md)return 0;
	if((p=OSSL_PARAM_locate_const(params,OSSL_SIGNATURE_PARAM_PSS_SALTLEN))){
		if(p->data_type==OSSL_PARAM_UTF8_STRING){
			if(!get_text(p,&name))return 0;
			if(!strcmp(name,"digest"))value=RSA_PSS_SALTLEN_DIGEST;
			else if(!strcmp(name,"max"))value=RSA_PSS_SALTLEN_MAX;
			else if(!strcmp(name,"auto"))value=RSA_PSS_SALTLEN_AUTO;
			else if(!strcmp(name,"auto-digestmax"))value=RSA_PSS_SALTLEN_AUTO_DIGEST_MAX;
			else{char *end;long parsed;errno=0;parsed=strtol(name,&end,10);if(errno || !*name || *end || parsed>INT_MAX || parsed<INT_MIN)return 0;value=parsed;}
		}else if(!OSSL_PARAM_get_int(p,&value))return 0;
		if(value<RSA_PSS_SALTLEN_AUTO_DIGEST_MAX)return 0;
		ctx->saltlen=value;
	}
	return 1;
}
static int sign_init(void *opaque,void *key,const OSSL_PARAM params[])
{
	struct rsa_op *ctx=opaque;return key_init(ctx,key,1)&&signature_set(ctx,params);
}
static int verify_init(void *opaque,void *key,const OSSL_PARAM params[])
{
	struct rsa_op *ctx=opaque;return key_init(ctx,key,0)&&signature_set(ctx,params);
}
static int sign_hash(void *opaque,unsigned char *signature,size_t *length,size_t capacity,const unsigned char *digest,size_t size)
{
	struct rsa_op *ctx=opaque;unsigned char encoded[512];unsigned int produced;int ret=0,n;
	*length=0;if(!ctx->key || !ctx->private)return 0;
	n=RSA_size(ctx->key->math);if(!signature){*length=n;return 1;}
	if(capacity<(size_t)n || size>INT_MAX)return 0;
	if(ctx->md && size!=(size_t)EVP_MD_get_size(ctx->md))return 0;
	if(ctx->padding==RSA_PKCS1_PSS_PADDING){
		if(!ctx->md || !RSA_padding_add_PKCS1_PSS_mgf1(ctx->key->math,encoded,digest,ctx->md,ctx->mgf,ctx->saltlen))goto out;
		ret=RSA_private_encrypt(n,encoded,signature,ctx->key->math,RSA_NO_PADDING);
		if(ret==n){*length=n;ret=1;}else ret=0;
	}else if(ctx->padding==RSA_PKCS1_PADDING && ctx->md){
		ret=RSA_sign(EVP_MD_get_type(ctx->md),digest,size,signature,&produced,ctx->key->math);
		if(ret)*length=produced;
	}else{
		ret=RSA_private_encrypt(size,digest,signature,ctx->key->math,ctx->padding);
		if(ret>=0){*length=ret;ret=1;}else ret=0;
	}
out:OPENSSL_cleanse(encoded,sizeof(encoded));if(!ret)OPENSSL_cleanse(signature,n);return ret;
}
static int verify_hash(void *opaque,const unsigned char *signature,size_t siglen,const unsigned char *digest,size_t size)
{
	struct rsa_op *ctx=opaque;unsigned char encoded[512];int ret=0,n,marked;
	if(!ctx->key || siglen>INT_MAX || size>INT_MAX)return 0;
	n=RSA_size(ctx->key->math);if(siglen!=(size_t)n || (ctx->md && size!=(size_t)EVP_MD_get_size(ctx->md)))return 0;
	/* RSA_verify folds callback errors into zero. Only errors added by this
	 * operation may distinguish a backend failure from a bad signature.
	 * OpenSSL's existing thread-local error queue also covers PSS hashes.
	 */
	marked=ERR_set_mark();
	if(ctx->padding==RSA_PKCS1_PSS_PADDING){
		if(!ctx->md || RSA_public_decrypt(siglen,signature,encoded,ctx->key->math,RSA_NO_PADDING)!=n)goto out;
		ret=RSA_verify_PKCS1_PSS_mgf1(ctx->key->math,digest,ctx->md,ctx->mgf,encoded,ctx->saltlen);
	}else if(ctx->padding==RSA_PKCS1_PADDING && ctx->md){
		ret=RSA_verify(EVP_MD_get_type(ctx->md),digest,size,signature,siglen,ctx->key->math);
	}else{
		n=RSA_public_decrypt(siglen,signature,encoded,ctx->key->math,ctx->padding);
		ret=n>=0 && (size_t)n==size && !CRYPTO_memcmp(encoded,digest,size);
	}
out:
	if(!ret && ERR_count_to_mark()>0){
		unsigned long error=ERR_peek_last_error();
		if((ERR_GET_LIB(error)==ERR_LIB_USER && ERR_GET_REASON(error)==ERR_R_OPERATION_FAIL) ||
		   ERR_GET_REASON(error)==ERR_R_MALLOC_FAILURE)ret=-1;
	}
	if(marked)ERR_clear_last_mark();
	OPENSSL_cleanse(encoded,sizeof(encoded));return ret;
}
static int digest_init(struct rsa_op *ctx,const char *name,void *key,const OSSL_PARAM params[],int sign)
{
	if(!key_init(ctx,key,sign) || !set_digest(ctx,&ctx->md,name?name:(ctx->md?EVP_MD_get0_name(ctx->md):"SHA256"),NULL) || !signature_set(ctx,params))return 0;
	ctx->hash=EVP_MD_CTX_new();return ctx->hash && EVP_DigestInit_ex2(ctx->hash,ctx->md,NULL);
}
static int digest_sign_init(void *ctx,const char *name,void *key,const OSSL_PARAM params[]){return digest_init(ctx,name,key,params,1);}
static int digest_verify_init(void *ctx,const char *name,void *key,const OSSL_PARAM params[]){return digest_init(ctx,name,key,params,0);}
static int digest_update(void *opaque,const unsigned char *input,size_t size){struct rsa_op *ctx=opaque;return ctx->hash && EVP_DigestUpdate(ctx->hash,input,size);}
static int final_hash(struct rsa_op *ctx,unsigned char *digest,unsigned *size)
{
	EVP_MD_CTX *copy=EVP_MD_CTX_new();int ret=copy&&ctx->hash&&EVP_MD_CTX_copy_ex(copy,ctx->hash)&&EVP_DigestFinal_ex(copy,digest,size);EVP_MD_CTX_free(copy);return ret;
}
static int digest_sign_final(void *opaque,unsigned char *out,size_t *length,size_t capacity)
{
	struct rsa_op *ctx=opaque;unsigned char digest[EVP_MAX_MD_SIZE];unsigned size;int ret;
	if(!out)return sign_hash(ctx,NULL,length,capacity,NULL,0);
	ret=final_hash(ctx,digest,&size)&&sign_hash(ctx,out,length,capacity,digest,size);OPENSSL_cleanse(digest,sizeof(digest));return ret;
}
static int digest_verify_final(void *opaque,const unsigned char *signature,size_t siglen)
{
	struct rsa_op *ctx=opaque;unsigned char digest[EVP_MAX_MD_SIZE];unsigned size;int ret;
	ret=final_hash(ctx,digest,&size)?verify_hash(ctx,signature,siglen,digest,size):-1;OPENSSL_cleanse(digest,sizeof(digest));return ret;
}
static int digest_sign(void *opaque,unsigned char *out,size_t *length,size_t capacity,const unsigned char *input,size_t size)
{
	struct rsa_op *ctx=opaque;unsigned char digest[EVP_MAX_MD_SIZE];unsigned len;int ret;
	if(!out)return sign_hash(ctx,NULL,length,capacity,NULL,0);
	ret=ctx->md && EVP_Digest(input,size,digest,&len,ctx->md,NULL)&&sign_hash(ctx,out,length,capacity,digest,len);OPENSSL_cleanse(digest,sizeof(digest));return ret;
}
static int digest_verify(void *opaque,const unsigned char *signature,size_t siglen,const unsigned char *input,size_t size)
{
	struct rsa_op *ctx=opaque;unsigned char digest[EVP_MAX_MD_SIZE];unsigned len;int ret;
	ret=ctx->md && EVP_Digest(input,size,digest,&len,ctx->md,NULL)?verify_hash(ctx,signature,siglen,digest,len):-1;OPENSSL_cleanse(digest,sizeof(digest));return ret;
}
static int signature_algid(struct rsa_op *ctx,OSSL_PARAM *p)
{
	X509_ALGOR *alg=NULL,*mgfhash=NULL;RSA_PSS_PARAMS *pss=NULL;ASN1_STRING *sequence=NULL;
	unsigned char *encoded=NULL,*inner=NULL;int ret=0,length,innerlen,nid,salt,maxsalt;
	if(!ctx->md || !ctx->key)return 0;
	alg=X509_ALGOR_new();if(!alg)goto out;
	if(ctx->padding==RSA_PKCS1_PADDING){
		if(!OBJ_find_sigid_by_algs(&nid,EVP_MD_get_type(ctx->md),EVP_PKEY_RSA) || !X509_ALGOR_set0(alg,OBJ_nid2obj(nid),V_ASN1_NULL,NULL))goto out;
	}else if(ctx->padding==RSA_PKCS1_PSS_PADDING){
		pss=RSA_PSS_PARAMS_new();mgfhash=X509_ALGOR_new();if(!pss || !mgfhash)goto out;
		pss->hashAlgorithm=X509_ALGOR_new();pss->maskGenAlgorithm=X509_ALGOR_new();pss->saltLength=ASN1_INTEGER_new();
		if(!pss->hashAlgorithm || !pss->maskGenAlgorithm || !pss->saltLength)goto out;
		X509_ALGOR_set_md(pss->hashAlgorithm,ctx->md);X509_ALGOR_set_md(mgfhash,ctx->mgf?ctx->mgf:ctx->md);
		innerlen=i2d_X509_ALGOR(mgfhash,&inner);sequence=ASN1_STRING_type_new(V_ASN1_SEQUENCE);
		if(innerlen<=0 || !sequence || !ASN1_STRING_set(sequence,inner,innerlen) || !X509_ALGOR_set0(pss->maskGenAlgorithm,OBJ_nid2obj(NID_mgf1),V_ASN1_SEQUENCE,sequence))goto out;
		sequence=NULL;OPENSSL_free(inner);inner=NULL;
		maxsalt=(RSA_bits(ctx->key->math)-1+7)/8-EVP_MD_get_size(ctx->md)-2;salt=ctx->saltlen;
		if(salt==RSA_PSS_SALTLEN_DIGEST)salt=EVP_MD_get_size(ctx->md);
		else if(salt==RSA_PSS_SALTLEN_AUTO || salt==RSA_PSS_SALTLEN_MAX)salt=maxsalt;
		else if(salt==RSA_PSS_SALTLEN_AUTO_DIGEST_MAX)salt=maxsalt<EVP_MD_get_size(ctx->md)?maxsalt:EVP_MD_get_size(ctx->md);
		if(salt<0 || salt>maxsalt || !ASN1_INTEGER_set(pss->saltLength,salt))goto out;
		innerlen=i2d_RSA_PSS_PARAMS(pss,&inner);sequence=ASN1_STRING_type_new(V_ASN1_SEQUENCE);
		if(innerlen<=0 || !sequence || !ASN1_STRING_set(sequence,inner,innerlen) || !X509_ALGOR_set0(alg,OBJ_nid2obj(NID_rsassaPss),V_ASN1_SEQUENCE,sequence))goto out;
		sequence=NULL;
	}else goto out;
	length=i2d_X509_ALGOR(alg,&encoded);ret=length>0 && OSSL_PARAM_set_octet_string(p,encoded,length);
out:X509_ALGOR_free(alg);X509_ALGOR_free(mgfhash);RSA_PSS_PARAMS_free(pss);ASN1_STRING_free(sequence);OPENSSL_free(inner);OPENSSL_free(encoded);return ret;
}
static int padding_get(OSSL_PARAM *p,int padding){return p->data_type==OSSL_PARAM_UTF8_STRING?OSSL_PARAM_set_utf8_string(p,padding_name(padding)):OSSL_PARAM_set_int(p,padding);}
static int signature_get(void *opaque,OSSL_PARAM params[])
{
	struct rsa_op *ctx=opaque;OSSL_PARAM *p;
	if((p=OSSL_PARAM_locate(params,OSSL_SIGNATURE_PARAM_PAD_MODE))&&!padding_get(p,ctx->padding))return 0;
	if((p=OSSL_PARAM_locate(params,OSSL_SIGNATURE_PARAM_DIGEST))&&!OSSL_PARAM_set_utf8_string(p,ctx->md?EVP_MD_get0_name(ctx->md):""))return 0;
	if((p=OSSL_PARAM_locate(params,OSSL_SIGNATURE_PARAM_MGF1_DIGEST))&&!OSSL_PARAM_set_utf8_string(p,ctx->mgf?EVP_MD_get0_name(ctx->mgf):(ctx->md?EVP_MD_get0_name(ctx->md):"")))return 0;
	if((p=OSSL_PARAM_locate(params,OSSL_SIGNATURE_PARAM_DIGEST_SIZE))&&!OSSL_PARAM_set_size_t(p,ctx->md?EVP_MD_get_size(ctx->md):0))return 0;
	if((p=OSSL_PARAM_locate(params,OSSL_SIGNATURE_PARAM_PSS_SALTLEN))){
		if(p->data_type==OSSL_PARAM_UTF8_STRING){char s[24];const char *v=NULL;
			switch(ctx->saltlen){case -1:v="digest";break;case -2:v="auto";break;case -3:v="max";break;case -4:v="auto-digestmax";break;default:snprintf(s,sizeof(s),"%d",ctx->saltlen);v=s;}
			if(!OSSL_PARAM_set_utf8_string(p,v))return 0;
		}else if(!OSSL_PARAM_set_int(p,ctx->saltlen))return 0;
	}
	if((p=OSSL_PARAM_locate(params,OSSL_SIGNATURE_PARAM_ALGORITHM_ID))&&!signature_algid(ctx,p))return 0;
	return 1;
}
static const OSSL_PARAM *signature_gettable(void *op,void *provider)
{
	static const OSSL_PARAM p[]={OSSL_PARAM_utf8_string(OSSL_SIGNATURE_PARAM_PAD_MODE,NULL,0),OSSL_PARAM_utf8_string(OSSL_SIGNATURE_PARAM_DIGEST,NULL,0),OSSL_PARAM_size_t(OSSL_SIGNATURE_PARAM_DIGEST_SIZE,NULL),OSSL_PARAM_utf8_string(OSSL_SIGNATURE_PARAM_MGF1_DIGEST,NULL,0),OSSL_PARAM_int(OSSL_SIGNATURE_PARAM_PSS_SALTLEN,NULL),OSSL_PARAM_octet_string(OSSL_SIGNATURE_PARAM_ALGORITHM_ID,NULL,0),OSSL_PARAM_END};(void)op;(void)provider;return p;
}
static const OSSL_PARAM *signature_settable(void *op,void *provider)
{
	static const OSSL_PARAM p[]={OSSL_PARAM_utf8_string(OSSL_SIGNATURE_PARAM_PAD_MODE,NULL,0),OSSL_PARAM_utf8_string(OSSL_SIGNATURE_PARAM_DIGEST,NULL,0),OSSL_PARAM_utf8_string(OSSL_SIGNATURE_PARAM_PROPERTIES,NULL,0),OSSL_PARAM_utf8_string(OSSL_SIGNATURE_PARAM_MGF1_DIGEST,NULL,0),OSSL_PARAM_utf8_string(OSSL_SIGNATURE_PARAM_MGF1_PROPERTIES,NULL,0),OSSL_PARAM_int(OSSL_SIGNATURE_PARAM_PSS_SALTLEN,NULL),OSSL_PARAM_END};(void)op;(void)provider;return p;
}
static int cipher_set(void *opaque,const OSSL_PARAM params[])
{
	struct rsa_op *ctx=opaque;const OSSL_PARAM *p;int value;unsigned implicit;
	if(!params)return 1;
	if((p=OSSL_PARAM_locate_const(params,OSSL_ASYM_CIPHER_PARAM_PAD_MODE))){
		if(!padding_value(p,0,&value) || (value!=RSA_PKCS1_PADDING && value!=RSA_PKCS1_OAEP_PADDING && value!=RSA_NO_PADDING))return 0;
		ctx->padding=value;
	}
	if(!digest_parameters(ctx,params,0))return 0;
	if((p=OSSL_PARAM_locate_const(params,OSSL_ASYM_CIPHER_PARAM_OAEP_LABEL))){
		unsigned char *copy=NULL;
		if(p->data_type!=OSSL_PARAM_OCTET_STRING || p->data_size>65536)return 0;
		if(p->data_size && !(copy=OPENSSL_memdup(p->data,p->data_size)))return 0;
		OPENSSL_clear_free(ctx->label,ctx->label_size);ctx->label=copy;ctx->label_size=p->data_size;
	}
	if((p=OSSL_PARAM_locate_const(params,OSSL_ASYM_CIPHER_PARAM_IMPLICIT_REJECTION))){
		if(!OSSL_PARAM_get_uint(p,&implicit)||implicit>1)return 0;
		ctx->implicit_rejection=implicit;
	}
	if(OSSL_PARAM_locate_const(params,OSSL_ASYM_CIPHER_PARAM_TLS_CLIENT_VERSION) || OSSL_PARAM_locate_const(params,OSSL_ASYM_CIPHER_PARAM_TLS_NEGOTIATED_VERSION))return 0;
	return 1;
}
static int encrypt_init(void *opaque,void *key,const OSSL_PARAM params[]){struct rsa_op *ctx=opaque;ctx->implicit_rejection=1;return key_init(ctx,key,0)&&cipher_set(ctx,params);}
static int decrypt_init(void *opaque,void *key,const OSSL_PARAM params[])
{
	struct rsa_op *ctx=opaque;struct s31_rsa_key *rsa=key?key:ctx->key;
	if(rsa && rsa->dsdata){s31_rsa_free(ctx->key);ctx->key=NULL;return 0;}
	ctx->implicit_rejection=1;return key_init(ctx,key,1)&&cipher_set(ctx,params);
}
static int cipher_crypt(struct rsa_op *ctx,unsigned char *out,size_t *length,size_t capacity,const unsigned char *input,size_t size)
{
	unsigned char encoded[512];int bytes,ret=-1,padding=ctx->padding;*length=0;
	if(!ctx->key || size>INT_MAX)return 0;
	bytes=RSA_size(ctx->key->math);
	if(!out){*length=bytes;return 1;}if(capacity<(size_t)bytes)return 0;
	if(padding==RSA_PKCS1_OAEP_PADDING){
		if(!ctx->md && !set_digest(ctx,&ctx->md,"SHA1","provider=default"))goto end;
		if(ctx->private){
			ret=RSA_private_decrypt(size,input,encoded,ctx->key->math,RSA_NO_PADDING);
			if(ret==bytes)ret=RSA_padding_check_PKCS1_OAEP_mgf1(out,bytes,encoded,bytes,bytes,ctx->label,ctx->label_size,ctx->md,ctx->mgf);
			else ret=-1;
		}else if(RSA_padding_add_PKCS1_OAEP_mgf1(encoded,bytes,input,size,ctx->label,ctx->label_size,ctx->md,ctx->mgf))ret=RSA_public_encrypt(bytes,encoded,out,ctx->key->math,RSA_NO_PADDING);
	}else if(ctx->private){
		if(padding==RSA_PKCS1_PADDING && !ctx->implicit_rejection)padding=RSA_PKCS1_NO_IMPLICIT_REJECT_PADDING;
		ret=RSA_private_decrypt(size,input,out,ctx->key->math,padding);
	}else ret=RSA_public_encrypt(size,input,out,ctx->key->math,padding);
end:OPENSSL_cleanse(encoded,sizeof(encoded));if(ret<0){OPENSSL_cleanse(out,bytes);return 0;}*length=ret;return 1;
}
static int rsa_cipher_crypt(void *ctx,unsigned char *out,size_t *length,size_t capacity,const unsigned char *input,size_t size){return cipher_crypt(ctx,out,length,capacity,input,size);}
static int cipher_get(void *opaque,OSSL_PARAM params[])
{
	struct rsa_op *ctx=opaque;OSSL_PARAM *p;
	if((p=OSSL_PARAM_locate(params,OSSL_ASYM_CIPHER_PARAM_PAD_MODE))&&!padding_get(p,ctx->padding))return 0;
	if((p=OSSL_PARAM_locate(params,OSSL_ASYM_CIPHER_PARAM_OAEP_DIGEST))&&!OSSL_PARAM_set_utf8_string(p,ctx->md?EVP_MD_get0_name(ctx->md):"SHA1"))return 0;
	if((p=OSSL_PARAM_locate(params,OSSL_ASYM_CIPHER_PARAM_MGF1_DIGEST))&&!OSSL_PARAM_set_utf8_string(p,ctx->mgf?EVP_MD_get0_name(ctx->mgf):(ctx->md?EVP_MD_get0_name(ctx->md):"SHA1")))return 0;
	if((p=OSSL_PARAM_locate(params,OSSL_ASYM_CIPHER_PARAM_IMPLICIT_REJECTION))&&!OSSL_PARAM_set_uint(p,ctx->implicit_rejection))return 0;
	if((p=OSSL_PARAM_locate(params,OSSL_ASYM_CIPHER_PARAM_OAEP_LABEL))){
		if(p->data_type==OSSL_PARAM_OCTET_PTR){if(!OSSL_PARAM_set_octet_ptr(p,ctx->label,ctx->label_size))return 0;}
		else if(!OSSL_PARAM_set_octet_string(p,ctx->label,ctx->label_size))return 0;
	}return 1;
}
static const OSSL_PARAM *cipher_gettable(void *op,void *provider)
{
	static const OSSL_PARAM p[]={OSSL_PARAM_utf8_string(OSSL_ASYM_CIPHER_PARAM_PAD_MODE,NULL,0),OSSL_PARAM_utf8_string(OSSL_ASYM_CIPHER_PARAM_OAEP_DIGEST,NULL,0),OSSL_PARAM_utf8_string(OSSL_ASYM_CIPHER_PARAM_MGF1_DIGEST,NULL,0),OSSL_PARAM_octet_ptr(OSSL_ASYM_CIPHER_PARAM_OAEP_LABEL,NULL,0),OSSL_PARAM_uint(OSSL_ASYM_CIPHER_PARAM_IMPLICIT_REJECTION,NULL),OSSL_PARAM_END};(void)op;(void)provider;return p;
}
static const OSSL_PARAM *cipher_settable(void *op,void *provider)
{
	static const OSSL_PARAM p[]={OSSL_PARAM_utf8_string(OSSL_ASYM_CIPHER_PARAM_PAD_MODE,NULL,0),OSSL_PARAM_utf8_string(OSSL_ASYM_CIPHER_PARAM_OAEP_DIGEST,NULL,0),OSSL_PARAM_utf8_string(OSSL_ASYM_CIPHER_PARAM_OAEP_DIGEST_PROPS,NULL,0),OSSL_PARAM_utf8_string(OSSL_ASYM_CIPHER_PARAM_MGF1_DIGEST,NULL,0),OSSL_PARAM_utf8_string(OSSL_ASYM_CIPHER_PARAM_MGF1_DIGEST_PROPS,NULL,0),OSSL_PARAM_octet_string(OSSL_ASYM_CIPHER_PARAM_OAEP_LABEL,NULL,0),OSSL_PARAM_uint(OSSL_ASYM_CIPHER_PARAM_IMPLICIT_REJECTION,NULL),OSSL_PARAM_END};(void)op;(void)provider;return p;
}
#define E(name,fn) {OSSL_FUNC_##name,(void(*)(void))fn}
const OSSL_DISPATCH s31_rsa_signature[]={
 E(SIGNATURE_NEWCTX,op_new),E(SIGNATURE_FREECTX,op_free),E(SIGNATURE_DUPCTX,op_copy),E(SIGNATURE_SIGN_INIT,sign_init),E(SIGNATURE_VERIFY_INIT,verify_init),
 E(SIGNATURE_SIGN,sign_hash),E(SIGNATURE_VERIFY,verify_hash),E(SIGNATURE_DIGEST_SIGN_INIT,digest_sign_init),E(SIGNATURE_DIGEST_VERIFY_INIT,digest_verify_init),
 E(SIGNATURE_DIGEST_SIGN_UPDATE,digest_update),E(SIGNATURE_DIGEST_VERIFY_UPDATE,digest_update),E(SIGNATURE_DIGEST_SIGN_FINAL,digest_sign_final),E(SIGNATURE_DIGEST_VERIFY_FINAL,digest_verify_final),
 E(SIGNATURE_DIGEST_SIGN,digest_sign),E(SIGNATURE_DIGEST_VERIFY,digest_verify),E(SIGNATURE_GET_CTX_PARAMS,signature_get),E(SIGNATURE_GETTABLE_CTX_PARAMS,signature_gettable),
 E(SIGNATURE_SET_CTX_PARAMS,signature_set),E(SIGNATURE_SETTABLE_CTX_PARAMS,signature_settable),{0,NULL}};
const OSSL_DISPATCH s31_rsa_cipher[]={
 E(ASYM_CIPHER_NEWCTX,cipher_new),E(ASYM_CIPHER_FREECTX,op_free),E(ASYM_CIPHER_DUPCTX,op_copy),E(ASYM_CIPHER_ENCRYPT_INIT,encrypt_init),E(ASYM_CIPHER_DECRYPT_INIT,decrypt_init),
 E(ASYM_CIPHER_ENCRYPT,rsa_cipher_crypt),E(ASYM_CIPHER_DECRYPT,rsa_cipher_crypt),E(ASYM_CIPHER_GET_CTX_PARAMS,cipher_get),E(ASYM_CIPHER_GETTABLE_CTX_PARAMS,cipher_gettable),
 E(ASYM_CIPHER_SET_CTX_PARAMS,cipher_set),E(ASYM_CIPHER_SETTABLE_CTX_PARAMS,cipher_settable),{0,NULL}};
