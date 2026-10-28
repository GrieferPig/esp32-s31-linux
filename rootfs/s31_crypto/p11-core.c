/* SPDX-License-Identifier: Apache-2.0 */
#include "p11.h"
#include <linux/esp32s31-crypto.h>
#include <sys/ioctl.h>
#include <fcntl.h>
#include <limits.h>
#include <string.h>

struct p11_state p11 = { .lock = PTHREAD_MUTEX_INITIALIZER, .idle = PTHREAD_COND_INITIALIZER };
static pthread_once_t fork_once = PTHREAD_ONCE_INIT;
static int fork_error;
static int exit_registered;
static int terminated;
static void module_close(void);
static _Thread_local unsigned cancel_depth;
static _Thread_local int cancel_state;

/* OpenSSL/provider I/O can be a cancellation point. A thread must not exit
 * while it owns a session mutex, a registry reference, or partial cleanup.
 * Keep this guard per thread; independent sessions still run concurrently.
 */
static void cancel_enter(void)
{
	int previous;
	pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, &previous);
	if (!cancel_depth++) cancel_state = previous;
}

static void cancel_leave(int deliver)
{
	int previous;
	if (--cancel_depth) return;
	previous = cancel_state;
	pthread_setcancelstate(previous, NULL);
	if (deliver && previous == PTHREAD_CANCEL_ENABLE) pthread_testcancel();
}

void p11_fill(void *destination, size_t size, const char *text)
{
	size_t length = strlen(text);
	memset(destination, ' ', size); memcpy(destination, text, length < size ? length : size);
}

CK_ULONG p11_handle(void)
{
	return p11.serial == ULONG_MAX ? CK_INVALID_HANDLE : ++p11.serial;
}

CK_RV p11_global_begin(void)
{
	cancel_enter();
	pthread_mutex_lock(&p11.lock);
	while (p11.quiescing && p11.pid == getpid()) pthread_cond_wait(&p11.idle, &p11.lock);
	if (!p11.initialized || p11.pid != getpid()) {
		pthread_mutex_unlock(&p11.lock); cancel_leave(1); return CKR_CRYPTOKI_NOT_INITIALIZED;
	}
	p11.users++; return CKR_OK;
}

CK_RV p11_global_end(CK_RV result)
{
	p11.users--; pthread_cond_broadcast(&p11.idle); pthread_mutex_unlock(&p11.lock);
	cancel_leave(1); return result;
}

struct p11_session *p11_session_find(CK_SESSION_HANDLE handle)
{
	for (struct p11_session *s = p11.sessions; s; s = s->next)
		if (s->handle == handle && !s->closing) return s;
	return NULL;
}

CK_RV p11_session_begin(CK_SESSION_HANDLE handle, struct p11_session **session)
{
	CK_RV rv = p11_global_begin();
	if (rv) return rv;
	*session = p11_session_find(handle);
	if (!*session) return p11_global_end(CKR_SESSION_HANDLE_INVALID);
	(*session)->users++; pthread_mutex_unlock(&p11.lock);
	pthread_mutex_lock(&(*session)->lock); return CKR_OK;
}

CK_RV p11_session_end(struct p11_session *session, CK_RV result)
{
	pthread_mutex_unlock(&session->lock); pthread_mutex_lock(&p11.lock);
	session->users--; return p11_global_end(result);
}

static void session_free(struct p11_session *session)
{
	p11_operation_clear(&session->sign); p11_operation_clear(&session->verify);
	p11_operation_clear(&session->encrypt); p11_operation_clear(&session->decrypt);
	EVP_MD_CTX_free(session->digest); pthread_mutex_destroy(&session->lock);
	OPENSSL_clear_free(session, sizeof(*session));
}

static void cleanup(void)
{
	struct p11_session *next;
	for (struct p11_session *s = p11.sessions; s; s = next) { next = s->next; session_free(s); }
	p11.sessions = NULL; p11_objects_clear(0);
	OSSL_PROVIDER_unload(p11.native); OSSL_PROVIDER_unload(p11.standard); OSSL_LIB_CTX_free(p11.libctx);
	p11.native = p11.standard = NULL; p11.libctx = NULL;
	p11.initialized = p11.logged_in = 0; p11.pid = 0;
}

