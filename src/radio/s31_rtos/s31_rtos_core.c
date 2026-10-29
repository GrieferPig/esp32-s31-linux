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
#include <stdarg.h>
#include <errno.h>
#include <string.h>
#include "s31_rtos.h"

/* Fixed-argument Linux logging import keeps arbitrary printf stack tails
 * behind a stable va_list pointer while TP normalization uses a call frame. */
extern void s31_linux_vprintf(const char *fmt, void *arguments);
_Static_assert(sizeof(va_list) == sizeof(void *), "RV32 va_list import ABI");
void s31_linux_printf(const char *fmt, ...)
{
    va_list arguments;
    va_start(arguments, fmt);
    s31_linux_vprintf(fmt, arguments);
    va_end(arguments);
}

uint32_t s31_rtos_isr_depth;

static volatile uint32_t s31_scheduler_started;
static uint32_t s31_stack_minimum = 8192;
/* Direct/deferred radio IRQs and the exclusive pre-task bootstrap need an
 * auxiliary ABI identity, never a schedulable or general foreign-thread TCB.
 * The kernel proves this role before allowing a missing native cookie. */
static struct s31_tcb s31_aux_tcb;
static void s31_rtos_task_finalize(void *cookie);

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
	/* Linux owns exact aligned stack extents and joins their last reference.
	 * No ordinary heap header occupies the previous stack's tail. */
	return (void *)s31_linux_sync_owner((void *)(uintptr_t)size,
					 S31_SYNC_STACK_ALLOC);
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
/* Linux owns the physical invocation, IRQ flags and object recursion. The
 * compact managed TCB retains only the logical ABI hint; foreign callers must
 * never use the shared compatibility TCB as their physical lock identity. */
void vPortEnterCritical(void)
{
    uint32_t flags = s31_linux_critical_enter();
    struct s31_tcb *t = s31_rtos_current();
    if (t && t->linux_task) { t->critical_depth++; t->critical_flags = flags; }
}
void vPortExitCritical(void)
{
    struct s31_tcb *t = s31_rtos_current();
    if (t && t->linux_task) {
        if (!t->critical_depth) s31_linux_sync_owner(NULL, S31_SYNC_OWNER_FATAL);
        t->critical_depth--;
    }
    s31_linux_critical_exit(0);
}
struct s31_mux_enter_args { void *mux; int32_t timeout_cycles; };
BaseType_t xPortEnterCriticalTimeout(void *mux, BaseType_t timeout)
{
    struct s31_mux_enter_args args = { mux, timeout };
    BaseType_t result = s31_linux_sync_owner(&args, S31_SYNC_MUX_ENTER) ? pdTRUE : pdFALSE;
    struct s31_tcb *t = s31_rtos_current();
    if (result && t && t->linux_task) t->critical_depth++;
    return result;
}
void vPortExitCriticalMultiCore(void *mux)
{
    struct s31_tcb *t = s31_rtos_current();
    if (t && t->linux_task) {
        if (!t->critical_depth) s31_linux_sync_owner(NULL, S31_SYNC_OWNER_FATAL);
        t->critical_depth--;
    }
    (void)s31_linux_sync_owner(mux, S31_SYNC_MUX_EXIT);
}

BaseType_t s31_rtos_in_isr(void)
{
	return s31_rtos_isr_depth > 0 || s31_linux_in_interrupt() ? pdTRUE : pdFALSE;
}

BaseType_t xPortInIsrContext(void)
{
	return s31_rtos_in_isr();
}

BaseType_t s31_rtos_can_yield(void)
{
	struct s31_tcb *t = s31_rtos_current();

	return !s31_rtos_in_isr() && (!t || !t->linux_task || !t->critical_depth) &&
        !s31_linux_sync_owner(NULL, S31_SYNC_CRITICAL_DEPTH);
}

void vPortYieldFromISR(void)
{
	/* Linux threaded IRQs wake the task through the bridge. */
}

TickType_t xTaskGetTickCount(void)
{
	return s31_linux_tick_count();
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
}

void s31_rtos_tick(void)
{
	extern void s31_linux_timers_tick(void);

	/* The native dispatcher owns task callbacks; its timer lease serializes
	 * callbacks independently of other radio execution. */
	s31_linux_timers_tick();
}

struct s31_tcb *s31_rtos_current(void)
{
	struct s31_tcb *t = s31_linux_current_cookie();

	if (t)
		return t;
	if (!s31_linux_sync_owner(NULL, S31_SYNC_AUX_CONTEXT)) {
        s31_linux_sync_owner(NULL, S31_SYNC_OWNER_FATAL);
        return NULL;
    }
    return &s31_aux_tcb;
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

    t->entry(t->arg);
    /* Normal/self/forced exit all converge on Linux's target finalizer. */
}

