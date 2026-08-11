/*
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * S31 radio world stub layer: queues, semaphores, mutexes.
 * See docs/S31_radio_rtos_stub.md.
 */
#include <stdint.h>
#include <string.h>
#include "s31_rtos.h"

static void s31_queue_init(struct s31_queue *q, uint32_t type,
			   uint32_t len, uint32_t item_size,
			   uint8_t *storage)
{
	memset(q, 0, sizeof(*q));
	q->type = type;
	q->capacity = len;
	q->item_size = item_size;
	q->storage = (uint32_t)storage;
	q->is_static = 0;
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
	s31_queue_init(q, queue_type, queue_len, item_size, storage);
	return q;
}

void *xQueueGenericCreateStatic(uint32_t queue_len, uint32_t item_size,
				uint8_t *storage, void *static_queue,
				uint8_t queue_type)
{
	struct s31_queue *q = static_queue;

	if (!q || !queue_len || (item_size && !storage))
		return NULL;
	s31_queue_init(q, queue_type, queue_len, item_size, storage);
	q->is_static = 1;
	return q;
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

	if (!q)
		return NULL;
	s31_queue_init(q, S31_Q_TYPE_MUTEX, 1, 0, NULL);
	(void)type;
	return q;
}

void *xQueueCreateCountingSemaphore(uint32_t max, uint32_t initial)
{
	struct s31_queue *q = s31_rtos_malloc(sizeof(*q));

	if (!q || !max || initial > max) {
		if (q)
			s31_rtos_free(q);
		return NULL;
	}
	s31_queue_init(q, S31_Q_TYPE_SEM, max, 0, NULL);
	q->count = initial;
	return q;
}

void vQueueDelete(void *queue)
{
	struct s31_queue *q = queue;
	struct s31_waiter *w = q->waiters;

	while (w) {
		struct s31_waiter *next = w->next;

		w->tcb->wait_obj = NULL;
		s31_rtos_make_ready(w->tcb);
		s31_rtos_free(w);
		w = next;
	}
	if (!q->is_static)
		s31_rtos_free(q);
}

static void *s31_waiter_alloc(struct s31_tcb *t, uint32_t dir)
{
	struct s31_waiter *w = s31_rtos_malloc(sizeof(*w));

	if (!w)
		return NULL;
	w->tcb = t;
	w->dir = dir;
	w->next = NULL;
	return w;
}

/* Link the current task as a waiter on q.  On a send-block it also wakes a
 * parked receiver so the queue can be drained (avoids cooperative
 * deadlock).  Returns 1 on success; on failure the caller must not block. */
static int s31_waiter_link(struct s31_queue *q, struct s31_tcb *t,
			   uint32_t dir)
{
	struct s31_waiter *w = s31_waiter_alloc(t, dir);
	struct s31_waiter **pp;

	if (!w)
		return 0;
	for (pp = &q->waiters; *pp; pp = &(*pp)->next)
		;
	*pp = w;
	if (dir == S31_WAIT_SEND) {
		for (pp = &q->waiters; *pp; pp = &(*pp)->next) {
			struct s31_waiter *r = *pp;

			if (r->dir == S31_WAIT_RECV && r->tcb != t) {
				*pp = r->next;
				r->tcb->wait_obj = NULL;
				s31_rtos_make_ready(r->tcb);
				s31_rtos_free(r);
				return 1;
			}
		}
	}
	return 1;
}

