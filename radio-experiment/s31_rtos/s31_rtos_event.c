/* SPDX-License-Identifier: BSD-2-Clause */
/* FreeRTOS event groups backed by the Linux synchronization bridge. */
#include <stdint.h>
#include <string.h>
#include "s31_rtos.h"



#define S31_EG_FLAG_ALL_BITS (1U << 0)
#define S31_EG_FLAG_CLEAR    (1U << 1)

static uint32_t s31_event_create_count;
static uint32_t s31_event_set_count;
static uint32_t s31_event_wait_count;

static const char *s31_event_task_name(void)
{
	struct s31_tcb *t = s31_rtos_current();

	return t ? t->name : (s31_rtos_in_isr() ? "isr" : "orphan");
}

static int s31_eg_match(EventBits_t have, EventBits_t want, uint32_t flags)
{
	if (flags & S31_EG_FLAG_ALL_BITS)
		return (have & want) == want;
	return (have & want) != 0;
}

void *xEventGroupCreate(void)
{
	struct s31_event_group *g = s31_rtos_malloc(sizeof(*g));

	if (!g)
		return NULL;
	memset(g, 0, sizeof(*g));
	g->wait_context = s31_linux_sync_create();
	if (!g->wait_context) {
		s31_rtos_free(g);
		return NULL;
	}
	if (++s31_event_create_count <= 16)
		s31_linux_printf("[S31] event create #%u g=%p task=%s\n",
			       s31_event_create_count, g, s31_event_task_name());
	return g;
}

void vEventGroupDelete(void *group)
{
	struct s31_event_group *g = group;

	if (!g)
		return;
	s31_linux_sync_destroy(g->wait_context);
	g->wait_context = NULL;
	s31_rtos_free(g);
}

EventBits_t xEventGroupSetBits(void *group, EventBits_t bits)
{
	struct s31_event_group *g = group;
	EventBits_t result;

	if (!g || !g->wait_context)
		return 0;
	s31_linux_sync_lock(g->wait_context);
	g->bits |= bits;
	result = g->bits;
	s31_linux_sync_unlock(g->wait_context);
	s31_linux_sync_wake(g->wait_context);
	if (++s31_event_set_count <= 64)
		s31_linux_printf("[S31] event set #%u g=%p add=%08x result=%08x task=%s\n",
			       s31_event_set_count, g, bits, result,
			       s31_event_task_name());
	return result;
}

EventBits_t xEventGroupClearBits(void *group, EventBits_t bits)
{
	struct s31_event_group *g = group;
	EventBits_t previous;

	if (!g || !g->wait_context)
		return 0;
	s31_linux_sync_lock(g->wait_context);
	previous = g->bits;
	g->bits &= ~bits;
	s31_linux_sync_unlock(g->wait_context);
	return previous;
}

EventBits_t xEventGroupWaitBits(void *group, EventBits_t bits,
				BaseType_t clear_on_exit,
				BaseType_t wait_all_bits,
				TickType_t timeout)
{
	struct s31_event_group *g = group;
	TickType_t start = s31_rtos_get_tick();
	uint32_t flags = (wait_all_bits ? S31_EG_FLAG_ALL_BITS : 0) |
		(clear_on_exit ? S31_EG_FLAG_CLEAR : 0);

	if (!g || !g->wait_context)
		return 0;
	{
		uint32_t n = ++s31_event_wait_count;

		if (n <= 64)
			s31_linux_printf("[S31] event wait #%u g=%p want=%08x clear=%d all=%d timeout=%u task=%s\n",
			       n, g, bits, clear_on_exit, wait_all_bits,
			       timeout, s31_event_task_name());
	}
	for (;;) {
		EventBits_t result;
		TickType_t remaining;
		uint32_t seq = s31_linux_sync_sequence(g->wait_context);
		int32_t waited;

		s31_linux_sync_lock(g->wait_context);
		result = g->bits;
		if (s31_eg_match(result, bits, flags)) {
			if (clear_on_exit)
				g->bits &= ~bits;
			s31_linux_sync_unlock(g->wait_context);
			if (s31_event_wait_count <= 64)
				s31_linux_printf("[S31] event wait done g=%p result=%08x task=%s\n",
				       g, result, s31_event_task_name());
			return result;
		}
		s31_linux_sync_unlock(g->wait_context);
		if (!timeout || s31_rtos_in_isr())
			return result;
		if (timeout == portMAX_DELAY)
			remaining = portMAX_DELAY;
		else {
			TickType_t elapsed = s31_rtos_get_tick() - start;

			remaining = elapsed >= timeout ? 0 : timeout - elapsed;
		}
		if (!remaining)
			return result;
		waited = s31_linux_sync_wait(g->wait_context, seq, remaining,
					     S31_BLOB_RELEASE_EVENT_WAIT);
		if (waited <= 0)
		{
			/* Recheck once after the deadline: the matching set and the
			 * timeout can race, and FreeRTOS evaluates the event bits when
			 * the blocked task resumes. */
			s31_linux_sync_lock(g->wait_context);
			result = g->bits;
			if (s31_eg_match(result, bits, flags) && clear_on_exit)
				g->bits &= ~bits;
			s31_linux_sync_unlock(g->wait_context);
			s31_linux_printf("[S31] event wait timeout g=%p want=%08x timeout=%u waited=%d have=%08x task=%s\n",
			       g, bits, timeout, waited, result,
			       s31_event_task_name());
			return result;
		}
	}
}
