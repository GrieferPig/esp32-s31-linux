/* SPDX-License-Identifier: BSD-2-Clause */
/* FreeRTOS event groups backed by the Linux synchronization bridge. */
#include <stdint.h>
#include <string.h>
#include "s31_rtos.h"

#define S31_EG_FLAG_ALL_BITS (1U << 0)
#define S31_EG_FLAG_CLEAR    (1U << 1)

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
	uint32_t flags = (wait_all_bits ? S31_EG_FLAG_ALL_BITS : 0) |
		(clear_on_exit ? S31_EG_FLAG_CLEAR : 0);

	if (!g || !g->wait_context)
		return 0;
	for (;;) {
		EventBits_t result;
		uint32_t seq = s31_linux_sync_sequence(g->wait_context);
		int32_t waited;

		s31_linux_sync_lock(g->wait_context);
		result = g->bits;
		if (s31_eg_match(result, bits, flags)) {
			if (clear_on_exit)
				g->bits &= ~bits;
			s31_linux_sync_unlock(g->wait_context);
			return result;
		}
		s31_linux_sync_unlock(g->wait_context);
		if (!timeout || s31_rtos_in_isr())
			return 0;
		waited = s31_linux_sync_wait(g->wait_context, seq, timeout);
		if (waited <= 0)
			return 0;
		if (timeout != portMAX_DELAY)
			timeout = 0;
	}
}
