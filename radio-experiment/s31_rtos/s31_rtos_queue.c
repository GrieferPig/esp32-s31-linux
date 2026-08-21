/*
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * FreeRTOS queue/semaphore/mutex compatibility backed by the opaque Linux
 * synchronization bridge.  Queue state remains in the payload ABI; Linux
 * owns the actual lock and wait queue.
 */
#include <stdint.h>
#include <string.h>
#include "s31_rtos.h"



static volatile BaseType_t s31_queue_trace_enabled;
static uint32_t s31_queue_trace_send_count;
static uint32_t s31_queue_trace_receive_count;
static uint32_t s31_queue_trace_rxevent_send_count;
static uint32_t s31_queue_trace_rxevent_receive_count;

void s31_rtos_trace_queues(BaseType_t enable)
{
	s31_queue_trace_enabled = enable;
}

static int s31_queue_is_wifi_main(const struct s31_queue *q)
{
	/* S31 IDF's PP/Wi-Fi dispatcher queue is measured as 200 x 8 bytes. */
	return q && q->capacity == 200 && q->item_size == 8;
}

static uint32_t s31_queue_event_id(const struct s31_queue *q, const void *item)
{
	uint32_t event = UINT32_MAX;

	if (s31_queue_is_wifi_main(q) && item)
		memcpy(&event, item, sizeof(event));
	return event;
}

static void s31_queue_trace_send(struct s31_queue *q, const void *item,
				 BaseType_t from_isr, BaseType_t ret)
{
	uint32_t event;
	uint32_t n;

	if (!s31_queue_trace_enabled || !s31_queue_is_wifi_main(q))
		return;
	event = s31_queue_event_id(q, item);
	n = ++s31_queue_trace_send_count;
	if (event == 13)
		s31_queue_trace_rxevent_send_count++;
	if (n <= 32 || (event == 13 && s31_queue_trace_rxevent_send_count <= 32))
		s31_linux_printf("[S31] wifi-q send #%u ev=%u rxev=%u isr=%d ret=%d count=%u head=%u\n",
			       n, event, s31_queue_trace_rxevent_send_count,
			       from_isr, ret, q->count, q->head);
}

static void s31_queue_trace_receive(struct s31_queue *q, const void *item,
				    BaseType_t ret)
{
	uint32_t event;
	uint32_t n;

	if (!s31_queue_is_wifi_main(q))
		return;
	event = s31_queue_event_id(q, item);
	if (ret)
		s31_linux_trace_wifi_event(event);
	if (!s31_queue_trace_enabled)
		return;
	n = ++s31_queue_trace_receive_count;
	if (event == 13)
		s31_queue_trace_rxevent_receive_count++;
	if (n <= 32 || (event == 13 && s31_queue_trace_rxevent_receive_count <= 32))
		s31_linux_printf("[S31] wifi-q recv #%u ev=%u rxev=%u ret=%d count=%u head=%u\n",
			       n, event, s31_queue_trace_rxevent_receive_count,
			       ret, q->count, q->head);
}

static uint32_t s31_queue_create_count;
static uint32_t s31_queue_dynamic_create_count;
static uint32_t s31_queue_delete_count;
static uint32_t s31_mutex_create_count;
static uint32_t s31_sem_create_count;
static uint32_t s31_sync_take_count;
static uint32_t s31_sync_give_count;

static const char *s31_queue_task_name(void)
{
	struct s31_tcb *t = s31_rtos_current();

	return t ? t->name : (s31_rtos_in_isr() ? "isr" : "orphan");
}

static void s31_queue_init(struct s31_queue *q, uint32_t type,
			   uint32_t len, uint32_t item_size, uint8_t *storage)
{
	memset(q, 0, sizeof(*q));
	q->type = type;
	q->capacity = len;
	q->item_size = item_size;
	q->storage = (uint32_t)storage;
	q->is_static = 0;
	q->wait_context = s31_linux_sync_create();
}

static int s31_queue_valid(const struct s31_queue *q)
{
	return q && q->wait_context;
}

