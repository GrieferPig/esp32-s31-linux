/*
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * S31 radio world FreeRTOS ABI shim.  The old cooperative scheduler used a
 * hand-written ILP32F context switch and a single ready queue.  That is not a
 * valid model once the Wi-Fi blob has more than one Linux execution context:
 * Linux now owns task stacks, scheduling, and blocking through the opaque
 * bridge declared in s31_rtos.h.
 */
#include <stdint.h>
#include <string.h>
#include "s31_rtos.h"



uint32_t s31_rtos_isr_depth;

static volatile TickType_t s31_tick;
static volatile uint32_t s31_scheduler_started;
static volatile uint32_t s31_orphan_critical_depth;
static volatile uint32_t s31_orphan_critical_flags;
static uint32_t s31_task_return_count;
static uint32_t s31_task_delete_count;
static uint32_t s31_task_yield_count;
static uint32_t s31_task_delay_count;
static uint32_t s31_task_priority_set_count;
/* Linux's serialized radio worker executes the deferred Wi-Fi ISR, esp_timer
 * callbacks, and direct data-path calls.  Native FreeRTOS always has a
 * pxCurrentTCB in those contexts (an ISR observes the interrupted task), while
 * the kthread bridge previously returned NULL.  Keep a stable internal-SRAM
 * identity for all non-compatibility worker entries so mutex ownership and
 * pthread TLS retain FreeRTOS semantics. */
static struct s31_tcb s31_foreign_tcb;

/* --- radio heap (heap_caps in the same world) --- */
void *s31_rtos_malloc(uint32_t size)
{
	 extern void *heap_caps_malloc(uint32_t size, uint32_t caps);

	return heap_caps_malloc(size, 0x804 /* INTERNAL | 8BIT */);
}

/*
 * A task stack must remain accessible while the PHY/controller changes the
 * external-memory/cache state (RF calibration, BT enable).  ESP-IDF likewise
 * keeps controller and Wi-Fi task stacks in internal byte-addressable RAM;
 * a DRAM stack is unreachable inside that window and the payload stalls.
 */
static void *s31_rtos_stack_malloc(uint32_t size)
{
	 extern void *heap_caps_malloc(uint32_t size, uint32_t caps);

	/* The caller has already expanded the advertised stack size to include
	 * the Linux bridge headroom.  Keeping that adjustment outside this
	 * allocator is essential: the trampoline must put SP at the end of the
	 * complete allocation, not at the end of the smaller IDF request. */
	return heap_caps_malloc(size, 0x804 /* INTERNAL | 8BIT */);
}

void s31_rtos_use_internal_stacks(void)
{
	/* Linux kthread stacks are kernel-owned.  Kept as an ABI no-op for the
	 * non-Wi-Fi callers which still reference this measured symbol. */
}

void s31_rtos_free(void *ptr)
{
	 extern void heap_caps_free(void *ptr);

	heap_caps_free(ptr);
}

/* --- critical sections --- */
void vPortEnterCritical(void)
{
	struct s31_tcb *t = s31_rtos_current();

	if (!t) {
		if (s31_orphan_critical_depth++ == 0)
			s31_orphan_critical_flags = s31_linux_critical_enter();
		return;
	}
	if (t->critical_depth++ == 0)
		t->critical_flags = s31_linux_critical_enter();
}

void vPortExitCritical(void)
{
	struct s31_tcb *t = s31_rtos_current();

	if (!t) {
		if (s31_orphan_critical_depth && --s31_orphan_critical_depth == 0)
			s31_linux_critical_exit(s31_orphan_critical_flags);
		return;
	}
	if (t->critical_depth && --t->critical_depth == 0)
		s31_linux_critical_exit(t->critical_flags);
}

/* IDF's SMP port spells the same critical-section contract through a
 * portMUX pointer. Blob execution is already serialized by the Linux gate;
 * retain FreeRTOS nesting/IRQ semantics through the per-task implementation
 * above and treat the mux storage as opaque. */
