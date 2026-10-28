/* SPDX-License-Identifier: Apache-2.0 */
#ifndef S31_P11_H
#define S31_P11_H
#define CK_PTR *
#define CK_DECLARE_FUNCTION(result, name) __attribute__((visibility("default"))) result name
#define CK_DECLARE_FUNCTION_POINTER(result, name) result (*name)
#define CK_CALLBACK_FUNCTION(result, name) result (*name)
#define NULL_PTR NULL
#include <stddef.h>
#include "pkcs11/pkcs11.h"
#include <openssl/evp.h>
#include <openssl/provider.h>
#include <openssl/core_names.h>
#include <openssl/param_build.h>
#include <openssl/store.h>
#include <openssl/rand.h>
#include <openssl/err.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <unistd.h>

#define P11_SESSIONS 16
#define P11_OBJECTS 64
#define P11_LABEL 64
#define P11_ID 64
#define P11_MAX_INPUT 65536

struct p11_object {
	struct p11_object *next;
	CK_OBJECT_HANDLE handle;
	CK_SESSION_HANDLE owner;
	CK_OBJECT_CLASS class;
	CK_KEY_TYPE type;
	atomic_uint references;
	int token, private, sensitive, extractable, hardware, slot, modifiable;
	int sign, verify, encrypt, decrypt, derive, deleted;
	unsigned char label[P11_LABEL], id[P11_ID];
	size_t label_size, id_size;
	EVP_PKEY *key;
	unsigned char *km_recovery;
	unsigned char *secret;
	size_t secret_size;
};

struct p11_operation {
	struct p11_object *object;
	CK_MECHANISM_TYPE mechanism;
	EVP_PKEY_CTX *pkey;
	EVP_MD_CTX *digest;
	EVP_MAC_CTX *mac;
	int active, multipart, raw;
	unsigned char input[512];
	size_t result_size, input_size;
};

struct p11_session {
	struct p11_session *next;
	CK_SESSION_HANDLE handle;
	CK_FLAGS flags;
	pthread_mutex_t lock;
	unsigned int users;
	int closing, finding;
	CK_OBJECT_HANDLE found[P11_OBJECTS];
	size_t found_count, found_position;
	struct p11_operation sign, verify, encrypt, decrypt;
	EVP_MD_CTX *digest;
	int digest_active, digest_multipart, digest_secret;
};

struct p11_state {
	pthread_mutex_t lock;
	pthread_cond_t idle;
	unsigned int users;
	int initialized, quiescing, logged_in;
	pid_t pid;
	CK_ULONG serial;
	struct p11_session *sessions;
	struct p11_object *objects;
	OSSL_LIB_CTX *libctx;
	OSSL_PROVIDER *standard, *native;
};
extern struct p11_state p11;

/* global_begin returns with registry locked and a lifecycle reference.
 * session_begin drops the registry before taking the independent session lock.
 * No registry lock is retained across crypto work or application update calls.
 * Cancellation is deferred while a call owns library state and delivered only
 * after its locks/references are released. The caller's disabled state survives.
 */
CK_RV p11_global_begin(void);
CK_RV p11_global_end(CK_RV result);
CK_RV p11_session_begin(CK_SESSION_HANDLE handle, struct p11_session **session);
CK_RV p11_session_end(struct p11_session *session, CK_RV result);
struct p11_session *p11_session_find(CK_SESSION_HANDLE handle);
struct p11_object *p11_object_find(CK_OBJECT_HANDLE handle);
struct p11_object *p11_object_ref(struct p11_object *object);
void p11_object_free(struct p11_object *object);
void p11_operation_clear(struct p11_operation *operation);
CK_RV p11_key_get(CK_OBJECT_HANDLE handle, struct p11_object **object);
CK_RV p11_private_check(struct p11_object *object);
CK_ULONG p11_handle(void);
void p11_fill(void *destination, size_t capacity, const char *text);
CK_RV p11_objects_load(void);
void p11_objects_clear(CK_SESSION_HANDLE owner);
CK_RV p11_attribute(struct p11_object *object, CK_ATTRIBUTE *attribute);
const CK_MECHANISM_TYPE *p11_mechanisms(size_t *count);
CK_RV p11_mechanism_info(CK_MECHANISM_TYPE mechanism, CK_MECHANISM_INFO *info);

#endif
