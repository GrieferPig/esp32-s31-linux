/*
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * FreeRTOS queue/semaphore/mutex compatibility backed by the opaque Linux
 * synchronization bridge.  Queue state remains in the payload ABI; Linux
 * owns the actual lock and wait queue.
 */
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include "s31_rtos.h"

/* Admission compares handle addresses without reading possibly freed queue
 * storage. Independent buckets avoid a runtime-wide scan/exclusion point;
 * admitted calls and held mutexes retain atomic object references. This does
 * not promise stale-handle ABA protection or caller-owned static lifetime. */
struct s31_queue_state {
    struct s31_queue_state *next;
    struct s31_queue *queue;
    void *sync;
    uint32_t refs;
    int deleted;
    uint32_t mutex_subtype;
    int is_static;
    int detached; /* Bucket lock protects admission and static-byte reuse. */
    struct s31_task_cleanup owner_cleanup;
    uint8_t owner_cleanup_registered;
    uint8_t owner_abandoned;
    uint8_t is_mutex;
};
#define S31_QUEUE_BUCKETS 32U
#define S31_QUEUE_MUX_FREE 0xb33fffffU
struct s31_queue_bucket {
    struct { uint32_t owner, count; } mux;
    struct s31_queue_state *head;
};
static struct s31_queue_bucket s31_queue_buckets[S31_QUEUE_BUCKETS] = {
    [0 ... S31_QUEUE_BUCKETS - 1] = { .mux.owner = S31_QUEUE_MUX_FREE },
};

static struct s31_queue_bucket *s31_queue_bucket_for(const void *handle)
{
    uintptr_t key = (uintptr_t)handle >> 3;
    key ^= key >> 7;
    return &s31_queue_buckets[key & (S31_QUEUE_BUCKETS - 1)];
}
/* Private directory bodies never sleep, allocate or call a vendor callback.
 * Keep address admission and references; only replace the general portMUX
 * bridge with a local IRQ-masked atomic lock. The second private mux word
 * stores the original SIE bit, not recursion. No recursive acquisition occurs.
 * Public portMUX semantics and native PI mutexes remain unchanged. */
static __attribute__((noipa)) void s31_directory_lock(struct s31_queue_bucket *b) {
 uint32_t flags, expected;
 __asm__ volatile("csrrci %0,sstatus,2":"=r"(flags)::"memory");
 do { expected=0xb33fffffU; }
 while (!__atomic_compare_exchange_n(&b->mux.owner,&expected,0,0,
                                    __ATOMIC_ACQUIRE,__ATOMIC_RELAXED));
 b->mux.count=flags&2U;
}
static __attribute__((noipa)) void s31_directory_unlock(struct s31_queue_bucket *b) {
 uint32_t flags=b->mux.count;
 __atomic_store_n(&b->mux.owner,0xb33fffffU,__ATOMIC_RELEASE);
 __asm__ volatile("csrs sstatus,%0"::"r"(flags):"memory");
}

static void s31_queue_ref(struct s31_queue_state *state)
{
    uint32_t old = __atomic_fetch_add(&state->refs, 1, __ATOMIC_RELAXED);
    if (!old || old == UINT32_MAX)
        s31_linux_sync_owner(NULL, S31_SYNC_OWNER_FATAL);
}
void s31_rtos_queue_runtime_reset(void)
{
    /* Only after task/work/IRQ join and old private heap reclamation. */
    for (unsigned int i = 0; i < S31_QUEUE_BUCKETS; i++) {
        s31_queue_buckets[i].head = NULL;
        s31_queue_buckets[i].mux.owner = S31_QUEUE_MUX_FREE;
        s31_queue_buckets[i].mux.count = 0;
    }
}
static struct s31_queue_state *s31_queue_get(void *handle)
{
    struct s31_queue_state *state;
    struct s31_queue_bucket *bucket;
    if (!handle) return NULL;
    bucket = s31_queue_bucket_for(handle);
    s31_directory_lock(bucket);
    for (state = bucket->head; state; state = state->next)
        if (state->queue == handle && !state->detached) {
            s31_queue_ref(state);
            break;
        }
    s31_directory_unlock(bucket);
    return state;
}
static void s31_queue_put(struct s31_queue_state *state)
{
    struct s31_queue_bucket *bucket;
    struct s31_queue_state **link;
    uint32_t old = __atomic_fetch_sub(&state->refs, 1, __ATOMIC_RELEASE);

    if (!old) s31_linux_sync_owner(NULL, S31_SYNC_OWNER_FATAL);
    if (old != 1) return;
    __atomic_thread_fence(__ATOMIC_ACQUIRE);
    bucket = s31_queue_bucket_for(state->queue);
    s31_directory_lock(bucket);
    /* The published handle's base reference is dropped only after delete
     * closes admission. A zero-ref descriptor must never be discoverable. */
    if (!state->detached)
        s31_linux_sync_owner(NULL, S31_SYNC_OWNER_FATAL);
    for (link = &bucket->head; *link && *link != state; link = &(*link)->next) {}
    if (!*link) s31_linux_sync_owner(NULL, S31_SYNC_OWNER_FATAL);
    *link = state->next;
    if (state->is_static) state->queue->wait_context = NULL;
    s31_directory_unlock(bucket);
    s31_linux_sync_destroy(state->sync);
    if (!state->is_static) s31_rtos_free(state->queue);
    s31_rtos_free(state);
}

