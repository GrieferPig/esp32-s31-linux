/* SPDX-License-Identifier: Apache-2.0 */
/* esp_timer compatibility with independent metadata and dispatcher ownership. */
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "esp_timer.h"
#include "rom/ets_sys.h"
#include "s31_rtos/s31_rtos.h"

struct esp_timer {
	esp_timer_cb_t callback;
	void *arg;
	uint64_t alarm_us;
	uint64_t period_us;
	bool active;
	bool skip_unhandled_events;
	struct esp_timer *next;
};

extern uint64_t esp_timer_impl_get_min_period_us(void);
uint64_t s31_linux_timer_next_deadline_ns(void);

static struct esp_timer *s31_timers;
/* Match the IDF portMUX owner/count ABI. This lock protects only timer
 * metadata, never allocation, logging, callbacks or a scheduler wait. */
static struct { uint32_t owner, count; } s31_timer_mux = { 0xb33fffffU, 0 };
static bool s31_timer_dispatching;
static struct s31_task_cleanup s31_timer_dispatch_cleanup;
extern BaseType_t xPortEnterCriticalTimeout(void *mux, BaseType_t timeout);
extern void vPortExitCriticalMultiCore(void *mux);

static void s31_timer_lock(void)
{
	if (!xPortEnterCriticalTimeout(&s31_timer_mux, -1))
		s31_linux_sync_owner(NULL, S31_SYNC_OWNER_FATAL);
}
static void s31_timer_unlock(void)
{
	vPortExitCriticalMultiCore(&s31_timer_mux);
}

/* IRQ/process hint readers never follow timer pointers.
 * All shared words are naturally aligned lock-free 32-bit atomics on RV32.
 * The list itself is protected by s31_timer_mux, independently of the gate. */
_Static_assert(__atomic_always_lock_free(sizeof(uint32_t), 0),
			   "timer snapshot requires lock-free 32-bit atomics");
static uint32_t s31_timer_mutation_generation;
static uint32_t s31_timer_snapshot_sequence;
static uint32_t s31_timer_snapshot_generation;
static uint32_t s31_timer_snapshot_present;
static uint32_t s31_timer_snapshot_alarm_lo;
static uint32_t s31_timer_snapshot_alarm_hi;

static uint32_t s31_timer_update_begin(void)
{
	/* Invalidate the old hint before any list/alarm mutation becomes visible. */
	return __atomic_add_fetch(&s31_timer_mutation_generation, 1,
							  __ATOMIC_ACQ_REL);
}

static void s31_timer_snapshot_publish(uint32_t generation)
{
	struct esp_timer *timer;
	uint64_t alarm = UINT64_MAX;
	uint32_t present = 0;
	uint32_t sequence;

	/* The timer mux serializes writers/scans, not lock-free hint readers. */
	for (timer = s31_timers; timer; timer = timer->next) {
		if (timer->active && (!present || timer->alarm_us < alarm)) {
			present = 1;
			alarm = timer->alarm_us;
		}
	}
	if (__atomic_load_n(&s31_timer_mutation_generation, __ATOMIC_ACQUIRE)
		!= generation)
		return;
	/* There is exactly one writer under the timer mux. Hint readers remain
	 * bounded even when publication is interrupted by a higher IRQ. */
	sequence = __atomic_fetch_add(&s31_timer_snapshot_sequence, 1, __ATOMIC_ACQ_REL);
	__atomic_store_n(&s31_timer_snapshot_generation, generation, __ATOMIC_RELAXED);
	__atomic_store_n(&s31_timer_snapshot_present, present, __ATOMIC_RELAXED);
	__atomic_store_n(&s31_timer_snapshot_alarm_lo, (uint32_t)alarm, __ATOMIC_RELAXED);
	__atomic_store_n(&s31_timer_snapshot_alarm_hi, (uint32_t)(alarm >> 32), __ATOMIC_RELAXED);
	__atomic_store_n(&s31_timer_snapshot_sequence, sequence + 2, __ATOMIC_RELEASE);
}