static void fork_prepare(void)
{
	cancel_enter();
	pthread_mutex_lock(&p11.lock);
	while (p11.quiescing) pthread_cond_wait(&p11.idle, &p11.lock);
	p11.quiescing = 1;
	while (p11.users) pthread_cond_wait(&p11.idle, &p11.lock);
}

static void fork_parent(void)
{
	p11.quiescing = 0; pthread_cond_broadcast(&p11.idle); pthread_mutex_unlock(&p11.lock);
	/* The enclosing fork still owns libc/OpenSSL lifecycle state. Do not
	 * introduce a deferred cancellation point inside its callbacks.
	 */
	cancel_leave(0);
}

static void fork_child(void)
{
	/* Calls in the child must initialize again. Every session was quiescent
	 * before fork, so the copied state can then be safely discarded.
	 */
	p11.pid = 0; p11.quiescing = 0; p11.idle = (pthread_cond_t)PTHREAD_COND_INITIALIZER;
	pthread_mutex_unlock(&p11.lock);
	cancel_leave(0);
}

static void register_fork(void)
{
	/* Drain our calls before OpenSSL's prepare hook takes its own locks. */
	if (!OPENSSL_init_crypto(OPENSSL_INIT_ATFORK, NULL) ||
	    pthread_atfork(fork_prepare, fork_parent, fork_child)) fork_error = 1;
}

CK_RV C_Initialize(CK_VOID_PTR argument)
{
	CK_C_INITIALIZE_ARGS *args = argument; CK_RV rv = CKR_OK;
	unsigned supplied;
	if (args) {
		supplied = !!args->CreateMutex + !!args->DestroyMutex + !!args->LockMutex + !!args->UnlockMutex;
		if (args->pReserved || (supplied && supplied != 4) ||
		    (args->flags & ~(CKF_OS_LOCKING_OK | CKF_LIBRARY_CANT_CREATE_OS_THREADS))) return CKR_ARGUMENTS_BAD;
		if (supplied && !(args->flags & CKF_OS_LOCKING_OK)) return CKR_CANT_LOCK;
	}
	cancel_enter();
	pthread_once(&fork_once, register_fork);
	if (fork_error) { cancel_leave(1); return CKR_CANT_LOCK; }
	pthread_mutex_lock(&p11.lock);
	while (p11.quiescing) pthread_cond_wait(&p11.idle, &p11.lock);
	if (terminated) { rv = CKR_CRYPTOKI_NOT_INITIALIZED; goto out; }
	if (p11.initialized && p11.pid == getpid()) { rv = CKR_CRYPTOKI_ALREADY_INITIALIZED; goto out; }
	if (p11.initialized) cleanup();
	/* Fork handlers now protect this mutex and the registration state.
	 * A failed allocation must be retryable without duplicating a successful
	 * cleanup hook. No context or key is published before the hook exists.
	 */
	if (!exit_registered) {
		if (!OPENSSL_atexit(module_close)) { rv = CKR_HOST_MEMORY; goto out; }
		exit_registered = 1;
	}
	p11.libctx = OSSL_LIB_CTX_new();
	if (!p11.libctx) { rv = CKR_HOST_MEMORY; goto fail; }
	p11.standard = OSSL_PROVIDER_load(p11.libctx, "default");
	p11.native = OSSL_PROVIDER_load(p11.libctx, "s31");
	if (!p11.standard || !p11.native) { rv = CKR_DEVICE_ERROR; goto fail; }
	rv = p11_objects_load();
	if (rv) goto fail;
	p11.pid = getpid(); p11.initialized = 1; goto out;
fail:
	cleanup();
out:
	pthread_mutex_unlock(&p11.lock); cancel_leave(1); return rv;
}

CK_RV C_Finalize(CK_VOID_PTR reserved)
{
	CK_RV rv = p11_global_begin();
	if (rv) return rv;
	if (reserved) return p11_global_end(CKR_ARGUMENTS_BAD);
	p11.quiescing = 1;
	while (p11.users != 1) pthread_cond_wait(&p11.idle, &p11.lock);
	cleanup(); p11.quiescing = 0; return p11_global_end(CKR_OK);
}