int s31_rtos_softmac_tx_copy(void *request)
{
    return s31_linux_sync_owner(request, S31_SYNC_SOFTMAC_TX_COPY);
}

/* Pinned ROM class1 record: alloc u32, recycle u32, failure fields.
 * Binding is mandatory; old adapters refuse the appended operation. */
int s31_rtos_tx_pool_bind(void *counter)
{
    /* Probe with NULL before passing a non-sync pointer to an old adapter. */
    return s31_linux_sync_owner(NULL, S31_SYNC_TX_POOL_BIND) &&
        (!counter || s31_linux_sync_owner(counter, S31_SYNC_TX_POOL_BIND));
}

void s31_rtos_tx_flight_return(void)
{
    s31_linux_sync_owner(NULL, S31_SYNC_TX_FLIGHT_RETURN);
}

/* Cold binding pins ordinary queue admission while the native side marks
 * this exact mutex. No global handle watcher survives queue destruction. */
int s31_rtos_mutex_tx_credit_bind(void *handle)
{
    struct s31_queue_state *state = s31_queue_get(handle);
    int result;

    if (!state) return 0;
    result = state->is_mutex &&
        s31_linux_sync_owner(state->sync, S31_SYNC_TX_LOCK_BIND);
    s31_queue_put(state);
    return result;
}

static void s31_queue_call_release(struct s31_task_cleanup *cleanup)
{
    struct s31_queue_call *call = (struct s31_queue_call *)
        ((char *)cleanup - offsetof(struct s31_queue_call, cleanup));
    s31_queue_put(call->state);
    call->state = NULL;
    call->cleanup.release = NULL;
}
static int32_t s31_queue_call_wait(struct s31_queue_state *state,
                                  struct s31_queue_call **owner,
                                  uint32_t sequence, TickType_t timeout,
                                  uint32_t reason)
{
    /* Foreign owners now retain mutex cleanup leases, but this API still
     * polls: no new blocking admission on a foreign SRAM stack. */
    struct s31_tcb *task = s31_rtos_current();
    if (!task || !task->linux_task) return -2; /* local native-task rejection */
    if (!*owner) {
        struct s31_queue_call *call = &task->native_call;
        /* One synchronous blocked admission per task; IRQ calls cannot
         * block, and task finalization drains this node before TLS runs. */
        if (call->state) s31_linux_sync_owner(NULL, S31_SYNC_OWNER_FATAL);
        memset(call, 0, sizeof(*call));
        call->state = state;
        call->cleanup.release = s31_queue_call_release;
        if (!s31_rtos_cleanup_push(&call->cleanup)) {
            /* Foreign workers may poll, never retain an untracked sleep. */
            call->state = NULL;
            call->cleanup.release = NULL;
            return -3; /* local cleanup admission rejection */
        }
        *owner = call; /* Transfer this call's reference, not an extra ref. */
    }
    return s31_linux_sync_wait(state->sync, sequence, timeout, reason);
}
static void s31_queue_call_finish(struct s31_queue_state *state,
                                  struct s31_queue_call *call)
{
    if (call) {
        s31_rtos_cleanup_pop(&call->cleanup);
        call->cleanup.release(&call->cleanup);
    } else s31_queue_put(state);
}

/* An ownership lease pins the queue until the actual native owner's final
 * continuation or legitimate last give. Never transfer an abandoned critical
 * section to another task: poison that mutex until it is explicitly deleted. */
