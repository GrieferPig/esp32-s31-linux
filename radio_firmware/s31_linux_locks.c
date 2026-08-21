/* SPDX-License-Identifier: Apache-2.0 */
/* ESP-IDF libc locks without M-mode xPortCanYield() CSR reads. */
#include <stdint.h>
#include <string.h>
#include <sys/lock.h>

#include "s31_rtos/s31_rtos.h"

#define S31_MUTEX_NORMAL 1
#define S31_MUTEX_RECURSIVE 4
#define S31_LOCK_SLOTS 32

struct __lock __lock___libc_recursive_mutex;
struct __lock __lock___sinit_recursive_mutex;
struct __lock __lock___malloc_recursive_mutex;
struct __lock __lock___env_recursive_mutex;
struct __lock __lock___sfp_recursive_mutex;
struct __lock __lock___atexit_recursive_mutex;
struct __lock __lock___at_quick_exit_mutex;
struct __lock __lock___tz_mutex;
struct __lock __lock___dd_hash_mutex;
struct __lock __lock___arc4random_mutex;

struct s31_lock_entry {
	struct __lock *key;
	void *handle;
	uint8_t type;
};

static struct s31_lock_entry s31_locks[S31_LOCK_SLOTS];

static void *s31_lock_get(struct __lock *lock, uint8_t type)
{
	void *handle;
	unsigned int i, free_slot = S31_LOCK_SLOTS;

	if (!lock)
		return NULL;

	vPortEnterCritical();
	for (i = 0; i < S31_LOCK_SLOTS; i++) {
		if (s31_locks[i].key == lock) {
			handle = s31_locks[i].handle;
			vPortExitCritical();
			return handle;
		}
		if (!s31_locks[i].key && free_slot == S31_LOCK_SLOTS)
			free_slot = i;
	}
	if (free_slot == S31_LOCK_SLOTS) {
		vPortExitCritical();
		return NULL;
	}
	handle = xQueueCreateMutex(type);
	if (handle) {
		s31_locks[free_slot].key = lock;
		s31_locks[free_slot].handle = handle;
		s31_locks[free_slot].type = type;
	}
	vPortExitCritical();
	return handle;
}

static void *s31_lock_find(struct __lock *lock)
{
	unsigned int i;

	for (i = 0; i < S31_LOCK_SLOTS; i++)
		if (s31_locks[i].key == lock)
			return s31_locks[i].handle;
	return NULL;
}

static int s31_lock_take(struct __lock *lock, TickType_t timeout,
			 uint8_t type)
{
	void *handle = s31_lock_get(lock, type);

	if (!handle)
		return -1;
	if (s31_rtos_in_isr())
		return xQueueReceiveFromISR(handle, NULL, NULL) == pdTRUE ? 0 : -1;
	if (type == S31_MUTEX_RECURSIVE)
		return xQueueTakeMutexRecursive(handle, timeout) == pdTRUE ? 0 : -1;
	return xQueueSemaphoreTake(handle, timeout) == pdTRUE ? 0 : -1;
}

static void s31_lock_give(struct __lock *lock)
{
	void *handle = s31_lock_find(lock);

	if (!handle)
		return;
	if (s31_rtos_in_isr())
		xQueueGiveFromISR(handle, NULL);
	else
		xQueueGiveMutexRecursive(handle);
}

void _lock_init(_lock_t *lock)
{
	if (!lock)
		return;
	*lock = s31_rtos_malloc(sizeof(**lock));
	if (*lock)
		memset(*lock, 0, sizeof(**lock));
}

void _lock_init_recursive(_lock_t *lock)
{
	_lock_init(lock);
}

void _lock_close(_lock_t *lock)
{
	if (lock && *lock) {
		unsigned int i;

		for (i = 0; i < S31_LOCK_SLOTS; i++) {
			if (s31_locks[i].key == *lock) {
				vQueueDelete(s31_locks[i].handle);
				memset(&s31_locks[i], 0, sizeof(s31_locks[i]));
				break;
			}
		}
		s31_rtos_free(*lock);
		*lock = NULL;
	}
}

void _lock_close_recursive(_lock_t *lock)
{
	_lock_close(lock);
}

void _lock_acquire(_lock_t *lock)
{
	if (lock && !*lock)
		_lock_init(lock);
	if (lock && *lock)
		s31_lock_take(*lock, portMAX_DELAY, S31_MUTEX_NORMAL);
}

void _lock_acquire_recursive(_lock_t *lock)
{
	if (lock && !*lock)
		_lock_init_recursive(lock);
	if (lock && *lock)
		s31_lock_take(*lock, portMAX_DELAY, S31_MUTEX_RECURSIVE);
}

int _lock_try_acquire(_lock_t *lock)
{
	if (lock && !*lock)
		_lock_init(lock);
	return lock && *lock ? s31_lock_take(*lock, 0, S31_MUTEX_NORMAL) : -1;
}

int _lock_try_acquire_recursive(_lock_t *lock)
{
	if (lock && !*lock)
		_lock_init_recursive(lock);
	return lock && *lock ? s31_lock_take(*lock, 0, S31_MUTEX_RECURSIVE) : -1;
}

void _lock_release(_lock_t *lock)
{
	if (lock && *lock)
		s31_lock_give(*lock);
}

void _lock_release_recursive(_lock_t *lock)
{
	_lock_release(lock);
}

void __retarget_lock_init(_LOCK_T *lock)
{
	_lock_init(lock);
	if (lock && *lock)
		s31_lock_get(*lock, S31_MUTEX_NORMAL);
}

void __retarget_lock_init_recursive(_LOCK_T *lock)
{
	_lock_init_recursive(lock);
	if (lock && *lock)
		s31_lock_get(*lock, S31_MUTEX_RECURSIVE);
}

void __retarget_lock_close(_LOCK_T lock) { _lock_close(&lock); }
void __retarget_lock_close_recursive(_LOCK_T lock) { _lock_close(&lock); }
void __retarget_lock_acquire(_LOCK_T lock)
{
	s31_lock_take(lock, portMAX_DELAY, S31_MUTEX_NORMAL);
}
void __retarget_lock_acquire_recursive(_LOCK_T lock)
{
	s31_lock_take(lock, portMAX_DELAY, S31_MUTEX_RECURSIVE);
}
int __retarget_lock_try_acquire(_LOCK_T lock)
{
	return s31_lock_take(lock, 0, S31_MUTEX_NORMAL);
}
int __retarget_lock_try_acquire_recursive(_LOCK_T lock)
{
	return s31_lock_take(lock, 0, S31_MUTEX_RECURSIVE);
}
void __retarget_lock_release(_LOCK_T lock) { s31_lock_give(lock); }
void __retarget_lock_release_recursive(_LOCK_T lock) { s31_lock_give(lock); }

void esp_libc_locks_init(void)
{
	memset(s31_locks, 0, sizeof(s31_locks));
	s31_lock_get(&__lock___libc_recursive_mutex, S31_MUTEX_RECURSIVE);
}

void esp_newlib_locks_init(void) __attribute__((alias("esp_libc_locks_init")));