CK_RV C_GetInfo(CK_INFO_PTR info)
{
	CK_RV rv = p11_global_begin(); if (rv) return rv;
	if (!info) return p11_global_end(CKR_ARGUMENTS_BAD);
	memset(info, 0, sizeof(*info)); info->cryptokiVersion = (CK_VERSION){2,40};
	info->libraryVersion = (CK_VERSION){1,0}; p11_fill(info->manufacturerID, 32, "GrieferPig");
	p11_fill(info->libraryDescription, 32, "ESP32-S31 native Linux crypto"); return p11_global_end(CKR_OK);
}

CK_RV C_GetSlotList(CK_BBOOL present, CK_SLOT_ID_PTR slots, CK_ULONG_PTR count)
{
	CK_RV rv = p11_global_begin(); (void)present; if (rv) return rv;
	if (!count) return p11_global_end(CKR_ARGUMENTS_BAD);
	rv = slots && *count < 1 ? CKR_BUFFER_TOO_SMALL : CKR_OK;
	if (slots && !rv) slots[0] = 1;
	*count = 1; return p11_global_end(rv);
}

CK_RV C_GetSlotInfo(CK_SLOT_ID slot, CK_SLOT_INFO_PTR info)
{
	CK_RV rv = p11_global_begin(); if (rv) return rv;
	if (slot != 1) return p11_global_end(CKR_SLOT_ID_INVALID);
	if (!info) return p11_global_end(CKR_ARGUMENTS_BAD);
	memset(info, 0, sizeof(*info)); info->flags = CKF_TOKEN_PRESENT | CKF_HW_SLOT;
	p11_fill(info->slotDescription, 64, "ESP32-S31 native crypto"); p11_fill(info->manufacturerID, 32, "GrieferPig");
	info->firmwareVersion = (CK_VERSION){1,0}; return p11_global_end(CKR_OK);
}

CK_RV C_GetTokenInfo(CK_SLOT_ID slot, CK_TOKEN_INFO_PTR info)
{
	CK_RV rv = p11_global_begin(); if (rv) return rv;
	if (slot != 1) return p11_global_end(CKR_SLOT_ID_INVALID);
	if (!info) return p11_global_end(CKR_ARGUMENTS_BAD);
	memset(info, 0, sizeof(*info)); p11_fill(info->label, 32, "S31 Linux crypto");
	p11_fill(info->manufacturerID, 32, "GrieferPig"); p11_fill(info->model, 16, "ESP32-S31");
	p11_fill(info->serialNumber, 16, "linux-native-0"); p11_fill(info->utcTime, 16, "");
	info->flags = CKF_RNG | CKF_LOGIN_REQUIRED | CKF_USER_PIN_INITIALIZED | CKF_TOKEN_INITIALIZED;
	info->ulMaxSessionCount = info->ulMaxRwSessionCount = P11_SESSIONS;
	for (struct p11_session *s = p11.sessions; s; s = s->next) {
		info->ulSessionCount++; if (s->flags & CKF_RW_SESSION) info->ulRwSessionCount++;
	}
	/* Authorization is the Linux device ACL. There is no separate token PIN. */
	info->ulMinPinLen = 0; info->ulMaxPinLen = 1;
	info->ulTotalPublicMemory = info->ulFreePublicMemory = CK_UNAVAILABLE_INFORMATION;
	info->ulTotalPrivateMemory = info->ulFreePrivateMemory = CK_UNAVAILABLE_INFORMATION;
	info->firmwareVersion = (CK_VERSION){1,0}; return p11_global_end(CKR_OK);
}