static void s31_mutex_owner_cleanup(struct s31_task_cleanup *cleanup)
{
    struct s31_queue_state *state = (struct s31_queue_state *)
        ((char *)cleanup - offsetof(struct s31_queue_state, owner_cleanup));
    s31_linux_sync_lock(state->sync);
    if (!state->owner_cleanup_registered ||
        !s31_linux_sync_owner(state->sync, S31_SYNC_OWNER_ABANDON))
        s31_linux_sync_owner(NULL, S31_SYNC_OWNER_FATAL);
    state->owner_cleanup_registered = 0;
    state->owner_abandoned = 1;
    state->queue->owner = NULL;
    /* count/depth continue to represent the unfinished held span. */
    s31_linux_sync_unlock(state->sync);
    {
        if (!s31_linux_sync_owner(state->sync, S31_SYNC_NATIVE_RELEASE))
            s31_linux_sync_owner(NULL, S31_SYNC_OWNER_FATAL);
    }
    s31_queue_put(state);
}

static int s31_mutex_claim_locked(struct s31_queue_state *state,
                                struct s31_tcb *cookie)
{
        if (state->owner_abandoned) return 0;
        state->owner_cleanup.release = s31_mutex_owner_cleanup;
        /* Publish the lease only after cleanup registration succeeds. The
         * native owner remains an unpublished reservation until CLAIM.
         */
        if (!s31_rtos_cleanup_push(&state->owner_cleanup)) return 0;
        if (!s31_linux_sync_owner(state->sync, S31_SYNC_OWNER_CLAIM)) {
            s31_rtos_cleanup_pop(&state->owner_cleanup);
            return 0;
        }
        /* The admitted call already pins state; no directory lock is
         * needed to retain the published owner's independent lease. */
        s31_queue_ref(state);
        state->owner_cleanup_registered = 1;
        state->queue->owner = cookie;
        state->queue->count = state->queue->depth = 1;
        return 1;
    }

static int s31_queue_init(struct s31_queue *q, uint32_t type,
                           uint32_t len, uint32_t item_size, uint8_t *storage,
                           int is_static, uint32_t initial_count, uint32_t mutex_subtype)
{
    struct s31_queue_state *state = s31_rtos_malloc(sizeof(*state));
    struct s31_queue_bucket *bucket = s31_queue_bucket_for(q);
    if (!state) return 0;
    memset(state, 0, sizeof(*state));
    state->sync = s31_linux_sync_create();
    state->is_mutex = (type == S31_Q_TYPE_MUTEX);
    if (state->sync && state->is_mutex &&
        !s31_linux_sync_owner(state->sync, S31_SYNC_NATIVE_INIT)) {
        s31_linux_sync_destroy(state->sync);
        s31_rtos_free(state);
        return 0;
    }
    if (!state->sync) {
        s31_rtos_free(state);
        return 0;
    }
    s31_directory_lock(bucket);
    /* Do not reinitialise static bytes with active admitted users. */
    for (struct s31_queue_state *other = bucket->head; other; other = other->next)
        if (other->queue == q) {
            s31_directory_unlock(bucket);
            s31_linux_sync_destroy(state->sync);
            s31_rtos_free(state);
            return 0;
        }
    memset(q, 0, sizeof(*q));
    q->type = type; q->capacity = len; q->item_size = item_size;
    q->count = initial_count; /* Fully initialise before registry publication. */
    q->storage = (uint32_t)(uintptr_t)storage;
    q->is_static = is_static;
    q->wait_context = state->sync;
    state->queue = q; state->is_static = is_static; state->refs = 1;
    state->mutex_subtype = type == S31_Q_TYPE_MUTEX ?
        (mutex_subtype ? mutex_subtype : S31_MUTEX_NORMAL) : 0;
    state->next = bucket->head; bucket->head = state;
    s31_directory_unlock(bucket);
    return 1;
}

void *xQueueGenericCreate(uint32_t queue_len, uint32_t item_size,
			  uint8_t queue_type)
{
	struct s31_queue *q;
	uint8_t *storage;
	uint32_t total;

	if (!queue_len || (item_size &&
	    queue_len > (UINT32_MAX - sizeof(*q)) / item_size))
		return NULL;
	total = sizeof(*q) + queue_len * item_size;
	q = s31_rtos_malloc(total);
	if (!q)
		return NULL;
	storage = (uint8_t *)q + sizeof(*q);
	if (!s31_queue_init(q, queue_type, queue_len, item_size, storage, 0, 0, 0)) {
		s31_rtos_free(q);
		return NULL;
	}
	return q;
}

