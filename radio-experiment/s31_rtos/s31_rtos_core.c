/*
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * S31 radio world stub layer: core (critical sections, tick, cooperative
 * scheduler, tasks, TLS).  See docs/S31_radio_rtos_stub.md.
 */
#include <stdint.h>
#include <string.h>
#include "s31_rtos.h"

uint32_t s31_rtos_isr_depth;          /* maintained by the world glue */

static volatile TickType_t s31_tick;
static volatile uint32_t s31_crit_depth;
static volatile uint32_t s31_crit_status;

static struct s31_tcb *s31_ready_head;
static struct s31_tcb *s31_ready_tail;
static struct s31_tcb *s31_all_tasks; /* registry for timeout scans */
static struct s31_tcb *s31_current_tcb;
static uint32_t s31_scheduler_active;
static uint32_t s31_scheduler_started;

static uint32_t s31_sched_stack_fallback[512];
static uint32_t *s31_sched_stack;
static uint32_t *s31_sched_ctx;
static uint32_t *s31_entry_ctx;
static int s31_force_internal_stacks;

/* --- radio heap (heap_caps in the same world) --- */
void *s31_rtos_malloc(uint32_t size)
{
	extern void *heap_caps_malloc(uint32_t size, uint32_t caps);

	return heap_caps_malloc(size, 0x804 /* INTERNAL | 8BIT */);
}

/*
 * A task stack must remain accessible while the PHY/controller changes the
 * external-memory/cache state.  Keeping these stacks in SPIRAM works during
 * init, but esp_bt_controller_enable() can then take a synchronous trap while
 * PSRAM is temporarily unavailable; Linux cannot even save the trap frame on
 * that stack and recursively faults in handle_exception.  ESP-IDF likewise
 * keeps controller task stacks in internal byte-addressable RAM.
 */
static void *s31_rtos_stack_malloc(uint32_t size, int internal)
{
	extern void *heap_caps_malloc(uint32_t size, uint32_t caps);

	(void)internal;
	return heap_caps_malloc(size, 0x804 /* INTERNAL | 8BIT */);
}

void s31_rtos_use_internal_stacks(void)
{
	s31_force_internal_stacks = 1;
}

void s31_rtos_free(void *ptr)
{
	extern void heap_caps_free(void *ptr);

	heap_caps_free(ptr);
}

/* --- critical sections --- */
void vPortEnterCritical(void)
{
	uint32_t status;

	/* Read and clear MIE in one instruction. Incrementing the nesting count
	 * first leaves an interrupt window in which an ISR observes a false
	 * nested critical section. */
	#ifdef S31_LINUX_SMODE
	__asm__ volatile("csrrci %0, sstatus, 2"
			 : "=r"(status) : : "memory");
	#else
	__asm__ volatile("csrrci %0, mstatus, 8"
			 : "=r"(status) : : "memory");
	#endif
	if (s31_crit_depth++ == 0)
		s31_crit_status = status;
}