/* Start a fresh runtime only after prior tasks/work/IRQs have joined and
 * the shared SRAM pool reclaimed every retained timer allocation.
 * Old links belong to that reclaimed domain; never dereference or free them. */
void s31_rtos_timer_runtime_reset(void)
{
	s31_timers = NULL;
	s31_timer_mux.owner = 0xb33fffffU;
	s31_timer_mux.count = 0;
	s31_timer_dispatching = false;
	memset(&s31_timer_dispatch_cleanup, 0, sizeof(s31_timer_dispatch_cleanup));
	__atomic_store_n(&s31_timer_mutation_generation, 0, __ATOMIC_RELAXED);
	__atomic_store_n(&s31_timer_snapshot_generation, 0, __ATOMIC_RELAXED);
	__atomic_store_n(&s31_timer_snapshot_present, 0, __ATOMIC_RELAXED);
	__atomic_store_n(&s31_timer_snapshot_alarm_lo, 0, __ATOMIC_RELAXED);
	__atomic_store_n(&s31_timer_snapshot_alarm_hi, 0, __ATOMIC_RELAXED);
	__atomic_store_n(&s31_timer_snapshot_sequence, 0, __ATOMIC_RELEASE);
}

static uint64_t s31_timer_now_us(void)
{
	/* Use the Linux monotonic clock directly.  s31_linux_time_ns() is
	 * ktime_get_mono_fast_ns(); 1 us resolution preserves the esp_timer API epoch without jiffies
	 * quantization. */
	return (uint64_t)(s31_linux_time_ns() / 1000ULL);
}

esp_err_t __wrap_esp_timer_create(const esp_timer_create_args_t *args,
					 esp_timer_handle_t *out)
{
	struct esp_timer *timer;
	uint32_t generation;

	if (!args || !args->callback || !out)
		return ESP_ERR_INVALID_ARG;
	timer = s31_rtos_malloc(sizeof(*timer));
	if (!timer)
		return ESP_ERR_NO_MEM;
	memset(timer, 0, sizeof(*timer));
	timer->callback = args->callback;
	timer->arg = args->arg;
	timer->skip_unhandled_events = args->skip_unhandled_events;
	s31_timer_lock();
	generation = s31_timer_update_begin();
	timer->next = s31_timers;
	s31_timers = timer;
	s31_timer_snapshot_publish(generation);
	s31_timer_unlock();
	*out = timer;
	return ESP_OK;
}

static esp_err_t s31_timer_delete(esp_timer_handle_t timer, bool cancel,
				 ETSTimer *legacy)
{
	struct esp_timer **link;
	uint32_t generation;

	s31_timer_lock();
	for (link = &s31_timers; *link && *link != timer; link = &(*link)->next) {
		/* Locate the link before validating and unlinking the timer. */
	}
	if (!*link) {
		s31_timer_unlock();
		return ESP_ERR_INVALID_ARG;
	}
	if (timer->active && !cancel) {
		s31_timer_unlock();
		return ESP_ERR_INVALID_STATE;
	}
	if (legacy && legacy->timer_arg != timer) {
		s31_timer_unlock();
		return ESP_ERR_INVALID_STATE;
	}
	generation = s31_timer_update_begin();
	*link = timer->next;
	if (legacy) {
		legacy->timer_arg = NULL;
		legacy->timer_expire = 0;
	}
	s31_timer_snapshot_publish(generation);
	s31_timer_unlock();
	s31_linux_sync_owner(NULL, S31_SYNC_TIMER_CHANGED);
	/* Dispatch copies callback/arg while locked and never dereferences the
	 * selected object afterward, including a callback deleting itself. */
	s31_rtos_free(timer);
	return ESP_OK;
}

esp_err_t __wrap_esp_timer_delete(esp_timer_handle_t timer)
{
	return s31_timer_delete(timer, false, NULL);
}

/* The SDK legacy done adapter forgets its handle even when public delete
 * refuses an active alarm. Finalize that legacy owner atomically with alarm
 * cancellation; keep the public stopped-only delete contract unchanged. */