BaseType_t xTaskCreatePinnedToCore(void (*task_func)(void *), const char *name,
				   uint32_t stack_depth, void *param,
				   UBaseType_t prio, void *task_handle,
				   BaseType_t core_id)
{
	struct s31_tcb *t;
	void *linux_task;
	uint32_t stack_size;

	/* FreeRTOS task creation has no FromISR variant.  Reject before
	 * touching the TCB, native waiters, payload stack or output handle.
	 * The payload depth alone cannot identify a depth-zero Linux IRQ. */
	if (s31_rtos_in_isr() || s31_linux_in_interrupt())
		return pdFAIL;
	if (!task_func || !stack_depth)
		return pdFAIL;
	/* Retired kernel stack references must join before allocating another
	 * aligned extent. Reclaiming only inside the factory is too late when
	 * this allocation itself exhausts or fragments the SRAM pool. */
	if (!s31_linux_sync_owner(NULL, S31_SYNC_STACK_RECLAIM))
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
	/* Physical and public task affinity agree with the core0 runtime. */
	t->core_id = 0;
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
	/* This is the actual Linux task stack. Linux negotiates its size and
	 * alignment before any task creation; there is no second wait stack. */
	if (stack_size < s31_stack_minimum)
		stack_size = s31_stack_minimum;
	t->stack_base = s31_rtos_stack_malloc(stack_size);
	if (!t->stack_base) {
		s31_linux_sync_destroy(t->notify_context);
		s31_linux_sync_destroy(t->suspend_context);
		s31_rtos_free(t);
		return pdFAIL;
	}
	t->stack_size = stack_size;
	linux_task = s31_linux_task_create_finalized(s31_rtos_task_entry, t->name,
					   stack_size, t->stack_base, t, prio, t,
					   core_id, s31_rtos_task_finalize, &t->linux_task);
	if (!linux_task) {
		s31_linux_sync_owner(t->stack_base, S31_SYNC_STACK_FREE);
		s31_linux_sync_destroy(t->notify_context);
		s31_linux_sync_destroy(t->suspend_context);
		s31_rtos_free(t);
		return pdFAIL;
	}
	if (task_handle)
		*(void **)task_handle = t;
	s31_scheduler_started = 1;
	return pdPASS;
}

void *xTaskGetCurrentTaskHandle(void)
{
	return s31_rtos_current();
}

/* Task-owned resource cleanup, executed by the task's final continuation.
 * Push/pop are owner-only; other tasks request stop but never run this chain.
 * Cleanup callbacks must not block or invoke a nonlocal task exit.
 */
BaseType_t s31_rtos_cleanup_push(struct s31_task_cleanup *cleanup)
{
    struct s31_tcb *t = s31_rtos_current();

    if (!cleanup || !cleanup->release || !t ||
        (!t->linux_task && !s31_linux_sync_owner(NULL, S31_SYNC_OWNER_FOREIGN_TOKEN)))
        return pdFALSE;
    cleanup->owner = t;
    cleanup->next = t->cleanups;
    t->cleanups = cleanup;
    return pdTRUE;
}

void s31_rtos_cleanup_pop(struct s31_task_cleanup *cleanup)
{
    struct s31_tcb *t = cleanup->owner;
    struct s31_task_cleanup **link;

    if (!t)
        return;
    for (link = &t->cleanups; *link; link = &(*link)->next) {
        if (*link == cleanup) {
            *link = cleanup->next;
            break;
        }
    }
    cleanup->owner = NULL;
    cleanup->next = NULL;
}

void s31_rtos_cleanup_drain(struct s31_tcb *t)
{
    while (t->cleanups) {
        struct s31_task_cleanup *cleanup = t->cleanups;

        t->cleanups = cleanup->next;
        cleanup->owner = NULL;
        cleanup->next = NULL;
        cleanup->release(cleanup);
    }
    if ((t->native_call.state))
        s31_linux_sync_owner(NULL, S31_SYNC_OWNER_FATAL);
}

/* Invoked by this task's final Linux continuation on a fresh SRAM stack,
 * while current-cookie, init_mm, blob gate and FP identity still belong to it.
 * Generic TLSP callbacks run here; a pthread callback retains its outer TLS
 * list while calling/repopulating POSIX destructors, so clear pointers later. */
