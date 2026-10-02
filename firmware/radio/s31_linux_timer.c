/* SPDX-License-Identifier: Apache-2.0 */
/* esp_timer compatibility backed by the Linux radio worker's 10 ms tick. */
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "esp_timer.h"
#include "s31_rtos/s31_rtos.h"

struct esp_timer {
	esp_timer_cb_t callback;
	void *arg;
	const char *name;
	uint64_t alarm_us;
	uint64_t period_us;
	uint64_t late_total_us;
	uint64_t late_max_us;
	uint64_t last_start_us;
	uint64_t last_timeout_us;
	uint32_t callbacks;
	uint32_t starts;
	uint32_t stops;
	bool active;
	struct esp_timer *next;
};

static struct esp_timer *s31_timers;

static uint64_t s31_timer_now_us(void)
{
	/* Use the Linux monotonic clock directly.  s31_linux_time_ns() is
	 * ktime_get_mono_fast_ns(); 1 us resolution keeps esp_timer_start_once
	 * and friends from seeing 10 ms quantization while the gate is held. */
	return (uint64_t)(s31_linux_time_ns() / 1000ULL);
}

esp_err_t __wrap_esp_timer_create(const esp_timer_create_args_t *args,
					 esp_timer_handle_t *out)
{
	struct esp_timer *timer;

	if (!args || !args->callback || !out)
		return ESP_ERR_INVALID_ARG;
	timer = s31_rtos_malloc(sizeof(*timer));
	if (!timer)
		return ESP_ERR_NO_MEM;
	memset(timer, 0, sizeof(*timer));
	timer->callback = args->callback;
	timer->arg = args->arg;
	timer->name = args->name;
	timer->next = s31_timers;
	s31_timers = timer;
	*out = timer;
	return ESP_OK;
}

esp_err_t __wrap_esp_timer_delete(esp_timer_handle_t timer)
{
	struct esp_timer **link = &s31_timers;

	while (*link && *link != timer)
		link = &(*link)->next;
	if (!*link)
		return ESP_ERR_INVALID_ARG;
	*link = timer->next;
	s31_rtos_free(timer);
	return ESP_OK;
}

static esp_err_t s31_timer_start(struct esp_timer *timer, uint64_t alarm_us,
				 uint64_t period_us)
{
	uint64_t now_us;

	if (!timer || !alarm_us)
		return ESP_ERR_INVALID_ARG;
	if (timer->active)
		return ESP_ERR_INVALID_STATE;
	now_us = s31_timer_now_us();
	timer->alarm_us = alarm_us;
	timer->period_us = period_us;
	timer->last_start_us = now_us;
	timer->last_timeout_us = alarm_us > now_us ? alarm_us - now_us : 0;
	timer->active = true;
	timer->starts++;
	return ESP_OK;
}

esp_err_t __wrap_esp_timer_start_once(esp_timer_handle_t timer,
				      uint64_t timeout_us)
{
	return s31_timer_start(timer, s31_timer_now_us() + timeout_us, 0);
}

esp_err_t __wrap_esp_timer_start_once_at(esp_timer_handle_t timer,
					 uint64_t alarm_us)
{
	if (alarm_us <= s31_timer_now_us())
		return ESP_ERR_INVALID_ARG;
	return s31_timer_start(timer, alarm_us, 0);
}

esp_err_t __wrap_esp_timer_start_periodic(esp_timer_handle_t timer,
					  uint64_t period_us)
{
	return s31_timer_start(timer, s31_timer_now_us() + period_us, period_us);
}

esp_err_t __wrap_esp_timer_start_periodic_at(esp_timer_handle_t timer,
					     uint64_t period_us,
					     uint64_t first_alarm_us)
{
	if (first_alarm_us <= s31_timer_now_us())
		return ESP_ERR_INVALID_ARG;
	return s31_timer_start(timer, first_alarm_us, period_us);
}

esp_err_t __wrap_esp_timer_stop(esp_timer_handle_t timer)
{
	if (!timer)
		return ESP_ERR_INVALID_ARG;
	if (!timer->active)
		return ESP_ERR_INVALID_STATE;
	timer->active = false;
	timer->stops++;
	return ESP_OK;
}