BaseType_t xPortEnterCriticalTimeout(void *mux, BaseType_t timeout)
{
	(void)mux;
	(void)timeout;
	vPortEnterCritical();
	return pdTRUE;
}

void vPortExitCriticalMultiCore(void *mux)
{
	(void)mux;
	vPortExitCritical();
}

BaseType_t s31_rtos_in_isr(void)
{
	return s31_rtos_isr_depth > 0 ? pdTRUE : pdFALSE;
}

BaseType_t xPortInIsrContext(void)
{
	return s31_rtos_in_isr();
}

BaseType_t s31_rtos_can_yield(void)
{
	struct s31_tcb *t = s31_rtos_current();

	return !s31_rtos_in_isr() && (!t || t->critical_depth == 0);
}

void vPortYieldFromISR(void)
{
	/* Linux threaded IRQs wake the task through the bridge. */
}

TickType_t xTaskGetTickCount(void)
{
#ifdef S31_LINUX_SMODE
	return s31_linux_tick_count();
#else
	return s31_tick;
#endif
}

TickType_t s31_rtos_get_tick(void)
{
	return xTaskGetTickCount();
}

TickType_t xTaskGetTickCountFromISR(void)
{
	return xTaskGetTickCount();
}

/*
 * The RTOS tick and esp_timer epoch now come from Linux jiffies through the
 * bridge, so the TIMG1 hard IRQ no longer owns the time base.  This hook is
 * kept as a no-op for the measured ABI.
 */
void s31_rtos_hard_tick(void)
{
#ifndef S31_LINUX_SMODE
	s31_tick++;
#endif
}

void s31_rtos_tick(void)
{
#ifdef S31_LINUX_SMODE
	 extern void s31_linux_timers_tick(void);

	/* Callback execution is serialized under the blob gate in the radio
	 * worker.  Time is read from the Linux bridge in s31_linux_timer.c. */
	s31_linux_timers_tick();
#else
	s31_tick++;
#endif
}

struct s31_tcb *s31_rtos_current(void)
{
	struct s31_tcb *t = s31_linux_current_cookie();

	return t ? t : &s31_foreign_tcb;
}

UBaseType_t xTaskGetSchedulerState(void)
{
	return s31_scheduler_started ? taskSCHEDULER_RUNNING
				      : taskSCHEDULER_NOT_STARTED;
}

/* --- Linux-backed tasks --- */
static void s31_rtos_task_entry(void *arg)
{
	struct s31_tcb *t = arg;
	int i;

	if (++s31_task_return_count <= 16)
		s31_linux_printf("[S31] compat task enter %s t=%p entry=%p\n",
			       t->name, t, t->entry);
	t->entry(t->arg);
	if (s31_task_return_count <= 16)
		s31_linux_printf("[S31] compat task returned %s t=%p\n", t->name, t);
	if (!t->tls_cleaned) {
		t->tls_cleaned = 1;
		for (i = 0; i < 4; i++)
			if (t->tls_dtor[i])
				t->tls_dtor[i](i, t->tls[i]);
	}
	/* The Linux trampoline still owns this TCB until the payload has returned
	 * to s31_linux_task_main().  In particular, the self-delete path escapes
	 * through that trampoline and must not leave it dereferencing freed SRAM. */
}