void *xQueueGenericCreate(uint32_t queue_len, uint32_t item_size,
			  uint8_t queue_type)
{
	struct s31_queue *q;
	uint8_t *storage;
	uint32_t total;
	uint32_t count = ++s31_queue_dynamic_create_count;

	if (count <= 16)
		s31_linux_printf("[S31] queue dynamic create #%u len=%u item=%u type=%u\n",
			       count, queue_len, item_size, queue_type);

	if (!queue_len || (item_size &&
	    queue_len > (UINT32_MAX - sizeof(*q)) / item_size))
		return NULL;
	total = sizeof(*q) + queue_len * item_size;
	q = s31_rtos_malloc(total);
	if (!q)
		return NULL;
	storage = (uint8_t *)q + sizeof(*q);
	s31_queue_init(q, queue_type, queue_len, item_size, storage);
	if (!q->wait_context) {
		s31_rtos_free(q);
		return NULL;
	}
	if (count <= 16)
		s31_linux_printf("[S31] queue dynamic create #%u -> q=%p wait=%p storage=%p\n",
			       count, q, q->wait_context, storage);
	return q;
}

void *xQueueGenericCreateStatic(uint32_t queue_len, uint32_t item_size,
				uint8_t *storage, void *static_queue,
				uint8_t queue_type)
{
	struct s31_queue *q = static_queue;
	uint32_t count = ++s31_queue_create_count;

	if (count <= 16)
		s31_linux_printf("[S31] queue static create #%u len=%u item=%u storage=%p static=%p type=%u\n",
			       count, queue_len, item_size, storage, static_queue,
			       queue_type);

	if (!q || !queue_len || (item_size && !storage))
		return NULL;
	s31_queue_init(q, queue_type, queue_len, item_size, storage);
	q->is_static = 1;
	if (count <= 16)
		s31_linux_printf("[S31] queue static create #%u -> q=%p wait=%p\n",
			       count, q, q->wait_context);
	return q->wait_context ? q : NULL;
}

BaseType_t xQueueGenericGetStaticBuffers(void *queue, uint8_t **storage,
					 void **static_queue)
{
	struct s31_queue *q = queue;

	if (!q || !q->is_static || !static_queue)
		return pdFALSE;
	if (storage)
		*storage = (uint8_t *)q->storage;
	*static_queue = q;
	return pdTRUE;
}

void *xQueueCreateMutex(uint8_t type)
{
	struct s31_queue *q = s31_rtos_malloc(sizeof(*q));
	uint32_t count = ++s31_mutex_create_count;

	if (!q)
		return NULL;
	s31_queue_init(q, S31_Q_TYPE_MUTEX, 1, 0, NULL);
	(void)type;
	if (!q->wait_context) {
		s31_rtos_free(q);
		return NULL;
	}
	if (count <= 32)
		s31_linux_printf("[S31] mutex create #%u q=%p kind=%u task=%s\n",
			       count, q, type, s31_queue_task_name());
	return q;
}

void *xQueueCreateCountingSemaphore(uint32_t max, uint32_t initial)
{
	struct s31_queue *q;
	uint32_t count = ++s31_sem_create_count;

	if (!max || initial > max)
		return NULL;
	q = s31_rtos_malloc(sizeof(*q));
	if (!q)
		return NULL;
	s31_queue_init(q, S31_Q_TYPE_SEM, max, 0, NULL);
	q->count = initial;
	if (!q->wait_context) {
		s31_rtos_free(q);
		return NULL;
	}
	if (count <= 32)
		s31_linux_printf("[S31] sem create #%u q=%p max=%u init=%u task=%s\n",
			       count, q, max, initial, s31_queue_task_name());
	return q;
}

void vQueueDelete(void *queue)
{
	struct s31_queue *q = queue;

	if (!q)
		return;
	if (++s31_queue_delete_count <= 32)
		s31_linux_printf("[S31] vQueueDelete #%u q=%p type=%u count=%u cap=%u static=%u wait=%p\n",
			       s31_queue_delete_count, q, q->type, q->count,
			       q->capacity, q->is_static, q->wait_context);
	s31_linux_sync_destroy(q->wait_context);
	q->wait_context = NULL;
	if (!q->is_static)
		s31_rtos_free(q);
}

static TickType_t s31_wait_remaining(TickType_t timeout,
				      TickType_t start)
{
	TickType_t elapsed;

	if (timeout == portMAX_DELAY)
		return portMAX_DELAY;
	elapsed = s31_rtos_get_tick() - start;
	return elapsed >= timeout ? 0 : timeout - elapsed;
}