esp_err_t __wrap_esp_timer_restart(esp_timer_handle_t timer,
				   uint64_t timeout_us)
{
	if (!timer || !timer->active || !timeout_us)
		return timer ? ESP_ERR_INVALID_STATE : ESP_ERR_INVALID_ARG;
	timer->alarm_us = s31_timer_now_us() + timeout_us;
	timer->last_start_us = s31_timer_now_us();
	timer->last_timeout_us = timeout_us;
	if (timer->period_us)
		timer->period_us = timeout_us;
	return ESP_OK;
}

bool __wrap_esp_timer_is_active(esp_timer_handle_t timer)
{
	return timer && timer->active;
}

int64_t __wrap_esp_timer_get_time(void)
{
	return (int64_t)s31_timer_now_us();
}

void s31_linux_timers_tick(void)
{
	unsigned int callbacks = 0;
	struct esp_timer *timer;
	uint64_t now_us;

	/* A callback is allowed to delete any timer, including the next element
	 * in the list.  Re-scan after each dispatch instead of retaining a pointer
	 * across the callback.  This matches esp_timer's serialized task dispatch
	 * and avoids losing the Wi-Fi BA reorder timeout to a stale list link. */
	for (;;) {
		esp_timer_cb_t callback;
		void *arg;
		struct esp_timer *candidate;

		now_us = s31_timer_now_us();
		timer = NULL;
		for (candidate = s31_timers; candidate;
		     candidate = candidate->next) {
			if (candidate->active && candidate->alarm_us <= now_us) {
				timer = candidate;
				break;
			}
		}
		if (!timer || callbacks++ == 64)
			break;

		callback = timer->callback;
		arg = timer->arg;
		{
			uint64_t late_us = now_us - timer->alarm_us;

			timer->callbacks++;
			timer->late_total_us += late_us;
			if (late_us > timer->late_max_us)
				timer->late_max_us = late_us;
		}
		if (timer->period_us) {
			uint64_t elapsed = now_us - timer->alarm_us;

			timer->alarm_us +=
				(elapsed / timer->period_us + 1) * timer->period_us;
		} else {
			timer->active = false;
		}
		callback(arg);
	}
}

uint32_t s31_linux_timer_next_due_us(void)
{
	struct esp_timer *timer;
	uint64_t now_us = s31_timer_now_us();
	uint64_t next_us = UINT32_MAX;

	for (timer = s31_timers; timer; timer = timer->next) {
		uint64_t delta;

		if (!timer->active)
			continue;
		if (timer->alarm_us <= now_us)
			return 1;
		delta = timer->alarm_us - now_us;
		if (delta < next_us)
			next_us = delta;
	}

	return (uint32_t)next_us;
}

void s31_linux_timer_report(void)
{
	struct esp_timer *timer;
	uint64_t now_us = s31_timer_now_us();

	for (timer = s31_timers; timer; timer = timer->next) {
		uint64_t average = timer->callbacks ?
			timer->late_total_us / timer->callbacks : 0;
		uint64_t due = timer->active && timer->alarm_us > now_us ?
			timer->alarm_us - now_us : 0;
		uint64_t age = timer->last_start_us && now_us > timer->last_start_us ?
			now_us - timer->last_start_us : 0;

		/* This payload uses ILP32 varargs.  Keep diagnostic integers at native
		 * unsigned-long width instead of passing 64-bit values through the
		 * Linux printk bridge. */
		s31_linux_printf("[S31] timer name=%s cb=%08lx active=%u period=%lu "
				 "starts=%lu stops=%lu calls=%lu late-avg=%luus "
				 "late-max=%luus last-timeout=%luus age=%luus due=%luus\n",
				 timer->name ? timer->name : "?",
				 (unsigned long)timer->callback,
				 timer->active ? 1U : 0U,
				 (unsigned long)timer->period_us,
				 (unsigned long)timer->starts,
				 (unsigned long)timer->stops,
				 (unsigned long)timer->callbacks,
				 (unsigned long)average,
				 (unsigned long)timer->late_max_us,
				 (unsigned long)timer->last_timeout_us,
				 (unsigned long)age,
				 (unsigned long)due);
	}
}