BaseType_t xTaskCreatePinnedToCore(void (*task_func)(void *), const char *name,
				   uint32_t stack_depth, void *param,
				   UBaseType_t prio, void *task_handle,
				   BaseType_t core_id)
{
	struct s31_tcb *t;
	void *linux_task;
	uint32_t stack_size;

	(void)core_id; /* the S31 radio executes on the single Linux radio hart */
	if (!task_func || !stack_depth)
		return pdFAIL;
	t = s31_rtos_malloc(sizeof(*t));
	if (!t)
		return pdFAIL;
	memset(t, 0, sizeof(*t));
	if (name)
		strncpy(t->name, name, S31_TASK_NAME_LEN - 1);
	else
		strncpy(t->name, "s31-task", S31_TASK_NAME_LEN - 1);
	t->entry = task_func;
	t->arg = param;
	t->priority = prio;
	t->notify_context = s31_linux_sync_create();
	t->suspend_context = s31_linux_sync_create();
	if (!t->notify_context || !t->suspend_context) {
		if (t->notify_context)
			s31_linux_sync_destroy(t->notify_context);
		if (t->suspend_context)
			s31_linux_sync_destroy(t->suspend_context);
		s31_rtos_free(t);
		return pdFAIL;
	}
	/* ESP-IDF, unlike upstream FreeRTOS, specifies this in bytes.  The
	 * execution stack must live in internal SRAM (see s31_rtos_stack_malloc). */
	stack_size = (stack_depth + 15U) & ~15U;
	/* A compatibility task calls Linux wait/schedule code on this same HP-SRAM
	 * stack.  Previously s31_rtos_stack_malloc() silently allocated 8 KiB but
	 * task->stack_size retained (for example) Wi-Fi's 3584-byte request, so the
	 * trampoline placed SP in the middle of the allocation and exposed only
	 * 3584 bytes to downward-growing kernel frames. */
	if (stack_size < 8192)
		stack_size = 8192;
	t->stack_base = s31_rtos_stack_malloc(stack_size);
	if (!t->stack_base) {
		s31_linux_sync_destroy(t->notify_context);
		s31_linux_sync_destroy(t->suspend_context);
		s31_rtos_free(t);
		return pdFAIL;
	}
	t->stack_size = stack_size;
	linux_task = s31_linux_task_create(s31_rtos_task_entry, t->name,
					   stack_size, t->stack_base, t, prio, t);
	if (!linux_task) {
		s31_rtos_free(t->stack_base);
		s31_linux_sync_destroy(t->notify_context);
		s31_linux_sync_destroy(t->suspend_context);
		s31_rtos_free(t);
		return pdFAIL;
	}
	t->linux_task = linux_task;
	if (task_handle)
		*(void **)task_handle = t;
	s31_scheduler_started = 1;
	return pdPASS;
}

void *xTaskGetCurrentTaskHandle(void)
{
	return s31_rtos_current();
}

void s31_rtos_task_release(void *cookie)
{
	struct s31_tcb *t = cookie;

	if (!t)
		return;
	if (t->notify_context)
		s31_linux_sync_destroy(t->notify_context);
	if (t->suspend_context)
		s31_linux_sync_destroy(t->suspend_context);
	s31_rtos_free(t);
}

void vTaskDelay(TickType_t ticks)
{
	if (s31_rtos_in_isr())
		return;
	if (!ticks) {
		if (++s31_task_yield_count <= 32)
			s31_linux_printf("[S31] vTaskDelay(0) yield #%u task=%s prio=%u\n",
				       s31_task_yield_count,
				       s31_rtos_current() ? s31_rtos_current()->name : "none",
				       s31_rtos_current() ? s31_rtos_current()->priority : 0);
		s31_linux_task_yield();
	} else {
		if (++s31_task_delay_count <= 64)
			s31_linux_printf("[S31] vTaskDelay #%u ticks=%u task=%s prio=%u\n",
			       s31_task_delay_count, ticks,
			       s31_rtos_current() ? s31_rtos_current()->name : "none",
			       s31_rtos_current() ? s31_rtos_current()->priority : 0);
		s31_linux_task_delay(ticks);
	}
}

enum {
	S31_NOTIFY_NOT_WAITING = 0,
	S31_NOTIFY_WAITING = 1,
	S31_NOTIFY_RECEIVED = 2,
};