void *xQueueGenericCreateStatic(uint32_t queue_len, uint32_t item_size,
				uint8_t *storage, void *static_queue,
				uint8_t queue_type)
{
	struct s31_queue *q = static_queue;

	if (!q || !queue_len || (item_size && !storage))
		return NULL;
	return s31_queue_init(q, queue_type, queue_len, item_size, storage, 1, 0, 0) ? q : NULL;
}

static BaseType_t s31_admitted_xQueueGenericGetStaticBuffers(void *queue, uint8_t **storage,
					 void **static_queue, struct s31_queue_state *state)
{
    struct s31_queue *q = queue;
    BaseType_t result = pdFALSE;
    s31_linux_sync_lock(state->sync);
    if (!state->deleted && q->is_static && static_queue) {
        if (storage) *storage = (uint8_t *)(uintptr_t)q->storage;
        *static_queue = q; result = pdTRUE;
    }
    s31_linux_sync_unlock(state->sync);
    return result;
}

void *xQueueCreateMutex(uint8_t type)
{
	struct s31_queue *q;

    if (type != S31_MUTEX_NORMAL && type != S31_MUTEX_RECURSIVE) return NULL;
    q = s31_rtos_malloc(sizeof(*q));
	if (!q)
		return NULL;
	if (!s31_queue_init(q, S31_Q_TYPE_MUTEX, 1, 0, NULL, 0, 0, type)) {
		s31_rtos_free(q);
		return NULL;
	}
	return q;
}

void *xQueueCreateMutexStatic(uint8_t type, void *static_queue)
{
	struct s31_queue *q = static_queue;

	/* IDF owns the supplied StaticQueue_t storage.  The Linux bridge keeps
	 * wait state out of that ABI object, so initialise only the compact queue
	 * representation used by the payload and never free it in vQueueDelete(). */
	if (!q)
		return NULL;
    if (type != S31_MUTEX_NORMAL && type != S31_MUTEX_RECURSIVE) return NULL;
	return s31_queue_init(q, S31_Q_TYPE_MUTEX, 1, 0, NULL, 1, 0, type) ? q : NULL;
}

void *xQueueCreateCountingSemaphore(uint32_t max, uint32_t initial)
{
	struct s31_queue *q;

	if (!max || initial > max)
		return NULL;
	q = s31_rtos_malloc(sizeof(*q));
	if (!q)
		return NULL;
	if (!s31_queue_init(q, S31_Q_TYPE_SEM, max, 0, NULL, 0, initial, 0)) {
		s31_rtos_free(q);
		return NULL;
	}
	return q;
}

void vQueueDelete(void *queue)
{
    struct s31_queue_state **link, *state = NULL;
    struct s31_queue_bucket *bucket;
    if (!queue || s31_rtos_in_isr()) return;
    bucket = s31_queue_bucket_for(queue);
    s31_directory_lock(bucket);
    for (link = &bucket->head; *link; link = &(*link)->next)
        if ((*link)->queue == queue && !(*link)->detached) {
            state = *link; state->detached = 1; break;
        }
    s31_directory_unlock(bucket);
    if (!state) return;
    s31_linux_sync_lock(state->sync);
    state->deleted = 1;
    s31_linux_sync_unlock(state->sync);
    if (state->is_mutex)
        s31_linux_sync_owner(state->sync, S31_SYNC_NATIVE_DELETE);
    else s31_linux_sync_wake(state->sync);
    s31_queue_put(state);
}

static TickType_t s31_wait_remaining(TickType_t timeout,
				      TickType_t start)
{
	TickType_t elapsed;

	if (!timeout || timeout == portMAX_DELAY)
		return timeout;
	elapsed = s31_rtos_get_tick() - start;
	return elapsed >= timeout ? 0 : timeout - elapsed;
}

#include "s31_rtos_native_mutex.inc"