static BaseType_t s31_queue_send_locked(struct s31_queue *q,
					const void *item, BaseType_t copy)
{
	uint32_t idx;

	if (q->type == S31_Q_TYPE_MUTEX) {
		struct s31_tcb *t = s31_rtos_current();

		if (!t || q->owner != t || !q->depth)
			return pdFAIL;
		if (--q->depth == 0) {
			q->owner = NULL;
			q->count = 0;
		}
		return pdTRUE;
	}
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
		memcpy((uint8_t *)q->storage + idx * q->item_size,
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
		memcpy(item, (uint8_t *)q->storage + q->head * q->item_size,
		       q->item_size);
	q->head = (q->head + 1) % q->capacity;
	q->count--;
	return pdTRUE;
}

BaseType_t xQueueGenericSend(void *queue, const void *item,
			     TickType_t timeout, BaseType_t copy)
{
	struct s31_queue *q = queue;
	TickType_t start = s31_rtos_get_tick();
	uint32_t seq;
	int32_t waited;

	if (!s31_queue_valid(q))
		return pdFAIL;
	if (s31_rtos_in_isr())
		return xQueueGenericSendFromISR(queue, item, NULL, copy);
	for (;;) {
		seq = s31_linux_sync_sequence(q->wait_context);
		s31_linux_sync_lock(q->wait_context);
		if (q->type == S31_Q_TYPE_MUTEX || q->type == S31_Q_TYPE_SEM ||
		    q->count < q->capacity || copy == S31_QUEUE_OVERWRITE) {
			BaseType_t ret = s31_queue_send_locked(q, item, copy);
			s31_linux_sync_unlock(q->wait_context);
			(void)s31_sync_give_count;
			s31_queue_trace_send(q, item, pdFALSE, ret);
			if (ret)
				s31_linux_sync_wake(q->wait_context);
			return ret;
		}
		s31_linux_sync_unlock(q->wait_context);
		if (timeout == 0)
			return pdFAIL;
		{
			TickType_t remaining = s31_wait_remaining(timeout, start);

			if (!remaining)
				return pdFAIL;
			waited = s31_linux_sync_wait(q->wait_context, seq,
						     remaining,
						     S31_BLOB_RELEASE_QUEUE_SEND);
		}
		if (waited < 0)
			return pdFAIL;
		if (waited == 0)
			return pdFAIL;
		/* A wake can belong to a competing waiter.  Recheck the predicate;
		 * the Linux bridge sequence prevents a lost wake. */
	}
}

BaseType_t xQueueGenericSendFromISR(void *queue, const void *item,
				    BaseType_t *woken, BaseType_t copy)
{
	struct s31_queue *q = queue;
	BaseType_t ret;

	if (!s31_queue_valid(q))
		return pdFAIL;
	s31_linux_sync_lock(q->wait_context);
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
	s31_queue_trace_send(q, item, pdTRUE, ret);
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

BaseType_t xQueueReceive(void *queue, void *item, TickType_t timeout)
{
	struct s31_queue *q = queue;
	TickType_t start = s31_rtos_get_tick();
	uint32_t seq;
	int32_t waited;

	if (!s31_queue_valid(q))
		return pdFAIL;
	if (s31_rtos_in_isr())
		return xQueueReceiveFromISR(queue, item, NULL);
	for (;;) {
		seq = s31_linux_sync_sequence(q->wait_context);
		s31_linux_sync_lock(q->wait_context);
		if (q->type != S31_Q_TYPE_MUTEX && q->count) {
			BaseType_t ret = s31_queue_take_locked(q, item);
			s31_linux_sync_unlock(q->wait_context);
			s31_queue_trace_receive(q, item, ret);
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
			waited = s31_linux_sync_wait(q->wait_context, seq,
						     remaining,
						     S31_BLOB_RELEASE_QUEUE_RECEIVE);
		}
		if (waited <= 0)
			return pdFALSE;
		/* Recheck after every wake, including wakes for another waiter. */
	}
}

BaseType_t xQueueReceiveFromISR(void *queue, void *item, BaseType_t *woken)
{
	struct s31_queue *q = queue;
	BaseType_t ret;

	if (!s31_queue_valid(q))
		return pdFAIL;
	s31_linux_sync_lock(q->wait_context);
	ret = q->type == S31_Q_TYPE_MUTEX ? pdFALSE :
		s31_queue_take_locked(q, item);
	s31_linux_sync_unlock(q->wait_context);
	s31_queue_trace_receive(q, item, ret);
	if (ret)
		s31_linux_sync_wake(q->wait_context);
	if (ret && woken)
		*woken = pdTRUE;
	return ret;
}

BaseType_t xQueueSemaphoreTake(void *queue, TickType_t timeout)
{
	struct s31_queue *q = queue;
	struct s31_tcb *t = s31_rtos_current();
	TickType_t start = s31_rtos_get_tick();
	uint32_t seq;
	int32_t waited;

	if (!s31_queue_valid(q) || !t)
		return pdFAIL;
	for (;;) {
		seq = s31_linux_sync_sequence(q->wait_context);
		s31_linux_sync_lock(q->wait_context);
		if (q->type == S31_Q_TYPE_MUTEX) {
			if (q->owner == t) {
				q->depth++;
				s31_linux_sync_unlock(q->wait_context);
				(void)s31_sync_take_count;
				return pdTRUE;
			}
			if (!q->owner && !q->count) {
				q->owner = t;
				q->depth = 1;
				q->count = 1;
				s31_linux_sync_unlock(q->wait_context);
				(void)s31_sync_take_count;
				return pdTRUE;
			}
		} else if (q->count) {
			q->count--;
			s31_linux_sync_unlock(q->wait_context);
			(void)s31_sync_take_count;
			s31_linux_sync_wake(q->wait_context);
			return pdTRUE;
		}
		s31_linux_sync_unlock(q->wait_context);
		if (!timeout)
		{
			(void)s31_sync_take_count;
			return pdFALSE;
		}
		{
			TickType_t remaining = s31_wait_remaining(timeout, start);

			if (!remaining)
				waited = 0;
			else
				waited = s31_linux_sync_wait(q->wait_context, seq,
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
	return xQueueSemaphoreTake(queue, timeout);
}

BaseType_t xQueueGiveMutexRecursive(void *queue)
{
	struct s31_queue *q = queue;
	struct s31_tcb *t = s31_rtos_current();
	BaseType_t ret = pdFAIL;

	if (!s31_queue_valid(q) || q->type != S31_Q_TYPE_MUTEX)
		return pdFAIL;
	s31_linux_sync_lock(q->wait_context);
	if (q->owner == t && q->depth) {
		if (--q->depth == 0) {
			q->owner = NULL;
			q->count = 0;
		}
		ret = pdTRUE;
	}
	s31_linux_sync_unlock(q->wait_context);
	if (ret)
		s31_linux_sync_wake(q->wait_context);
	(void)s31_sync_give_count;
	return ret;
}

void *xQueueGetMutexHolder(void *queue)
{
	struct s31_queue *q = queue;
	void *owner;

	if (!s31_queue_valid(q) || q->type != S31_Q_TYPE_MUTEX)
		return NULL;
	s31_linux_sync_lock(q->wait_context);
	owner = q->owner;
	s31_linux_sync_unlock(q->wait_context);
	return owner;
}

UBaseType_t uxQueueMessagesWaiting(void *queue)
{
	struct s31_queue *q = queue;
	UBaseType_t count;

	if (!s31_queue_valid(q))
		return 0;
	s31_linux_sync_lock(q->wait_context);
	/* FreeRTOS represents a mutex as a length-one queue containing one
	 * token while it is available.  Our internal count deliberately tracks
	 * ownership (one while held), so translate at this public ABI boundary. */
	count = q->type == S31_Q_TYPE_MUTEX ? !q->count : q->count;
	s31_linux_sync_unlock(q->wait_context);
	return count;
}

UBaseType_t uxQueueMessagesWaitingFromISR(void *queue)
{
	return uxQueueMessagesWaiting(queue);
}

BaseType_t xQueueGenericReset(void *queue, BaseType_t new_queue)
{
	struct s31_queue *q = queue;

	if (!s31_queue_valid(q))
		return pdFAIL;
	s31_linux_sync_lock(q->wait_context);
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