static TickType_t s31_notify_wait_remaining(TickType_t timeout,
					     TickType_t start)
{
	TickType_t elapsed;

	if (timeout == portMAX_DELAY)
		return portMAX_DELAY;
	elapsed = s31_rtos_get_tick() - start;
	return elapsed >= timeout ? 0 : timeout - elapsed;
}

uint32_t ulTaskGenericNotifyTake(UBaseType_t index, BaseType_t clear,
				 TickType_t timeout)
{
	struct s31_tcb *t = s31_rtos_current();
	TickType_t start = s31_rtos_get_tick();
	uint32_t seq, value;
	int32_t waited;

	if (!t || index != 0)
		return 0;
	for (;;) {
		seq = s31_linux_sync_sequence(t->notify_context);
		s31_linux_sync_lock(t->notify_context);
		value = t->notify_value;
		if (value) {
			t->notify_value = clear ? 0 : value - 1;
			t->notify_state = S31_NOTIFY_NOT_WAITING;
			s31_linux_sync_unlock(t->notify_context);
			return value;
		}
		t->notify_state = S31_NOTIFY_WAITING;
		s31_linux_sync_unlock(t->notify_context);
		if (!timeout)
			waited = 0;
		else {
			TickType_t remaining =
				s31_notify_wait_remaining(timeout, start);

			waited = remaining ?
				s31_linux_sync_wait(t->notify_context, seq,
						     remaining,
						     S31_BLOB_RELEASE_NOTIFY_TAKE) : 0;
		}
		if (waited <= 0) {
			s31_linux_sync_lock(t->notify_context);
			t->notify_state = S31_NOTIFY_NOT_WAITING;
			s31_linux_sync_unlock(t->notify_context);
			return 0;
		}
	}
}

BaseType_t xTaskGenericNotifyWait(UBaseType_t index, uint32_t clear_entry,
				   uint32_t clear_exit, uint32_t *out,
				   TickType_t timeout)
{
	struct s31_tcb *t = s31_rtos_current();
	TickType_t start = s31_rtos_get_tick();
	uint32_t seq, value;
	int32_t waited;

	if (!t || index != 0)
		return pdFAIL;
	for (;;) {
		seq = s31_linux_sync_sequence(t->notify_context);
		s31_linux_sync_lock(t->notify_context);
		if (t->notify_state != S31_NOTIFY_RECEIVED)
			t->notify_value &= ~clear_entry;
		if (t->notify_state == S31_NOTIFY_RECEIVED) {
			value = t->notify_value;
			if (out)
				*out = value;
			t->notify_value &= ~clear_exit;
			t->notify_state = S31_NOTIFY_NOT_WAITING;
			s31_linux_sync_unlock(t->notify_context);
			return pdPASS;
		}
		t->notify_state = S31_NOTIFY_WAITING;
		s31_linux_sync_unlock(t->notify_context);
		if (!timeout)
			waited = 0;
		else {
			TickType_t remaining =
				s31_notify_wait_remaining(timeout, start);

			waited = remaining ?
				s31_linux_sync_wait(t->notify_context, seq,
						     remaining,
						     S31_BLOB_RELEASE_NOTIFY_WAIT) : 0;
		}
		if (waited <= 0) {
			s31_linux_sync_lock(t->notify_context);
			if (out)
				*out = t->notify_value;
			t->notify_state = S31_NOTIFY_NOT_WAITING;
			s31_linux_sync_unlock(t->notify_context);
			return pdFAIL;
		}
	}
}

