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
	uint64_t alarm_us;
	uint64_t period_us;
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
	if (!timer || !alarm_us)
		return ESP_ERR_INVALID_ARG;
	if (timer->active)
		return ESP_ERR_INVALID_STATE;
	timer->alarm_us = alarm_us;
	timer->period_us = period_us;
	timer->active = true;
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
	return ESP_OK;
}

esp_err_t __wrap_esp_timer_restart(esp_timer_handle_t timer,
				   uint64_t timeout_us)
{
	if (!timer || !timer->active || !timeout_us)
		return timer ? ESP_ERR_INVALID_STATE : ESP_ERR_INVALID_ARG;
	timer->alarm_us = s31_timer_now_us() + timeout_us;
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

/* Legacy hard-IRQ advance hook.  Time is now read from the Linux monotonic
 * clock in s31_timer_now_us(), so there is no separate epoch to advance. */
void s31_linux_timer_advance(void)
{
}

void s31_linux_timers_tick(void)
{
	struct esp_timer *timer, *next;
	uint64_t now_us = s31_timer_now_us();

	for (timer = s31_timers; timer; timer = next) {
		esp_timer_cb_t callback;
		void *arg;

		next = timer->next;
		if (!timer->active || timer->alarm_us > now_us)
			continue;
		callback = timer->callback;
		arg = timer->arg;
		if (timer->period_us)
			timer->alarm_us += timer->period_us;
		else
			timer->active = false;
		callback(arg);
	}
}