void __wrap_ets_timer_done(ETSTimer *timer)
{
	if (!timer || timer->timer_expire != 0x12121212U)
		return;
	if (timer->timer_arg)
		(void)s31_timer_delete((esp_timer_handle_t)timer->timer_arg, true, timer);
	else
		timer->timer_expire = 0;
}
void __wrap_os_timer_done(ETSTimer *timer)
	__attribute__((alias("__wrap_ets_timer_done")));

/* The SDK legacy arm adapter implements replacement with separate public
 * stop/start calls. A native task or an admitted radio IRQ can rearm the same
 * timer between those calls. Serialize replacement itself with timer metadata,
 * not with all vendor execution. Public esp_timer_start_* retains its active
 * timer rejection. The legacy owner and registered timer are checked together
 * with done/delete, before any timer object is dereferenced. */
static void __attribute__((section(".text.__wrap_ets_timer_arm_us")))
s31_timer_legacy_rearm(ETSTimer *legacy, uint64_t timeout_us,
                                  bool repeat)
{
    struct esp_timer *timer;
    uint64_t now_us;
    uint32_t generation;

    if (!legacy) {
        s31_linux_sync_owner(NULL, S31_SYNC_OWNER_FATAL);
        return;
    }
    if (repeat) {
        uint64_t minimum = esp_timer_impl_get_min_period_us();
        if (timeout_us < minimum) timeout_us = minimum;
    }
    s31_timer_lock();
    for (timer = s31_timers; timer && timer != legacy->timer_arg;
         timer = timer->next) {
        /* Locate the timer before validating its legacy owner. */
    }
    if (legacy->timer_expire != 0x12121212U || !timer) {
        s31_timer_unlock();
        s31_linux_sync_owner(NULL, S31_SYNC_OWNER_FATAL);
        return;
    }
    now_us = s31_timer_now_us();
    generation = s31_timer_update_begin();

    timer->alarm_us = now_us + timeout_us;
    timer->period_us = repeat ? timeout_us : 0;

    timer->active = true;

    s31_timer_snapshot_publish(generation);
    s31_timer_unlock();
    s31_linux_sync_owner(NULL, S31_SYNC_TIMER_CHANGED);
}

void __wrap_ets_timer_arm_us(ETSTimer *timer, uint32_t timeout_us, bool repeat)
{
    s31_timer_legacy_rearm(timer, timeout_us, repeat);
}
void __wrap_os_timer_arm_us(ETSTimer *timer, uint32_t timeout_us, bool repeat)
    __attribute__((alias("__wrap_ets_timer_arm_us")));

void __wrap_ets_timer_arm(ETSTimer *timer, uint32_t timeout_ms, bool repeat)
{
    s31_timer_legacy_rearm(timer, (uint64_t)timeout_ms * 1000ULL, repeat);
}
void __wrap_os_timer_arm(ETSTimer *timer, uint32_t timeout_ms, bool repeat)
    __attribute__((alias("__wrap_ets_timer_arm")));

static esp_err_t s31_timer_start(struct esp_timer *timer, uint64_t alarm_us,
				 uint64_t period_us)
{
	uint32_t generation;

	if (!timer || !alarm_us)
		return ESP_ERR_INVALID_ARG;
	s31_timer_lock();
	if (timer->active) {
		s31_timer_unlock();
		return ESP_ERR_INVALID_STATE;
	}
	generation = s31_timer_update_begin();
	timer->alarm_us = alarm_us;
	timer->period_us = period_us;

	timer->active = true;

	s31_timer_snapshot_publish(generation);
	s31_timer_unlock();
	s31_linux_sync_owner(NULL, S31_SYNC_TIMER_CHANGED);
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
	uint64_t now = s31_timer_now_us();
	if (alarm_us < now && now - alarm_us > esp_timer_impl_get_min_period_us())
		return ESP_ERR_INVALID_ARG;
	return s31_timer_start(timer, alarm_us, 0);
}