/* Keep this cache-off helper as a manifest-addressable SRAM function. */
static __attribute__((noipa)) BaseType_t s31_queue_send_locked(struct s31_queue *q,
					const void *item, BaseType_t copy)
{
	uint32_t idx;

	if (q->type == S31_Q_TYPE_MUTEX)
        return pdFAIL; /* Mutexes use the native owner protocol only. */
	if (q->type == S31_Q_TYPE_SEM) {
		if (q->count >= q->capacity)
			return pdFAIL;
		q->count++;
		return pdTRUE;
	}
	if (!q->capacity ||
	    (q->count >= q->capacity && copy != S31_QUEUE_OVERWRITE))
		return pdFAIL;
	if (copy == S31_QUEUE_SEND_TO_FRONT) {
		q->head = (q->head + q->capacity - 1) % q->capacity;
		idx = q->head;
	}
	else if (copy == S31_QUEUE_OVERWRITE && q->count == q->capacity) {
		idx = q->head;
		q->head = (q->head + 1) % q->capacity;
	} else
		idx = (copy == S31_QUEUE_SEND_TO_FRONT) ? q->head :
			(q->head + q->count) % q->capacity;
	if (item && q->item_size)
		memcpy((uint8_t *)(uintptr_t)q->storage + idx * q->item_size,
		       item, q->item_size);
	if (q->count < q->capacity)
		q->count++;
	return pdTRUE;
}

static BaseType_t s31_queue_take_locked(struct s31_queue *q, void *item)
{
	if (!q->count || !q->capacity)
		return pdFALSE;
	if (item && q->item_size)
		memcpy(item, (uint8_t *)(uintptr_t)q->storage + q->head * q->item_size,
		       q->item_size);
	q->head = (q->head + 1) % q->capacity;
	q->count--;
	return pdTRUE;
}

static BaseType_t s31_admitted_xQueueGenericSend(void *queue, const void *item,
			     TickType_t timeout, BaseType_t copy, struct s31_queue_state *state, struct s31_queue_call **call, uint32_t reason)
{
	struct s31_queue *q = queue;
	TickType_t start;
	uint32_t seq;
	int32_t waited;

    if (state->is_mutex) {
        if (state->mutex_subtype != S31_MUTEX_NORMAL) return pdFAIL;
        return s31_native_mutex_give(state, call);
    }
    start = timeout && timeout != portMAX_DELAY ? s31_rtos_get_tick() : 0;
	for (;;) {
		seq = s31_linux_sync_sequence(q->wait_context);
		s31_linux_sync_lock(q->wait_context);
	if (state->deleted || (q->type == S31_Q_TYPE_MUTEX &&
            state->mutex_subtype != S31_MUTEX_NORMAL)) {
            s31_linux_sync_unlock(q->wait_context);
             waited = 0; goto failed;
        }
		if (q->type == S31_Q_TYPE_MUTEX || q->type == S31_Q_TYPE_SEM ||
		    q->count < q->capacity || copy == S31_QUEUE_OVERWRITE) {
			BaseType_t ret = s31_queue_send_locked(q, item, copy);
			s31_linux_sync_unlock(q->wait_context);

			if (ret)
				s31_linux_sync_wake(q->wait_context);

			return ret;
		}
		s31_linux_sync_unlock(q->wait_context);
		if (timeout == 0) {
             waited = 0; goto failed;
        }
		{
			TickType_t remaining = s31_wait_remaining(timeout, start);

			if (!remaining) {
                 waited = 0; goto failed;
            }
			waited = s31_queue_call_wait(state, call, seq,
						     remaining,
						     reason);
		}
		if (waited <= 0) {

            goto failed;
        }
		/* A wake can belong to a competing waiter.  Recheck the predicate;
		 * the Linux bridge sequence prevents a lost wake. */
	}
failed:

    return pdFAIL;
}

static BaseType_t s31_admitted_xQueueGenericSendFromISR(void *queue, const void *item,
				    BaseType_t *woken, BaseType_t copy, struct s31_queue_state *state)
{
	struct s31_queue *q = queue;
	BaseType_t ret;

	s31_linux_sync_lock(q->wait_context);
	if (state->deleted) {
        s31_linux_sync_unlock(q->wait_context);

        return 0;
    }
	if (q->type == S31_Q_TYPE_MUTEX) {
		/* FreeRTOS ISR give is only meaningful for a semaphore. */
		ret = pdFAIL;
	} else if (q->type == S31_Q_TYPE_SEM) {
		if (q->count >= q->capacity)
			ret = pdFAIL;
		else {
			q->count++;
			ret = pdTRUE;
		}
	} else {
		ret = s31_queue_send_locked(q, item, copy);
	}
	s31_linux_sync_unlock(q->wait_context);

	if (ret)
		s31_linux_sync_wake(q->wait_context);

	/* FreeRTOS only ever promotes this flag to pdTRUE.  One ISR can perform
	 * several queue operations using the same flag; a later failed/no-wake
	 * operation must not erase an earlier required yield. */
	if (ret && woken)
		*woken = pdTRUE;
	return ret;
}

