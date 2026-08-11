/*
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * ESP32-S31 radio world: FreeRTOS API stub layer (design doc:
 * docs/S31_radio_rtos_stub.md).  Self-contained, no FreeRTOS headers.
 * Compiled with the ESP toolchain at ilp32f; ABI-compatible with the IDF
 * callers because every public signature is integer/pointer-only.
 */
#ifndef S31_RTOS_H
#define S31_RTOS_H

#include <stdint.h>

typedef int BaseType_t;
typedef unsigned int UBaseType_t;
typedef uint32_t TickType_t;
typedef uint32_t EventBits_t;

#define pdTRUE  ((BaseType_t)1)
#define pdFALSE ((BaseType_t)0)
#define pdPASS  ((BaseType_t)1)
#define pdFAIL  ((BaseType_t)0)
#define errQUEUE_FULL  ((BaseType_t)0)
#define errQUEUE_EMPTY ((BaseType_t)0)

#define portMAX_DELAY ((TickType_t)0xffffffffUL)
#define portTICK_PERIOD_MS 10UL

#define taskSCHEDULER_NOT_STARTED 0
#define taskSCHEDULER_RUNNING     2

#define S31_TASK_NAME_LEN 16

#define S31_TASK_READY    1
#define S31_TASK_RUNNING  2
#define S31_TASK_BLOCKED  3
#define S31_TASK_DELAYED  4
#define S31_TASK_DELETED  5

#define S31_WAIT_RECV 0
#define S31_WAIT_SEND 1

struct s31_tcb {
	char name[S31_TASK_NAME_LEN];
	void (*entry)(void *arg);
	void *arg;
	uint32_t stack_size;
	void *stack_base;
	uint32_t priority;
	uint32_t *ctx_sp;       /* offset-free; only used via pointers */
	uint32_t state;         /* S31_TASK_* */
	uint32_t wait_kind;     /* 1 = queue/sem/mutex, 2 = event group */
	void *wait_obj;         /* queue/event group being waited on */
	TickType_t wake_tick;   /* delay deadline or wait timeout deadline */
	uint32_t wait_result;   /* event bits captured when a waiter is woken */
	void *tls[4];
	void (*tls_dtor[4])(int, void *);
	struct s31_tcb *ready_next;
	struct s31_tcb *all_next;
};

/* single waiter entry, heap-allocated while parked */
struct s31_waiter {
	struct s31_tcb *tcb;
	uint32_t dir;           /* S31_WAIT_* (queues) */
	EventBits_t want;       /* event group: bits needed */
	uint32_t flags;         /* event group: all-bits/clear-on-exit */
	struct s31_waiter *next;
};

#define S31_Q_TYPE_QUEUE 0
#define S31_Q_TYPE_MUTEX 1
#define S31_Q_TYPE_SEM   2

#define S31_QUEUE_SEND_TO_BACK  0
#define S31_QUEUE_SEND_TO_FRONT 1
#define S31_QUEUE_OVERWRITE     2

/* fits inside the IDF StaticQueue_t buffer (<= ~52 bytes) */
struct s31_queue {
	uint32_t type;
	uint32_t item_size;
	uint32_t capacity;
	uint32_t count;
	uint32_t head;          /* next free slot index */
	uint32_t storage;       /* item buffer address (0 for sem/mutex) */
	void *owner;            /* mutex owner tcb */
	uint32_t depth;         /* mutex recursion depth */
	struct s31_waiter *waiters;
	uint32_t is_static;
};

/* 8 bytes, fits inside StaticEventGroup_t */
struct s31_event_group {
	EventBits_t bits;
	struct s31_waiter *waiters;
};

/* --- core --- */
void s31_rtos_init(void);
void s31_rtos_tick(void);             /* from the Linux-owned TIMG1 tick */
int s31_radio_tick_init(void);         /* TIMG1/T1, 1 kHz S-mode tick */
int s31_radio_tick_service(void);      /* called by Linux through SBI */
void s31_radio_tick_handoff_to_linux(void);
BaseType_t s31_rtos_in_isr(void);     /* xPortInIsrContext */
void s31_rtos_schedule(void);         /* run ready tasks until quiescent */
void s31_rtos_enter_critical(void);
void s31_rtos_exit_critical(void);
TickType_t s31_rtos_get_tick(void);
struct s31_tcb *s31_rtos_current(void);
void *s31_rtos_malloc(uint32_t size); /* radio heap */
void s31_rtos_free(void *ptr);
extern uint32_t s31_rtos_isr_depth;   /* world glue sets around radio ISRs */