esp_err_t __wrap_esp_timer_start_periodic(esp_timer_handle_t timer,
					  uint64_t period_us)
{
	uint64_t minimum = esp_timer_impl_get_min_period_us();
	if (period_us < minimum) period_us = minimum;
	return s31_timer_start(timer, s31_timer_now_us() + period_us, period_us);
}

esp_err_t __wrap_esp_timer_start_periodic_at(esp_timer_handle_t timer,
					     uint64_t period_us,
					     uint64_t first_alarm_us)
{
	uint64_t now = s31_timer_now_us();
	uint64_t minimum = esp_timer_impl_get_min_period_us();
	if (first_alarm_us < now && now - first_alarm_us > minimum)
		return ESP_ERR_INVALID_ARG;
	if (period_us < minimum) period_us = minimum;
	return s31_timer_start(timer, first_alarm_us, period_us);
}

esp_err_t __wrap_esp_timer_stop(esp_timer_handle_t timer)
{
	uint32_t generation;
	if (!timer)
		return ESP_ERR_INVALID_ARG;
	s31_timer_lock();
	if (!timer->active) {
		s31_timer_unlock();
		return ESP_ERR_INVALID_STATE;
	}
	generation = s31_timer_update_begin();
	timer->active = false;

	s31_timer_snapshot_publish(generation);
	s31_timer_unlock();
	s31_linux_sync_owner(NULL, S31_SYNC_TIMER_CHANGED);
	return ESP_OK;
}

esp_err_t __wrap_esp_timer_restart(esp_timer_handle_t timer,
				   uint64_t timeout_us)
{
	uint32_t generation;
	uint64_t now_us;

	if (!timer)
		return ESP_ERR_INVALID_ARG;
	s31_timer_lock();
	if (!timer->active) {
		s31_timer_unlock();
		return ESP_ERR_INVALID_STATE;
	}
	generation = s31_timer_update_begin();
	if (timer->period_us) {
		uint64_t minimum = esp_timer_impl_get_min_period_us();
		if (timeout_us < minimum) timeout_us = minimum;
	}
	now_us = s31_timer_now_us();
	timer->alarm_us = now_us + timeout_us;

	if (timer->period_us)
		timer->period_us = timeout_us;
	s31_timer_snapshot_publish(generation);
	s31_timer_unlock();
	s31_linux_sync_owner(NULL, S31_SYNC_TIMER_CHANGED);
	return ESP_OK;
}

bool __wrap_esp_timer_is_active(esp_timer_handle_t timer)
{
	bool active;
	s31_timer_lock();
	active = timer && timer->active;
	s31_timer_unlock();
	return active;
}

int64_t __wrap_esp_timer_get_time(void)
{
	return (int64_t)s31_timer_now_us();
}

/* Owner-side cleanup also releases a dispatcher whose managed callback exits
 * nonlocally. No task can inherit this lease from a suspended callback. */
static void s31_timer_dispatch_release(struct s31_task_cleanup *cleanup)
{
	(void)cleanup;
	s31_timer_lock();
	s31_timer_dispatching = false;
	s31_timer_unlock();
}

/* Task-dispatch work is cache-on, like the callbacks it invokes. Keep the
 * infrequent list walk out of the SRAM fast path used by packet passes. */