BaseType_t xQueueGiveFromISR(void *queue, BaseType_t *woken)
{
	return xQueueGenericSendFromISR(queue, NULL, woken,
				      S31_QUEUE_SEND_TO_BACK);
}

static BaseType_t s31_admitted_xQueueReceive(void *queue, void *item, TickType_t timeout, struct s31_queue_state *state, struct s31_queue_call **call)
{
	struct s31_queue *q = queue;
	TickType_t start = timeout && timeout != portMAX_DELAY ?
        s31_rtos_get_tick() : 0;
	uint32_t seq;
	int32_t waited;

	for (;;) {
		seq = s31_linux_sync_sequence(q->wait_context);
		s31_linux_sync_lock(q->wait_context);
	if (state->deleted) { s31_linux_sync_unlock(q->wait_context); return 0; }
		if (q->type != S31_Q_TYPE_MUTEX && q->count) {
			BaseType_t ret = s31_queue_take_locked(q, item);
			s31_linux_sync_unlock(q->wait_context);

			if (ret)
				s31_linux_sync_wake(q->wait_context);
			return ret;
		}
		s31_linux_sync_unlock(q->wait_context);
		if (timeout == 0)
			return pdFALSE;
		{
			TickType_t remaining = s31_wait_remaining(timeout, start);

			if (!remaining)
				return pdFALSE;
			waited = s31_queue_call_wait(state, call, seq,
						     remaining,
						     S31_BLOB_RELEASE_QUEUE_RECEIVE);
		}
		if (waited <= 0)
			return pdFALSE;
		/* Recheck after every wake, including wakes for another waiter. */
	}
}

static BaseType_t s31_admitted_xQueueReceiveFromISR(void *queue, void *item, BaseType_t *woken, struct s31_queue_state *state)
{
	struct s31_queue *q = queue;
	BaseType_t ret;

	s31_linux_sync_lock(q->wait_context);
	if (state->deleted) { s31_linux_sync_unlock(q->wait_context); return 0; }
	ret = q->type == S31_Q_TYPE_MUTEX ? pdFALSE :
		s31_queue_take_locked(q, item);
	s31_linux_sync_unlock(q->wait_context);

	if (ret)
		s31_linux_sync_wake(q->wait_context);
	if (ret && woken)
		*woken = pdTRUE;
	return ret;
}

static BaseType_t s31_admitted_xQueueSemaphoreTake(void *queue, TickType_t timeout, struct s31_queue_state *state, struct s31_queue_call **call, int recursive)
{
	struct s31_queue *q = queue;
	struct s31_tcb *t = s31_rtos_current();
	TickType_t start;
	uint32_t seq;
	int32_t waited;

	if (!t) return pdFAIL;
    if (recursive ? (q->type != S31_Q_TYPE_MUTEX || state->mutex_subtype != S31_MUTEX_RECURSIVE) :
        (q->type == S31_Q_TYPE_MUTEX && state->mutex_subtype != S31_MUTEX_NORMAL)) return pdFAIL;
	if (state->is_mutex) {
		return s31_native_mutex_take(state, call, timeout, recursive);
	}
    start = timeout && timeout != portMAX_DELAY ? s31_rtos_get_tick() : 0;
	for (;;) {
		seq = s31_linux_sync_sequence(q->wait_context);
		s31_linux_sync_lock(q->wait_context);
	if (state->deleted) { s31_linux_sync_unlock(q->wait_context); return 0; }
		if (q->count) {
			q->count--;
			s31_linux_sync_unlock(q->wait_context);

			s31_linux_sync_wake(q->wait_context);
			return pdTRUE;
		}
		s31_linux_sync_unlock(q->wait_context);
		if (!timeout)
		{

			return pdFALSE;
		}
		{
			TickType_t remaining = s31_wait_remaining(timeout, start);

			if (!remaining)
				waited = 0;
			else
				waited = s31_queue_call_wait(state, call, seq,
							     remaining,
							     S31_BLOB_RELEASE_SEMAPHORE_TAKE);
		}
		if (waited <= 0)
			return pdFALSE;
		/* Recheck after every wake. */
	}
}