void vPortExitCritical(void)
{
	if (!s31_crit_depth)
		return;
	#ifdef S31_LINUX_SMODE
	if (--s31_crit_depth == 0 && (s31_crit_status & 0x2))
		__asm__ volatile("csrsi sstatus, 2" ::: "memory");
	#else
	if (--s31_crit_depth == 0 && (s31_crit_status & 0x8))
		__asm__ volatile("csrsi mstatus, 8" ::: "memory");
	#endif
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
	/* The world glue always schedules after an ISR; nothing to do. */
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

void s31_rtos_tick(void)
{
	#ifdef S31_LINUX_SMODE
	extern void s31_linux_timers_tick(void);
	#endif

	s31_tick++;
	#ifdef S31_LINUX_SMODE
	s31_linux_timers_tick();
	#endif
}

struct s31_tcb *s31_rtos_current(void)
{
	return s31_current_tcb;
}

UBaseType_t xTaskGetSchedulerState(void)
{
	return s31_scheduler_started ? taskSCHEDULER_RUNNING
				      : taskSCHEDULER_NOT_STARTED;
}

/* --- context switch (naked) --- */
/* Frame layout (112 bytes, 16-byte aligned):
 *   0: ra, 4: a0, 8..52: s0..s11,
 *   56..100: fs0..fs11, 104: fcsr, 108: padding. */
__attribute__((naked)) void s31_rtos_switch(uint32_t **prev_ctx,
						      uint32_t *next_ctx,
						      uint32_t retval)
{
	__asm__ volatile(
		"addi sp, sp, -112\n\t"
		"sw ra, 0(sp)\n\t"
		"sw a2, 4(sp)\n\t"      /* retval becomes resume a0 */
		"sw s0, 8(sp)\n\t"
		"sw s1, 12(sp)\n\t"
		"sw s2, 16(sp)\n\t"
		"sw s3, 20(sp)\n\t"
		"sw s4, 24(sp)\n\t"
		"sw s5, 28(sp)\n\t"
		"sw s6, 32(sp)\n\t"
		"sw s7, 36(sp)\n\t"
		"sw s8, 40(sp)\n\t"
		"sw s9, 44(sp)\n\t"
		"sw s10, 48(sp)\n\t"
		"sw s11, 52(sp)\n\t"
		"fsw fs0, 56(sp)\n\t"
		"fsw fs1, 60(sp)\n\t"
		"fsw fs2, 64(sp)\n\t"
		"fsw fs3, 68(sp)\n\t"
		"fsw fs4, 72(sp)\n\t"
		"fsw fs5, 76(sp)\n\t"
		"fsw fs6, 80(sp)\n\t"
		"fsw fs7, 84(sp)\n\t"
		"fsw fs8, 88(sp)\n\t"
		"fsw fs9, 92(sp)\n\t"
		"fsw fs10, 96(sp)\n\t"
		"fsw fs11, 100(sp)\n\t"
		"frcsr t0\n\t"
		"sw t0, 104(sp)\n\t"
		"sw sp, 0(a0)\n\t"      /* *prev_ctx = sp */
		"mv sp, a1\n\t"         /* sp = next_ctx */
		"lw ra, 0(sp)\n\t"
		"lw a0, 4(sp)\n\t"
		"lw s0, 8(sp)\n\t"
		"lw s1, 12(sp)\n\t"
		"lw s2, 16(sp)\n\t"
		"lw s3, 20(sp)\n\t"
		"lw s4, 24(sp)\n\t"
		"lw s5, 28(sp)\n\t"
		"lw s6, 32(sp)\n\t"
		"lw s7, 36(sp)\n\t"
		"lw s8, 40(sp)\n\t"
		"lw s9, 44(sp)\n\t"
		"lw s10, 48(sp)\n\t"
		"lw s11, 52(sp)\n\t"
		"flw fs0, 56(sp)\n\t"
		"flw fs1, 60(sp)\n\t"
		"flw fs2, 64(sp)\n\t"
		"flw fs3, 68(sp)\n\t"
		"flw fs4, 72(sp)\n\t"
		"flw fs5, 76(sp)\n\t"
		"flw fs6, 80(sp)\n\t"
		"flw fs7, 84(sp)\n\t"
		"flw fs8, 88(sp)\n\t"
		"flw fs9, 92(sp)\n\t"
		"flw fs10, 96(sp)\n\t"
		"flw fs11, 100(sp)\n\t"
		"lw t0, 104(sp)\n\t"
		"fscsr t0\n\t"
		"addi sp, sp, 112\n\t"
		"ret\n\t");
}

/* task entry trampoline: run the task function, then self-delete. */
void s31_rtos_task_entry(void)
{
	struct s31_tcb *t = s31_current_tcb;
	t->entry(t->arg);
	vTaskDelete(NULL);
}

/* park the running task on wait_obj; retval is returned on resume.
 * wait_kind: 1 = queue/sem/mutex, 2 = event group. */
void s31_rtos_block(uint32_t wait_kind, void *wait_obj, TickType_t timeout,
		      uint32_t retval)
{
	struct s31_tcb *t = s31_current_tcb;

	t->state = S31_TASK_BLOCKED;
	t->wait_kind = wait_kind;
	t->wait_obj = wait_obj;
	t->wake_tick = (timeout == portMAX_DELAY) ? 0 : s31_tick + timeout;
	t->wait_result = 0;
	s31_rtos_switch(&t->ctx_sp, s31_sched_ctx, retval);
}

void s31_rtos_wake_waiters(struct s31_queue *q)
{
	struct s31_waiter **pp = &q->waiters;

	while (*pp) {
		struct s31_waiter *w = *pp;

		if (w->dir == S31_WAIT_RECV &&
		    (q->count > 0 || (q->type == S31_Q_TYPE_MUTEX && !q->owner))) {
			*pp = w->next;
			w->tcb->wait_obj = NULL;
			s31_rtos_make_ready(w->tcb);
			s31_rtos_free(w);
			return;
		}
		if (w->dir == S31_WAIT_SEND && q->count < q->capacity) {
			*pp = w->next;
			w->tcb->wait_obj = NULL;
			s31_rtos_make_ready(w->tcb);
			s31_rtos_free(w);
			return;
		}
		pp = &w->next;
	}
}

/* --- task list helpers --- */
static void s31_enqueue_ready(struct s31_tcb *t)
{
	struct s31_tcb *pos;

	if (!s31_ready_head) {
		s31_ready_head = s31_ready_tail = t;
		t->ready_next = NULL;
		return;
	}
	if (t->priority > s31_ready_head->priority) {
		t->ready_next = s31_ready_head;
		s31_ready_head = t;
		return;
	}
	pos = s31_ready_head;
	while (pos->ready_next &&
	       pos->ready_next->priority >= t->priority)
		pos = pos->ready_next;
	t->ready_next = pos->ready_next;
	pos->ready_next = t;
	if (!t->ready_next)
		s31_ready_tail = t;
}

static struct s31_tcb *s31_dequeue_ready(void)
{
	struct s31_tcb *t = s31_ready_head;

	if (t) {
		s31_ready_head = t->ready_next;
		if (!s31_ready_head)
			s31_ready_tail = NULL;
		t->ready_next = NULL;
	}
	return t;
}

void s31_rtos_make_ready(struct s31_tcb *t)
{
	if (!t || t->state == S31_TASK_READY || t->state == S31_TASK_DELETED)
		return;
	t->state = S31_TASK_READY;
	s31_enqueue_ready(t);
}

/* wake tasks whose delay or wait timeout expired */
static void s31_process_timeouts(void)
{
	struct s31_tcb *t;

	for (t = s31_all_tasks; t; t = t->all_next) {
		if (t->state == S31_TASK_DELAYED) {
			if ((int32_t)(s31_tick - t->wake_tick) >= 0) {
				t->wait_obj = NULL;
				s31_rtos_make_ready(t);
			}
		} else if (t->state == S31_TASK_BLOCKED && t->wake_tick &&
			   (int32_t)(s31_tick - t->wake_tick) >= 0) {
			struct s31_waiter **pp = NULL;

			if (t->wait_kind == 1) {
				struct s31_queue *q = t->wait_obj;

				pp = &q->waiters;
			} else if (t->wait_kind == 2) {
				struct s31_event_group *g = t->wait_obj;

				pp = &g->waiters;
			}
			if (pp) {
				while (*pp) {
					if ((*pp)->tcb == t) {
						struct s31_waiter *w = *pp;

						*pp = w->next;
						s31_rtos_free(w);
						break;
					}
					pp = &(*pp)->next;
				}
			}
			t->wait_obj = NULL;
			s31_rtos_make_ready(t);
		}
	}
}

/* --- scheduler --- */
static void s31_sched_main(void)
{
	for (;;) {
		for (;;) {
			struct s31_tcb *t;

			s31_process_timeouts();
			t = s31_dequeue_ready();
			if (!t)
				break;
			s31_current_tcb = t;
			t->state = S31_TASK_RUNNING;
			s31_rtos_switch(&s31_sched_ctx, t->ctx_sp, 0);
			if (t->state == S31_TASK_DELETED) {
				struct s31_tcb **pp = &s31_all_tasks;

				while (*pp && *pp != t)
					pp = &(*pp)->all_next;
				if (*pp)
					*pp = t->all_next;
				s31_rtos_free(t->stack_base);
				s31_rtos_free(t);
			}
		}
		s31_current_tcb = NULL;
		s31_scheduler_active = 0;
		s31_rtos_switch(&s31_sched_ctx, s31_entry_ctx, 0);
	}
}

static void s31_sched_bootstrap(void)
{
	uint32_t *sp = s31_sched_stack + 512;
	int i;

	sp -= 28;
	sp[0] = (uint32_t)s31_sched_main;
	sp[1] = 0;
	for (i = 2; i < 28; i++)
		sp[i] = 0;
	s31_sched_ctx = sp;
}

void s31_rtos_schedule(void)
{
	if (s31_scheduler_active)
		return;
	s31_scheduler_active = 1;
	s31_scheduler_started = 1;
	if (!s31_sched_ctx)
		s31_sched_bootstrap();
	s31_rtos_switch(&s31_entry_ctx, s31_sched_ctx, 0);
}

/* --- tasks --- */
BaseType_t xTaskCreatePinnedToCore(void (*task_func)(void *), const char *name,
				   uint32_t stack_depth, void *param,
				   UBaseType_t prio, void *task_handle,
				   BaseType_t core_id)
{
	struct s31_tcb *t;
	uint32_t *sp;
	int i;

	(void)core_id; /* single radio hart */
	if (!task_func || stack_depth == 0)
		return pdFAIL;
	t = s31_rtos_malloc(sizeof(*t));
	if (!t)
		return pdFAIL;
	memset(t, 0, sizeof(*t));
	if (name)
		strncpy(t->name, name, S31_TASK_NAME_LEN - 1);
	t->entry = task_func;
	t->arg = param;
	/* ESP-IDF, unlike upstream FreeRTOS, specifies this in bytes. */
	t->stack_size = (stack_depth + 15U) & ~15U;
	t->priority = prio;
	t->state = S31_TASK_READY;
	/* radio-init has already been proven safe in PSRAM and retaining its
	 * 8-KiB stack in the small internal pool starves BLE's DMA allocations.
	 * The separately staged bt-enable call is the one which changes the
	 * external-memory/cache state while its stack is live. */
	sp = s31_rtos_stack_malloc(t->stack_size,
				   s31_force_internal_stacks ||
				   (name && strcmp(name, "bt-enable") == 0));
	if (!sp) {
		s31_rtos_free(t);
		return pdFAIL;
	}
	t->stack_base = sp;
	sp += t->stack_size / 4;
	sp -= 28;
	sp[0] = (uint32_t)s31_rtos_task_entry;
	sp[1] = 0;
	for (i = 2; i < 28; i++)
		sp[i] = 0;
	t->ctx_sp = sp;
	t->all_next = s31_all_tasks;
	s31_all_tasks = t;
	s31_enqueue_ready(t);
	if (task_handle)
		*(void **)task_handle = t;
	return pdPASS;
}

void *xTaskGetCurrentTaskHandle(void)
{
	return s31_current_tcb;
}

void vTaskDelay(TickType_t ticks)
{
	struct s31_tcb *t = s31_current_tcb;

	if (!t || xPortInIsrContext())
		return;
	t->state = S31_TASK_DELAYED;
	t->wake_tick = s31_tick + ticks;
	s31_rtos_switch(&t->ctx_sp, s31_sched_ctx, 0);
}

void vTaskDelete(void *task)
{
	struct s31_tcb *t = task ? task : s31_current_tcb;
	int i;

	if (!t)
		return;
	for (i = 0; i < 4; i++) {
		if (t->tls_dtor[i])
			t->tls_dtor[i](i, t->tls[i]);
	}
	t->state = S31_TASK_DELETED;
	if (t == s31_current_tcb)
		s31_rtos_switch(&t->ctx_sp, s31_sched_ctx, 0);
}

void *pvTaskGetThreadLocalStoragePointer(void *task, int index)
{
	struct s31_tcb *t = task ? task : s31_current_tcb;

	if (!t || index < 0 || index >= 4)
		return NULL;
	return t->tls[index];
}

void vTaskSetThreadLocalStoragePointerAndDelCallback(void *task, int index,
						     void *value,
						     void (*dtor)(int, void *))
{
	struct s31_tcb *t = task ? task : s31_current_tcb;

	if (!t || index < 0 || index >= 4)
		return;
	t->tls[index] = value;
	t->tls_dtor[index] = dtor;
}

void s31_rtos_init(void)
{
	s31_force_internal_stacks = 0;
	s31_sched_stack = s31_rtos_stack_malloc(sizeof(s31_sched_stack_fallback),
						 1);
	if (!s31_sched_stack)
		s31_sched_stack = s31_sched_stack_fallback;
	s31_tick = 0;
	s31_crit_depth = 0;
	s31_current_tcb = NULL;
	s31_ready_head = s31_ready_tail = NULL;
	s31_all_tasks = NULL;
	s31_scheduler_active = 0;
	s31_scheduler_started = 0;
}
