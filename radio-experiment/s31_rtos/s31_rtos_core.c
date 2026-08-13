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

extern int esp_rom_printf(const char *fmt, ...);

uint32_t s31_rtos_isr_depth;

static volatile TickType_t s31_tick;
static volatile uint32_t s31_scheduler_started;
static volatile uint32_t s31_orphan_critical_depth;
static volatile uint32_t s31_orphan_critical_flags;
static uint32_t s31_task_return_count;
static uint32_t s31_task_delete_count;

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

	/* Linux sleeps on this stack too (bridge waits), so keep headroom. */
	if (size < 16384)
		size = 16384;
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

BaseType_t s31_rtos_in_isr(void)
{
	return s31_rtos_isr_depth > 0 ? pdTRUE : pdFALSE;
}

BaseType_t xPortInIsrContext(void)
{
	return s31_rtos_in_isr();
}

void vPortYieldFromISR(void)
{
	/* Linux threaded IRQs wake the task through the bridge. */
}

TickType_t xTaskGetTickCount(void)
{
	return s31_tick;
}

TickType_t s31_rtos_get_tick(void)
{
	return s31_tick;
}

TickType_t xTaskGetTickCountFromISR(void)
{
	return s31_tick;
}

/*
 * The S-mode radio worker may be starved for long stretches while a
 * compatibility task owns the blob gate and busy-waits inside closed code.
 * The TIMG1 hard IRQ therefore advances the RTOS tick (and the esp_timer
 * epoch) directly; the worker only runs the esp_timer callbacks afterwards.
 * Without this, any payload busy-wait on xTaskGetTickCount() or
 * esp_timer_get_time() while the gate is held would spin forever.
 */
void s31_rtos_hard_tick(void)
{
	s31_tick++;
#ifdef S31_LINUX_SMODE
	 extern void s31_linux_timer_advance(void);

	s31_linux_timer_advance();
#endif
}

void s31_rtos_tick(void)
{
#ifdef S31_LINUX_SMODE
	 extern void s31_linux_timers_tick(void);

	/* s31_rtos_hard_tick() already advanced the tick from the hard IRQ. */
	s31_linux_timers_tick();
#else
	s31_tick++;
#endif
}

struct s31_tcb *s31_rtos_current(void)
{
	return s31_linux_current_cookie();
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
		esp_rom_printf("[S31] compat task enter %s t=%p entry=%p\n",
			       t->name, t, t->entry);
	t->entry(t->arg);
	if (s31_task_return_count <= 16)
		esp_rom_printf("[S31] compat task returned %s t=%p\n", t->name, t);
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
	/* ESP-IDF, unlike upstream FreeRTOS, specifies this in bytes.  The
	 * execution stack must live in internal SRAM (see s31_rtos_stack_malloc). */
	stack_size = (stack_depth + 15U) & ~15U;
	t->stack_base = s31_rtos_stack_malloc(stack_size);
	if (!t->stack_base) {
		s31_rtos_free(t);
		return pdFAIL;
	}
	t->stack_size = stack_size;
	linux_task = s31_linux_task_create(s31_rtos_task_entry, t->name,
					   stack_size, t->stack_base, t, prio, t);
	if (!linux_task) {
		s31_rtos_free(t->stack_base);
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

void vTaskDelay(TickType_t ticks)
{
	if (s31_rtos_in_isr())
		return;
	s31_linux_task_delay(ticks);
}

void vTaskDelete(void *task)
{
	struct s31_tcb *t = task ? task : s31_rtos_current();
	int i;

	if (!t)
		return;
	if (++s31_task_delete_count <= 32)
		esp_rom_printf("[S31] vTaskDelete #%u task=%p current=%p name=%s linux=%p\n",
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
	s31_tick = 0;
	s31_scheduler_started = 0;
	s31_rtos_isr_depth = 0;
	s31_orphan_critical_depth = 0;
}