BaseType_t xTaskGenericNotify(void *task, UBaseType_t index, uint32_t value,
			       int action, uint32_t *previous)
{
	struct s31_tcb *t = task;
	BaseType_t rc = pdPASS;

	if (!t || index != 0)
		return pdFAIL;
	s31_linux_sync_lock(t->notify_context);
	if (previous)
		*previous = t->notify_value;
	switch (action) {
	case 0: break;
	case 1: t->notify_value |= value; break;
	case 2: t->notify_value++; break;
	case 3: t->notify_value = value; break;
	case 4:
		if (t->notify_state == S31_NOTIFY_RECEIVED)
			rc = pdFAIL;
		else
			t->notify_value = value;
		break;
	default: rc = pdFAIL; break;
	}
	if (rc == pdPASS)
		t->notify_state = S31_NOTIFY_RECEIVED;
	s31_linux_sync_unlock(t->notify_context);
	if (rc == pdPASS)
		s31_linux_sync_wake(t->notify_context);
	return rc;
}

void vTaskGenericNotifyGiveFromISR(void *task, UBaseType_t index,
				    BaseType_t *higher_woken)
{
	(void)xTaskGenericNotify(task, index, 0, 2, NULL);
	if (higher_woken)
		*higher_woken = pdTRUE;
}

void vTaskSuspendAll(void) { }
BaseType_t xTaskResumeAll(void) { return pdFALSE; }

void vTaskSuspend(void *task)
{
	struct s31_tcb *t = task ? task : s31_rtos_current();
	uint32_t seq;

	if (!t || t != s31_rtos_current())
		return;
	seq = s31_linux_sync_sequence(t->suspend_context);
	(void)s31_linux_sync_wait(t->suspend_context, seq, portMAX_DELAY,
				  S31_BLOB_RELEASE_TASK_SUSPEND);
}

void vTaskPrioritySet(void *task, UBaseType_t priority)
{
	struct s31_tcb *t = task ? task : s31_rtos_current();
	if (t) {
		t->priority = priority;
		s31_linux_task_set_priority(t->linux_task, priority);
		if (++s31_task_priority_set_count <= 32)
			s31_linux_printf("[S31] vTaskPrioritySet #%u task=%s prio=%u\n",
				       s31_task_priority_set_count, t->name, priority);
	}
}

BaseType_t xTaskGetCoreID(void *task) { (void)task; return 0; }
void *xTaskGetCurrentTaskHandleForCore(BaseType_t core)
{
	return core == 0 ? s31_rtos_current() : NULL;
}

void vTaskDelete(void *task)
{
	struct s31_tcb *t = task ? task : s31_rtos_current();
	int i;

	if (!t)
		return;
	if (++s31_task_delete_count <= 32)
		s31_linux_printf("[S31] vTaskDelete #%u task=%p current=%p name=%s linux=%p\n",
			       s31_task_delete_count, t, s31_rtos_current(), t->name,
			       t->linux_task);
	if (!t->tls_cleaned) {
		t->tls_cleaned = 1;
		for (i = 0; i < 4; i++)
			if (t->tls_dtor[i])
				t->tls_dtor[i](i, t->tls[i]);
	}
	if (t == s31_rtos_current()) {
		s31_linux_task_exit_current();
		return;
	}
	s31_linux_task_stop(t->linux_task);
}

void *pvTaskGetThreadLocalStoragePointer(void *task, int index)
{
	struct s31_tcb *t = task ? task : s31_rtos_current();

	if (!t || index < 0 || index >= 4)
		return NULL;
	return t->tls[index];
}

void vTaskSetThreadLocalStoragePointerAndDelCallback(void *task, int index,
						     void *value,
						     void (*dtor)(int, void *))
{
	struct s31_tcb *t = task ? task : s31_rtos_current();

	if (!t || index < 0 || index >= 4)
		return;
	t->tls[index] = value;
	t->tls_dtor[index] = dtor;
}

void s31_rtos_init(void)
{
	memset(&s31_foreign_tcb, 0, sizeof(s31_foreign_tcb));
	strncpy(s31_foreign_tcb.name, "radio-worker",
		S31_TASK_NAME_LEN - 1);
	s31_foreign_tcb.priority = 24;
	s31_tick = 0;
	s31_scheduler_started = 0;
	s31_rtos_isr_depth = 0;
	s31_orphan_critical_depth = 0;
}