CK_RV C_GetMechanismList(CK_SLOT_ID slot, CK_MECHANISM_TYPE_PTR mechanisms, CK_ULONG_PTR count)
{
	CK_RV rv = p11_global_begin(); size_t size; const CK_MECHANISM_TYPE *list;
	if (rv) return rv;
	if (slot != 1) return p11_global_end(CKR_SLOT_ID_INVALID);
	if (!count) return p11_global_end(CKR_ARGUMENTS_BAD);
	list = p11_mechanisms(&size); rv = mechanisms && *count < size ? CKR_BUFFER_TOO_SMALL : CKR_OK;
	if (mechanisms && !rv) memcpy(mechanisms, list, size * sizeof(*list));
	*count = size; return p11_global_end(rv);
}

CK_RV C_GetMechanismInfo(CK_SLOT_ID slot, CK_MECHANISM_TYPE mechanism, CK_MECHANISM_INFO_PTR info)
{
	CK_RV rv = p11_global_begin(); if (rv) return rv;
	return p11_global_end(slot != 1 ? CKR_SLOT_ID_INVALID : !info ? CKR_ARGUMENTS_BAD : p11_mechanism_info(mechanism, info));
}

CK_RV C_OpenSession(CK_SLOT_ID slot, CK_FLAGS flags, CK_VOID_PTR application, CK_NOTIFY notify, CK_SESSION_HANDLE_PTR handle)
{
	struct p11_session *session; size_t count = 0; CK_RV rv = p11_global_begin();
	(void)application; (void)notify; if (rv) return rv;
	if (slot != 1) return p11_global_end(CKR_SLOT_ID_INVALID);
	if (!handle || (flags & ~(CKF_SERIAL_SESSION | CKF_RW_SESSION))) return p11_global_end(CKR_ARGUMENTS_BAD);
	if (!(flags & CKF_SERIAL_SESSION)) return p11_global_end(CKR_SESSION_PARALLEL_NOT_SUPPORTED);
	for (session = p11.sessions; session; session = session->next) count++;
	if (count >= P11_SESSIONS) return p11_global_end(CKR_SESSION_COUNT);
	session = OPENSSL_zalloc(sizeof(*session));
	if (!session) return p11_global_end(CKR_HOST_MEMORY);
	if (pthread_mutex_init(&session->lock, NULL)) { OPENSSL_free(session); return p11_global_end(CKR_CANT_LOCK); }
	session->handle = p11_handle(); session->flags = flags;
	if (!session->handle) { session_free(session); return p11_global_end(CKR_HOST_MEMORY); }
	session->next = p11.sessions; p11.sessions = session; *handle = session->handle; return p11_global_end(CKR_OK);
}

CK_RV C_CloseSession(CK_SESSION_HANDLE handle)
{
	struct p11_session *session, **link; CK_RV rv = p11_global_begin(); if (rv) return rv;
	session = p11_session_find(handle);
	if (!session) return p11_global_end(CKR_SESSION_HANDLE_INVALID);
	session->closing = 1;
	while (session->users) pthread_cond_wait(&p11.idle, &p11.lock);
	for (link = &p11.sessions; *link != session; link = &(*link)->next) { }
	*link = session->next; p11_objects_clear(handle); session_free(session);
	if (!p11.sessions) p11.logged_in = 0;
	return p11_global_end(CKR_OK);
}

CK_RV C_CloseAllSessions(CK_SLOT_ID slot)
{
	CK_RV rv = p11_global_begin(); if (rv) return rv;
	if (slot != 1) return p11_global_end(CKR_SLOT_ID_INVALID);
	p11.quiescing = 1;
	while (p11.users != 1) pthread_cond_wait(&p11.idle, &p11.lock);
	/* API admission is closed, including to other session closers. */
	while (p11.sessions) {
		struct p11_session *s = p11.sessions; s->closing = 1;
		p11.sessions = s->next; p11_objects_clear(s->handle); session_free(s);
	}
	p11.logged_in = 0; p11.quiescing = 0; return p11_global_end(CKR_OK);
}