/* --- send --- */
BaseType_t xQueueGenericSend(void *queue, const void *item,
			     TickType_t timeout, BaseType_t copy)
{
	struct s31_queue *q = queue;
	struct s31_tcb *t = s31_rtos_current();

	if (!q)
		return pdFAIL;

	/* from ISR: never block */
	if (s31_rtos_in_isr())
		return xQueueGenericSendFromISR(queue, item, NULL);

	/* FreeRTOS implements xSemaphoreGive() as xQueueGenericSend(), including
	 * for an ordinary mutex.  A locked mutex therefore is not a "full queue":
	 * giving it releases ownership and wakes the next taker.  Recursive mutex
	 * callers normally use xQueueGiveMutexRecursive(), but honour depth here as
	 * well so mixed IDF/pthread wrappers cannot drop ownership prematurely. */
	if (q->type == S31_Q_TYPE_MUTEX) {
		if (!t || q->owner != t || !q->depth)
			return pdFAIL;
		if (--q->depth == 0) {
			q->owner = NULL;
			q->count = 0;
			s31_rtos_wake_waiters(q);
		}
		return pdTRUE;
	} else if (q->type == S31_Q_TYPE_SEM) {
		if (q->count < q->capacity) {
			q->count++;
			s31_rtos_wake_waiters(q);
			return pdTRUE;
		}
	} else if (q->capacity &&
		   (q->count < q->capacity || copy == S31_QUEUE_OVERWRITE)) {
		uint32_t idx;

		if (copy == S31_QUEUE_SEND_TO_FRONT) {
			q->head = (q->head + q->capacity - 1) % q->capacity;
			idx = q->head;
		} else if (copy == S31_QUEUE_OVERWRITE && q->count == q->capacity) {
			idx = q->head;
			q->head = (q->head + 1) % q->capacity;
		} else {
			idx = (q->head + q->count) % q->capacity;
		}

		if (item && q->item_size)
			memcpy((uint8_t *)q->storage + idx * q->item_size,
			       item, q->item_size);
		if (q->count < q->capacity)
			q->count++;
		s31_rtos_wake_waiters(q);
		return pdTRUE;
	}

	/* full: block? */
	if (timeout == 0 || !t)
		return pdFAIL;
	if (!s31_waiter_link(q, t, S31_WAIT_SEND))
		return pdFAIL;
	/* park; resumed when a receiver drained a slot */
	s31_rtos_block(1, q, timeout, pdTRUE);
	if (q->type == S31_Q_TYPE_SEM) {
		if (q->count < q->capacity) {
			q->count++;
			s31_rtos_wake_waiters(q);
			return pdTRUE;
		}
	} else if (q->capacity && q->count < q->capacity) {
		uint32_t idx = (q->head + q->count) % q->capacity;

		if (item && q->item_size)
			memcpy((uint8_t *)q->storage + idx * q->item_size,
			       item, q->item_size);
		q->count++;
		s31_rtos_wake_waiters(q);
		return pdTRUE;
	}
	return pdFAIL;
}

BaseType_t xQueueGenericSendFromISR(void *queue, const void *item,
				    BaseType_t *woken)
{
	struct s31_queue *q = queue;

	if (!q)
		return pdFAIL;
	if (q->type == S31_Q_TYPE_SEM) {
		if (q->count >= q->capacity)
			return pdFAIL;
		q->count++;
	} else {
		if (q->count >= q->capacity)
			return pdFAIL;
		if (item && q->item_size) {
			uint32_t idx = (q->head + q->count) % q->capacity;

			memcpy((uint8_t *)q->storage + idx * q->item_size,
			       item, q->item_size);
		}
		q->count++;
	}
	s31_rtos_wake_waiters(q);
	if (woken)
		*woken = pdTRUE;
	return pdTRUE;
}

BaseType_t xQueueGiveFromISR(void *queue, BaseType_t *woken)
{
	return xQueueGenericSendFromISR(queue, NULL, woken);
}

/* --- receive --- */
static BaseType_t s31_queue_take_item(struct s31_queue *q, void *item)
{
	if (q->count == 0 || q->capacity == 0)
		return pdFALSE;
	if (item && q->item_size)
		memcpy(item, (uint8_t *)q->storage + q->head * q->item_size,
		       q->item_size);
	q->head = (q->head + 1) % q->capacity;
	q->count--;
	s31_rtos_wake_waiters(q);
	return pdTRUE;
}