/* park the current task (internal; used by queue/event implementations) */
void s31_rtos_block(uint32_t wait_kind, void *wait_obj, TickType_t timeout,
		    uint32_t retval);
/* wake a matching waiter on a queue (internal) */
void s31_rtos_wake_waiters(struct s31_queue *q);
void s31_rtos_make_ready(struct s31_tcb *t);

/* cooperative switch: saves integer and ilp32f callee-saved state + retval,
 * stores sp to *prev_ctx, resumes *next_ctx.  Naked asm. */
void s31_rtos_switch(uint32_t **prev_ctx, uint32_t *next_ctx, uint32_t retval);
void s31_rtos_task_entry(void);

/* --- FreeRTOS-compatible API (the 34 measured symbols) --- */
void vPortEnterCritical(void);
void vPortExitCritical(void);
BaseType_t xPortInIsrContext(void);
void vPortYieldFromISR(void);
TickType_t xTaskGetTickCount(void);
TickType_t xTaskGetTickCountFromISR(void);
void vTaskDelay(TickType_t xTicksToDelay);
BaseType_t xTaskCreatePinnedToCore(void (*task_func)(void *), const char *name,
				   uint32_t stack_depth, void *param,
				   UBaseType_t prio, void *task_handle,
				   BaseType_t core_id);
void *xTaskGetCurrentTaskHandle(void);
UBaseType_t xTaskGetSchedulerState(void);
void vTaskDelete(void *task);
void *pvTaskGetThreadLocalStoragePointer(void *task, int index);
void vTaskSetThreadLocalStoragePointerAndDelCallback(void *task, int index,
						     void *value,
						     void (*dtor)(int, void *));

void *xQueueGenericCreate(uint32_t queue_len, uint32_t item_size,
			  uint8_t queue_type);
void *xQueueGenericCreateStatic(uint32_t queue_len, uint32_t item_size,
				uint8_t *storage, void *static_queue,
				uint8_t queue_type);
BaseType_t xQueueGenericGetStaticBuffers(void *queue, uint8_t **storage,
					 void **static_queue);
void *xQueueCreateMutex(uint8_t type);
void *xQueueCreateCountingSemaphore(uint32_t max, uint32_t initial);
void vQueueDelete(void *queue);
BaseType_t xQueueGenericSend(void *queue, const void *item,
			     TickType_t timeout, BaseType_t copy);
BaseType_t xQueueGenericSendFromISR(void *queue, const void *item,
				    BaseType_t *woken);
BaseType_t xQueueGiveFromISR(void *queue, BaseType_t *woken);
BaseType_t xQueueReceive(void *queue, void *item, TickType_t timeout);
BaseType_t xQueueReceiveFromISR(void *queue, void *item, BaseType_t *woken);
BaseType_t xQueueSemaphoreTake(void *queue, TickType_t timeout);
BaseType_t xQueueTakeMutexRecursive(void *queue, TickType_t timeout);
BaseType_t xQueueGiveMutexRecursive(void *queue);
void *xQueueGetMutexHolder(void *queue);
UBaseType_t uxQueueMessagesWaiting(void *queue);
UBaseType_t uxQueueMessagesWaitingFromISR(void *queue);
BaseType_t xQueueGenericReset(void *queue, BaseType_t new_queue);
BaseType_t xQueueIsQueueEmptyFromISR(void *queue);

void *xEventGroupCreate(void);
EventBits_t xEventGroupSetBits(void *group, EventBits_t bits);
EventBits_t xEventGroupClearBits(void *group, EventBits_t bits);
EventBits_t xEventGroupWaitBits(void *group, EventBits_t bits,
				BaseType_t clear_on_exit,
				BaseType_t wait_all_bits,
				TickType_t timeout);
void vEventGroupDelete(void *group);

#endif /* S31_RTOS_H */