static __attribute__((noinline, noclone)) void s31_timer_dispatch_due(void)
{
	unsigned int callbacks = 0;
	struct esp_timer *timer;
	uint64_t now_us;

	s31_timer_lock();
	if (s31_timer_dispatching) {
		s31_timer_unlock();
		return;
	}
	s31_timer_dispatching = true;
	s31_timer_unlock();
	s31_timer_dispatch_cleanup.release = s31_timer_dispatch_release;
	if (!s31_rtos_cleanup_push(&s31_timer_dispatch_cleanup)) {
		s31_timer_dispatch_release(&s31_timer_dispatch_cleanup);
		return;
	}

	/* A callback is allowed to delete any timer, including the next element
	 * in the list.  Re-scan after each dispatch instead of retaining a pointer
	 * across the callback.  This matches esp_timer's serialized task dispatch
	 * and avoids losing the Wi-Fi BA reorder timeout to a stale list link. */
	for (;;) {
		esp_timer_cb_t callback;
		void *arg;
		struct esp_timer *candidate;
		uint32_t generation;

		s31_timer_lock();
		now_us = s31_timer_now_us();
		timer = NULL;
		for (candidate = s31_timers; candidate;
		     candidate = candidate->next) {
			if (candidate->active && candidate->alarm_us <= now_us &&
			    (!timer || candidate->alarm_us < timer->alarm_us)) {
				timer = candidate;
			}
		}
		if (!timer || callbacks++ == 64) {
			s31_timer_unlock();
			break;
		}

	generation = s31_timer_update_begin();
		callback = timer->callback;
		arg = timer->arg;
		{

		}
		if (timer->period_us) {
			uint64_t elapsed = now_us - timer->alarm_us;

			/* Match IDF: ordinary periodic timers catch up one deadline at
			 * a time; only skip_unhandled_events suppresses a backlog. The
			 * existing 64-callback batch bounds each service pass. */
			if (timer->skip_unhandled_events && elapsed / timer->period_us > 1)
				timer->alarm_us = now_us + timer->period_us;
			else
				timer->alarm_us += timer->period_us;
		} else {
			timer->active = false;
		}
	s31_timer_snapshot_publish(generation);
		s31_timer_unlock();
		callback(arg);
	}
	s31_rtos_cleanup_pop(&s31_timer_dispatch_cleanup);
	s31_timer_dispatch_release(&s31_timer_dispatch_cleanup);
}

void s31_linux_timers_tick(void)
{
	/* Most radio passes carry packets/IRQs, not expired timers. The bounded
	 * generation-checked hint returns zero if due or unstable. A mutation
	 * publishes a wake, and the dispatcher rechecks its deadline before sleep. */
	if (s31_linux_timer_next_deadline_ns() == 0)
		s31_timer_dispatch_due();
}

uint64_t s31_linux_timer_next_deadline_ns(void)
{
	uint32_t generation = __atomic_load_n(&s31_timer_mutation_generation,
										  __ATOMIC_ACQUIRE);
	uint32_t sequence = __atomic_load_n(&s31_timer_snapshot_sequence,
										__ATOMIC_ACQUIRE);
	uint32_t published, present, lo, hi, after, current_generation;
	uint64_t alarm, now;

	if (sequence & 1)
		return 0;
	published = __atomic_load_n(&s31_timer_snapshot_generation, __ATOMIC_RELAXED);
	present = __atomic_load_n(&s31_timer_snapshot_present, __ATOMIC_RELAXED);
	lo = __atomic_load_n(&s31_timer_snapshot_alarm_lo, __ATOMIC_RELAXED);
	hi = __atomic_load_n(&s31_timer_snapshot_alarm_hi, __ATOMIC_RELAXED);
	__atomic_thread_fence(__ATOMIC_ACQUIRE);
	after = __atomic_load_n(&s31_timer_snapshot_sequence, __ATOMIC_RELAXED);
	current_generation = __atomic_load_n(&s31_timer_mutation_generation,
										 __ATOMIC_ACQUIRE);
	/* Bounded one-shot read: a high-priority/IRQ reader cannot starve a
	 * preempted writer by spinning on a sequence that only it can finish. */
	if (sequence != after || generation != current_generation ||
		published != current_generation)
		return 0;
	if (!present)
		return UINT64_MAX;
	alarm = ((uint64_t)hi << 32) | lo;
	now = s31_timer_now_us();
	if (alarm <= now)
		return 0;
	/* Preserve the Linux monotonic epoch across the payload/driver boundary.
	 * Zero is runnable service; UINT64_MAX is no timer. Saturate extremely
	 * distant active alarms below Linux's signed KTIME_MAX sentinel. */
	return alarm >= (uint64_t)INT64_MAX / 1000 ?
		(uint64_t)INT64_MAX - 1 : alarm * 1000;
}