BaseType_t xQueueReceive(void *queue, void *item, TickType_t timeout)
{
	struct s31_queue *q = queue;
	struct s31_tcb *t = s31_rtos_current();

	if (!q)
		return pdFAIL;
	if (s31_rtos_in_isr())
		return xQueueReceiveFromISR(queue, item, NULL);
	if (s31_queue_take_item(q, item))
		return pdTRUE;
	if (timeout == 0 || !t)
		return pdFALSE;
	if (!s31_waiter_link(q, t, S31_WAIT_RECV))
		return pdFALSE;
	s31_rtos_block(1, q, timeout, pdTRUE);
	/* resumed by a sender; the item must be present */
	return s31_queue_take_item(q, item);
}

BaseType_t xQueueReceiveFromISR(void *queue, void *item, BaseType_t *woken)
{
	struct s31_queue *q = queue;

	if (!q)
		return pdFAIL;
	if (!s31_queue_take_item(q, item))
		return pdFALSE;
	if (woken)
		*woken = pdTRUE;
	return pdTRUE;
}

BaseType_t xQueueSemaphoreTake(void *queue, TickType_t timeout)
{
	struct s31_queue *q = queue;
	struct s31_tcb *t = s31_rtos_current();

	if (!q)
		return pdFAIL;
	if (q->type == S31_Q_TYPE_MUTEX) {
		if (q->owner == t) {
			q->depth++;
			return pdTRUE;
		}
		if (q->owner == NULL && q->count == 0) {
			q->owner = t;
			q->depth = 1;
			q->count = 1;
			return pdTRUE;
		}
	} else {
		if (q->count > 0) {
			q->count--;
			s31_rtos_wake_waiters(q);
			return pdTRUE;
		}
	}
	if (timeout == 0 || !t)
		return pdFALSE;
	if (!s31_waiter_link(q, t, S31_WAIT_RECV))
		return pdFALSE;
	s31_rtos_block(1, q, timeout, pdTRUE);
	if (q->type == S31_Q_TYPE_MUTEX) {
		if (q->owner == NULL && q->count == 0) {
			q->owner = t;
			q->depth = 1;
			q->count = 1;
			return pdTRUE;
		}
	} else if (q->count > 0) {
		q->count--;
		s31_rtos_wake_waiters(q);
		return pdTRUE;
	}
	return pdFALSE;
}

BaseType_t xQueueTakeMutexRecursive(void *queue, TickType_t timeout)
{
	return xQueueSemaphoreTake(queue, timeout);
}

BaseType_t xQueueGiveMutexRecursive(void *queue)
{
	struct s31_queue *q = queue;
	struct s31_tcb *t = s31_rtos_current();

	if (!q || q->type != S31_Q_TYPE_MUTEX)
		return pdFAIL;
	if (q->owner != t)
		return pdFAIL;
	if (--q->depth == 0) {
		q->owner = NULL;
		q->count = 0;
		s31_rtos_wake_waiters(q);
	}
	return pdTRUE;
}

void *xQueueGetMutexHolder(void *queue)
{
	struct s31_queue *q = queue;

	if (!q || q->type != S31_Q_TYPE_MUTEX)
		return NULL;
	return q->owner;
}

UBaseType_t uxQueueMessagesWaiting(void *queue)
{
	struct s31_queue *q = queue;

	return q ? q->count : 0;
}

UBaseType_t uxQueueMessagesWaitingFromISR(void *queue)
{
	return uxQueueMessagesWaiting(queue);
}

BaseType_t xQueueGenericReset(void *queue, BaseType_t new_queue)
{
	struct s31_queue *q = queue;

	if (!q)
		return pdFAIL;
	q->head = 0;
	q->count = 0;
	q->owner = NULL;
	q->depth = 0;
	if (!new_queue)
		s31_rtos_wake_waiters(q);
	return pdPASS;
}

BaseType_t xQueueIsQueueEmptyFromISR(void *queue)
{
	struct s31_queue *q = queue;

	return !q || q->count == 0 ? pdTRUE : pdFALSE;
}