BaseType_t xQueueTakeMutexRecursive(void *queue, TickType_t timeout)
{
    if (s31_rtos_in_isr()) return pdFALSE;
    struct s31_queue_state *state = s31_queue_get(queue);
    struct s31_queue_call *call = NULL;
    BaseType_t result;
    if (!state) return pdFAIL;
    result = s31_admitted_xQueueSemaphoreTake(queue, timeout, state, &call, 1);
    s31_queue_call_finish(state, call);
    return result;
}

static BaseType_t s31_admitted_xQueueGiveMutexRecursive(void *queue, struct s31_queue_state *state, struct s31_queue_call **call)
{
	struct s31_queue *q = queue;

	if (q->type != S31_Q_TYPE_MUTEX || state->mutex_subtype != S31_MUTEX_RECURSIVE)
		return pdFAIL;
	return s31_native_mutex_give(state, call);
}

static void *s31_admitted_xQueueGetMutexHolder(void *queue, struct s31_queue_state *state)
{
	struct s31_queue *q = queue;
	void *owner;

	if (q->type != S31_Q_TYPE_MUTEX)
		return NULL;
	s31_linux_sync_lock(q->wait_context);
	if (state->deleted) { s31_linux_sync_unlock(q->wait_context); return NULL; }
	{
        struct s31_tcb *held = q->owner;
        owner = held && s31_linux_sync_owner(state->sync, S31_SYNC_OWNER_ALIVE) &&
            held->linux_task ? held : NULL;
    }
	s31_linux_sync_unlock(q->wait_context);
	return owner;
}

static UBaseType_t s31_admitted_uxQueueMessagesWaiting(void *queue, struct s31_queue_state *state)
{
	struct s31_queue *q = queue;
	UBaseType_t count;

	s31_linux_sync_lock(q->wait_context);
	if (state->deleted) { s31_linux_sync_unlock(q->wait_context); return 0; }
	/* FreeRTOS represents a mutex as a length-one queue containing one
	 * token while it is available.  Our internal count deliberately tracks
	 * ownership (one while held), so translate at this public ABI boundary. */
	count = state->is_mutex ?
        s31_linux_sync_owner(state->sync, S31_SYNC_NATIVE_AVAILABLE) :
        q->count;
	s31_linux_sync_unlock(q->wait_context);
	return count;
}

UBaseType_t uxQueueMessagesWaitingFromISR(void *queue)
{
	return uxQueueMessagesWaiting(queue);
}

static BaseType_t s31_admitted_xQueueGenericReset(void *queue, BaseType_t new_queue, struct s31_queue_state *state)
{
	struct s31_queue *q = queue;

	s31_linux_sync_lock(q->wait_context);
	if (state->deleted) { s31_linux_sync_unlock(q->wait_context); return 0; }
    /* Public reset is not an owner release. In particular new_queue does
     * not authorize clearing an already-held ordinary/recursive mutex. */
    if (q->type == S31_Q_TYPE_MUTEX && (q->owner || q->depth || q->count ||
        (state->is_mutex && s31_linux_sync_owner(state->sync, S31_SYNC_NATIVE_BUSY)))) {
        s31_linux_sync_unlock(q->wait_context);
        return pdFAIL;
    }
	q->head = 0;
	q->count = 0;
	q->owner = NULL;
	q->depth = 0;
	s31_linux_sync_unlock(q->wait_context);
	if (!new_queue)
		s31_linux_sync_wake(q->wait_context);
	return pdPASS;
}

BaseType_t xQueueIsQueueEmptyFromISR(void *queue)
{
	return !queue || uxQueueMessagesWaitingFromISR(queue) == 0 ?
		pdTRUE : pdFALSE;
}

BaseType_t xQueueGenericGetStaticBuffers(void *queue, uint8_t **storage,
					 void **static_queue)
{
    struct s31_queue_state *state = s31_queue_get(queue);
    BaseType_t result;
    if (!state) return 0;
    result = s31_admitted_xQueueGenericGetStaticBuffers(queue, storage, static_queue, state);
    s31_queue_put(state);
    return result;
}

BaseType_t xQueueGenericSend(void *queue, const void *item,
			     TickType_t timeout, BaseType_t copy)
{
    if (s31_rtos_in_isr()) return xQueueGenericSendFromISR(queue, item, NULL, copy);
    struct s31_queue_state *state = s31_queue_get(queue);
    struct s31_queue_call *call = NULL;
    BaseType_t result;
    if (!state) {

        return 0;
    }
    result = s31_admitted_xQueueGenericSend(queue, item, timeout, copy, state, &call,
                                           S31_BLOB_RELEASE_QUEUE_SEND);
    s31_queue_call_finish(state, call);
    return result;
}

