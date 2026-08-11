/*
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * S31 radio world stub layer: event groups.
 * See docs/S31_radio_rtos_stub.md.
 */
#include <stdint.h>
#include <string.h>
#include "s31_rtos.h"

#define S31_EG_FLAG_ALL_BITS (1U << 0)
#define S31_EG_FLAG_CLEAR    (1U << 1)

void *xEventGroupCreate(void)
{
	struct s31_event_group *g = s31_rtos_malloc(sizeof(*g));

	if (!g)
		return NULL;
	memset(g, 0, sizeof(*g));
	return g;
}

void vEventGroupDelete(void *group)
{
	struct s31_event_group *g = group;
	struct s31_waiter *w = g->waiters;

	while (w) {
		struct s31_waiter *next = w->next;

		w->tcb->wait_obj = NULL;
		s31_rtos_make_ready(w->tcb);
		s31_rtos_free(w);
		w = next;
	}
	s31_rtos_free(g);
}

static int s31_eg_match(const struct s31_waiter *w, EventBits_t bits)
{
	if (w->flags & S31_EG_FLAG_ALL_BITS)
		return (bits & w->want) == w->want;
	return (bits & w->want) != 0;
}

EventBits_t xEventGroupSetBits(void *group, EventBits_t bits)
{
	struct s31_event_group *g = group;
	struct s31_waiter **pp;

	g->bits |= bits;
	pp = &g->waiters;
	while (*pp) {
		struct s31_waiter *w = *pp;

		if (s31_eg_match(w, g->bits)) {
			EventBits_t result = g->bits;

			*pp = w->next;
			w->tcb->wait_result = result;
			if (w->flags & S31_EG_FLAG_CLEAR)
				g->bits &= ~w->want;
			w->tcb->wait_obj = NULL;
			s31_rtos_make_ready(w->tcb);
			s31_rtos_free(w);
			continue;
		}
		pp = &w->next;
	}
	return g->bits;
}

EventBits_t xEventGroupClearBits(void *group, EventBits_t bits)
{
	struct s31_event_group *g = group;
	EventBits_t previous = g->bits;

	g->bits &= ~bits;
	return previous;
}

EventBits_t xEventGroupWaitBits(void *group, EventBits_t bits,
				BaseType_t clear_on_exit,
				BaseType_t wait_all_bits,
				TickType_t timeout)
{
	struct s31_event_group *g = group;
	struct s31_tcb *t = s31_rtos_current();
	struct s31_waiter *w;
	EventBits_t have;

	if (!g)
		return 0;
	have = g->bits & bits;
	if (wait_all_bits ? have == bits : have != 0) {
		EventBits_t result = g->bits;

		if (clear_on_exit)
			g->bits &= ~bits;
		return result;
	}
	if (timeout == 0 || !t)
		return 0;
	w = s31_rtos_malloc(sizeof(*w));
	if (!w)
		return 0;
	w->tcb = t;
	w->dir = 0;
	w->want = bits;
	w->flags = (wait_all_bits ? S31_EG_FLAG_ALL_BITS : 0) |
		   (clear_on_exit ? S31_EG_FLAG_CLEAR : 0);
	w->next = g->waiters;
	g->waiters = w;
	s31_rtos_block(2, g, timeout, 0);
	/* A setter captures the pre-clear value; timeout leaves it zero. */
	return t->wait_result;
}