static void s31_rtos_task_finalize(void *cookie)
{
    struct s31_tcb *t = cookie;
    int i, pending;

    if (!t || t != s31_rtos_current() || !t->linux_task || t->tls_finalizing) {
        s31_linux_task_finalize_report(-1);
        return;
    }
    if (t->tls_cleaned) {
        s31_linux_task_finalize_report(0);
        return;
    }
    t->tls_finalizing = 1;
    /* Linux retired this target's physical body critical ownership before
     * reentering the fresh callback span. Its abandoned logical nesting
     * must not suppress a destructor's new short critical/IRQ fence. */
    t->critical_depth = 0;
    t->critical_flags = 0;
    s31_rtos_cleanup_drain(t);
    t->tls_cleaned = 1;
    do {
        for (i = 0; i < 4; ++i) {
            while (t->tls_dtor[i]) {
                void (*dtor)(int, void *) = t->tls_dtor[i];
                void *value = t->tls[i];

                /* Claim this registration exactly once before callback reentry.
                 * Owner-installed replacement registrations remain observable. */
                t->tls_dtor[i] = NULL;
                dtor(i, value);
                if (!t->tls_dtor[i] && t->tls[i] == value)
                    t->tls[i] = NULL;
            }
        }
        pending = 0;
        for (i = 0; i < 4; ++i)
            pending |= t->tls_dtor[i] != NULL;
    } while (pending);
    s31_rtos_cleanup_drain(t); /* retire destructor-created ownership too */
    /* Body-owned nesting was retired before this phase. A callback must
     * balance its own newly acquired logical span, including cancellation
     * returns. Never disguise a returned live/suspended lock by zeroing it. */
    if (t->critical_depth) {
        s31_linux_task_finalize_report(-1);
        return;
    }
    memset(t->tls, 0, sizeof(t->tls));
    t->tls_finalizing = 0;
    s31_linux_task_finalize_report(0);
}

void s31_rtos_task_release(void *cookie)
{
	struct s31_tcb *t = cookie;

	if (!t)
		return;
	/* Wait helpers finish/cancel their Linux waiter before a forced exit.
     * Drain payload resources before TCB contexts or memory disappear. */
    s31_rtos_cleanup_drain(t);
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
		s31_linux_task_yield();
	} else {
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
    if (!t || priority > 31) return;
    if (t->linux_task)
        s31_linux_task_set_priority(t->linux_task, priority, 0);
    /* IRQ/bootstrap identity has no scheduler role to mutate. */
}

UBaseType_t uxTaskPriorityGet(void *task)
{
    struct s31_tcb *t = task ? task : s31_rtos_current();
    if (!t) return 0;
    if (t->linux_task)
        return s31_linux_sync_owner(t->linux_task, S31_SYNC_NATIVE_EFFECTIVE);
    return t->priority; /* Fixed IRQ/bootstrap identity level. */
}

BaseType_t xTaskGetCoreID(void *task)
{
	struct s31_tcb *t = task ? task : s31_rtos_current();

	return t ? t->core_id : 0;
}

BaseType_t xPortGetCoreID(void)
{
	return s31_linux_current_cpu();
}

void *xTaskGetCurrentTaskHandleForCore(BaseType_t core)
{
	struct s31_tcb *t = s31_rtos_current();

	if (!t || core != xPortGetCoreID())
		return NULL;
	return (t->core_id == 0x7fffffff || t->core_id == core) ? t : NULL;
}

void vTaskDelete(void *task)
{
	struct s31_tcb *t = task ? task : s31_rtos_current();

	if (!t)
		return;

    /* Recursive self-delete from a TLSP callback must not abandon the
     * remaining final continuation. Ordinary delete only requests stop. */
    if (t == s31_rtos_current() && t->tls_finalizing) {
        return;
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
    if (dtor && !t->linux_task) {
        s31_linux_sync_owner(NULL, S31_SYNC_OWNER_FATAL);
        return;
    }
	t->tls[index] = value;
	t->tls_dtor[index] = dtor;
}

void s31_rtos_init(void)
{
    /* Module data is not copied/zeroed by runtime_init.  The old registry
     * and its lock belonged to the prior, now-reclaimed SRAM pool. */
    /* This init runs in the exclusive non-sleeping bootstrap. Refuse an
     * adapter without the new explicit auxiliary-role contract. */
    if (!s31_linux_sync_owner(NULL, S31_SYNC_AUX_CONTEXT))
        s31_linux_sync_owner(NULL, S31_SYNC_OWNER_FATAL);
    s31_rtos_event_runtime_reset();
    s31_rtos_queue_runtime_reset();
    s31_rtos_timer_runtime_reset();
    if (!s31_linux_sync_owner(NULL, S31_SYNC_NATIVE_MODE))
        s31_linux_sync_owner(NULL, S31_SYNC_OWNER_FATAL);
    uintptr_t minimum = s31_linux_sync_owner(NULL, S31_SYNC_STACK_RAW_MINIMUM);
    if (minimum < 4096 || minimum > UINT32_MAX || (minimum & (minimum - 1)))
        s31_linux_sync_owner(NULL, S31_SYNC_OWNER_FATAL);
    s31_stack_minimum = (uint32_t)minimum;
    memset(&s31_aux_tcb, 0, sizeof(s31_aux_tcb));
    strncpy(s31_aux_tcb.name, "radio-irq-boot", S31_TASK_NAME_LEN - 1);
    s31_aux_tcb.priority = 24;
    /* Native scheduling and independent IRQ entry bank the relocated cell. */
	s31_linux_sync_owner(&errno, S31_SYNC_ERRNO_REGISTER);
	s31_scheduler_started = 0;
	s31_rtos_isr_depth = 0;
}