int s31_rtos_pp_notify_capable(void)
{
    return s31_linux_sync_owner(NULL, S31_SYNC_TX_REPAIR_CAP) != 0;
}

/* Exceptional PP repair only. The queue call keeps its ordinary stable
 * admission/cleanup lease; cancellation returns without deleting the task. */
BaseType_t s31_rtos_pp_notify_send(void *queue, const void *item)
{
    struct s31_queue_state *state = s31_queue_get(queue);
    struct s31_queue_call *call = NULL;
    BaseType_t result;
    if (!state) return pdFAIL;
    if (state->is_mutex || s31_rtos_in_isr()) {
        s31_queue_put(state);
        return pdFAIL;
    }
    result = s31_admitted_xQueueGenericSend(queue, item, portMAX_DELAY,
        S31_QUEUE_SEND_TO_BACK, state, &call,
        S31_BLOB_RELEASE_QUEUE_SEND | S31_SYNC_WAIT_TX_REPAIR);
    s31_queue_call_finish(state, call);
    return result;
}

BaseType_t xQueueGenericSendFromISR(void *queue, const void *item,
				    BaseType_t *woken, BaseType_t copy)
{
    struct s31_queue_state *state = s31_queue_get(queue);
    BaseType_t result;
    if (!state) {

        return 0;
    }
    result = s31_admitted_xQueueGenericSendFromISR(queue, item, woken, copy, state);
    s31_queue_put(state);
    return result;
}

BaseType_t xQueueReceive(void *queue, void *item, TickType_t timeout)
{
    if (s31_rtos_in_isr()) return xQueueReceiveFromISR(queue, item, NULL);
    struct s31_queue_state *state = s31_queue_get(queue);
    struct s31_queue_call *call = NULL;
    BaseType_t result;
    if (!state) return 0;
    result = s31_admitted_xQueueReceive(queue, item, timeout, state, &call);
    s31_queue_call_finish(state, call);
    return result;
}

BaseType_t xQueueReceiveFromISR(void *queue, void *item, BaseType_t *woken)
{
    struct s31_queue_state *state = s31_queue_get(queue);
    BaseType_t result;
    if (!state) return 0;
    result = s31_admitted_xQueueReceiveFromISR(queue, item, woken, state);
    s31_queue_put(state);
    return result;
}

BaseType_t xQueueSemaphoreTake(void *queue, TickType_t timeout)
{
    if (s31_rtos_in_isr()) return pdFALSE;
    struct s31_queue_state *state = s31_queue_get(queue);
    struct s31_queue_call *call = NULL;
    BaseType_t result;
    if (!state) return 0;
    result = s31_admitted_xQueueSemaphoreTake(queue, timeout, state, &call, 0);
    s31_queue_call_finish(state, call);
    return result;
}

BaseType_t xQueueGiveMutexRecursive(void *queue)
{
    if (s31_rtos_in_isr()) return pdFALSE;
    struct s31_queue_state *state = s31_queue_get(queue);
    BaseType_t result;
    struct s31_queue_call *call = NULL;
    if (!state) return 0;
    result = s31_admitted_xQueueGiveMutexRecursive(queue, state, &call);
    s31_queue_call_finish(state, call);
    return result;
}

void *xQueueGetMutexHolder(void *queue)
{
    struct s31_queue_state *state = s31_queue_get(queue);
    void * result;
    if (!state) return NULL;
    result = s31_admitted_xQueueGetMutexHolder(queue, state);
    s31_queue_put(state);
    return result;
}

UBaseType_t uxQueueMessagesWaiting(void *queue)
{
    struct s31_queue_state *state = s31_queue_get(queue);
    UBaseType_t result;
    if (!state) return 0;
    result = s31_admitted_uxQueueMessagesWaiting(queue, state);
    s31_queue_put(state);
    return result;
}

BaseType_t xQueueGenericReset(void *queue, BaseType_t new_queue)
{
    struct s31_queue_state *state = s31_queue_get(queue);
    BaseType_t result;
    if (!state) return 0;
    result = s31_admitted_xQueueGenericReset(queue, new_queue, state);
    s31_queue_put(state);
    return result;
}