CK_RV C_GetSessionInfo(CK_SESSION_HANDLE handle, CK_SESSION_INFO_PTR info)
{
	struct p11_session *s; CK_RV rv = p11_global_begin(); if (rv) return rv;
	s = p11_session_find(handle);
	if (!s) return p11_global_end(CKR_SESSION_HANDLE_INVALID);
	if (!info) return p11_global_end(CKR_ARGUMENTS_BAD);
	memset(info, 0, sizeof(*info)); info->slotID = 1; info->flags = s->flags;
	info->state = s->flags & CKF_RW_SESSION ? (p11.logged_in ? CKS_RW_USER_FUNCTIONS : CKS_RW_PUBLIC_SESSION) :
		(p11.logged_in ? CKS_RO_USER_FUNCTIONS : CKS_RO_PUBLIC_SESSION);
	return p11_global_end(CKR_OK);
}

CK_RV C_Login(CK_SESSION_HANDLE handle, CK_USER_TYPE user, CK_UTF8CHAR_PTR pin, CK_ULONG size)
{
	int fd; CK_RV rv = p11_global_begin(); if (rv) return rv;
	if (!p11_session_find(handle)) return p11_global_end(CKR_SESSION_HANDLE_INVALID);
	if (user != CKU_USER) return p11_global_end(CKR_USER_TYPE_INVALID);
	if (!pin) return p11_global_end(CKR_ARGUMENTS_BAD);
	if (size > 1) return p11_global_end(CKR_PIN_LEN_RANGE);
	/* Some standard clients reject an empty PIN before calling Cryptoki.
	 * One ASCII space is also a public activation marker, not a credential.
	 * Device/file permissions remain the authorization boundary.
	 */
	if (size && pin[0] != ' ') return p11_global_end(CKR_PIN_INCORRECT);
	if (p11.logged_in) return p11_global_end(CKR_USER_ALREADY_LOGGED_IN);
	fd = open("/dev/s31-crypto", O_RDWR | O_CLOEXEC);
	if (fd < 0) return p11_global_end(CKR_USER_NOT_LOGGED_IN);
	close(fd); p11.logged_in = 1; return p11_global_end(CKR_OK);
}

CK_RV C_Logout(CK_SESSION_HANDLE handle)
{
	CK_RV rv = p11_global_begin(); if (rv) return rv;
	if (!p11_session_find(handle)) return p11_global_end(CKR_SESSION_HANDLE_INVALID);
	if (!p11.logged_in) return p11_global_end(CKR_USER_NOT_LOGGED_IN);
	p11.logged_in = 0; p11.quiescing = 1;
	while (p11.users != 1) pthread_cond_wait(&p11.idle, &p11.lock);
	for (struct p11_session *s = p11.sessions; s; s = s->next) {
		if (s->sign.object && s->sign.object->private) p11_operation_clear(&s->sign);
		if (s->verify.object && s->verify.object->private) p11_operation_clear(&s->verify);
		if (s->encrypt.object && s->encrypt.object->private) p11_operation_clear(&s->encrypt);
		if (s->decrypt.object && s->decrypt.object->private) p11_operation_clear(&s->decrypt);
	}
	/* Logging out destroys private session objects, rather than making their
	 * handles usable again after a later login (PKCS #11 C_Logout).
	 */
	for (struct p11_object **link = &p11.objects, *o; (o = *link); ) {
		if (o->token || !o->private) { link = &o->next; continue; }
		*link = o->next; o->deleted = 1; p11_object_free(o);
	}
	p11.quiescing = 0; return p11_global_end(CKR_OK);
}

CK_RV C_GenerateRandom(CK_SESSION_HANDLE handle, CK_BYTE_PTR output, CK_ULONG length)
{
	struct p11_session *session; CK_RV rv = p11_session_begin(handle, &session);
	if (rv) return rv;
	rv = !output && length ? CKR_ARGUMENTS_BAD : RAND_bytes_ex(p11.libctx, output, length, 0) ? CKR_OK : CKR_RANDOM_NO_RNG;
	return p11_session_end(session, rv);
}

static void module_close(void)
{
	cancel_enter();
	pthread_mutex_lock(&p11.lock); p11.quiescing = 1;
	while (p11.users) pthread_cond_wait(&p11.idle, &p11.lock);
	if (p11.initialized) cleanup();
	terminated = 1;
	p11.quiescing = 0; pthread_cond_broadcast(&p11.idle); pthread_mutex_unlock(&p11.lock);
	cancel_leave(0);
}
