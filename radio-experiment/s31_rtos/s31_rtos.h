/*
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * ESP32-S31 radio world: FreeRTOS API stub layer.  The payload keeps the
 * measured FreeRTOS ABI, while task scheduling and blocking are delegated to
 * the Linux-side bridge in the S-mode driver.
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

struct s31_tcb {
	char name[S31_TASK_NAME_LEN];
	void (*entry)(void *arg);
	void *arg;
	uint32_t priority;
	void *linux_task;       /* opaque Linux task object */
	void *stack_base;       /* internal-SRAM payload stack */
	uint32_t stack_size;
	void *tls[4];
	void (*tls_dtor[4])(int, void *);
	uint32_t critical_depth;
	uint32_t critical_flags;
	uint32_t tls_cleaned;
	uint32_t notify_value;
	uint32_t notify_state;
	void *notify_context;
	void *suspend_context;
};

#define S31_Q_TYPE_QUEUE 0
#define S31_Q_TYPE_MUTEX 1
#define S31_Q_TYPE_SEM   2

#define S31_QUEUE_SEND_TO_BACK  0
#define S31_QUEUE_SEND_TO_FRONT 1
#define S31_QUEUE_OVERWRITE     2

/* Fixed payload/kernel ABI values used only for gate timing diagnostics. */
enum s31_blob_release_reason {
	S31_BLOB_RELEASE_LEAVE = 0,
	S31_BLOB_RELEASE_TASK_DELAY,
	S31_BLOB_RELEASE_TASK_YIELD,
	S31_BLOB_RELEASE_QUEUE_SEND,
	S31_BLOB_RELEASE_QUEUE_RECEIVE,
	S31_BLOB_RELEASE_SEMAPHORE_TAKE,
	S31_BLOB_RELEASE_NOTIFY_TAKE,
	S31_BLOB_RELEASE_NOTIFY_WAIT,
	S31_BLOB_RELEASE_EVENT_WAIT,
	S31_BLOB_RELEASE_TASK_SUSPEND,
	S31_BLOB_RELEASE_COUNT,
};

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
	void *wait_context;     /* opaque Linux lock/wait object */
	uint32_t is_static;
};

/* 8 bytes, fits inside StaticEventGroup_t */
struct s31_event_group {
	EventBits_t bits;
	void *wait_context;     /* opaque Linux lock/wait object */
};

/* --- core --- */
void s31_rtos_init(void);
void s31_rtos_tick(void);             /* worker callback pass */
void s31_rtos_hard_tick(void);        /* legacy hard-IRQ no-op */
int s31_radio_tick_init(void);         /* legacy, unused */
int s31_radio_tick_service(void);      /* called by Linux through SBI */
void s31_radio_tick_handoff_to_linux(void);
BaseType_t s31_rtos_in_isr(void);     /* xPortInIsrContext */
BaseType_t s31_rtos_can_yield(void);  /* S31 xPortCanYield semantics */
void s31_rtos_enter_critical(void);
void s31_rtos_exit_critical(void);
TickType_t s31_rtos_get_tick(void);
struct s31_tcb *s31_rtos_current(void);
void *s31_rtos_malloc(uint32_t size); /* radio heap */
void s31_rtos_free(void *ptr);
extern uint32_t s31_rtos_isr_depth;   /* world glue sets around radio ISRs */

/* Linux bridge.  This ABI deliberately contains only fixed-width integers,
 * opaque pointers, and function pointers; it is shared by the ESP payload and
 * the Linux kernel driver without including kernel headers in the payload. */
void *s31_linux_task_create(void (*entry)(void *), const char *name,
				uint32_t stack_size, void *stack_base,
				void *arg, uint32_t priority, void *cookie);
void s31_linux_task_exit_current(void);
int32_t s31_linux_task_stop(void *task);
void *s31_linux_current_cookie(void);
void s31_linux_task_delay(TickType_t ticks);
uint32_t s31_linux_tick_count(void);
uint64_t s31_linux_time_ns(void);
void s31_linux_printf(const char *fmt, ...);
void s31_linux_task_yield(void);
void s31_linux_task_set_priority(void *task, UBaseType_t priority);
void s31_rtos_task_release(void *cookie);

void s31_linux_blob_enter(void);
void s31_linux_blob_leave(void);
void s31_linux_blob_suspend(uint32_t reason);
void s31_linux_blob_resume(void);
void s31_linux_trace_wifi_event(uint32_t event);

void *s31_linux_sync_create(void);
void s31_linux_sync_destroy(void *sync);
void s31_linux_sync_lock(void *sync);
void s31_linux_sync_unlock(void *sync);
uint32_t s31_linux_sync_sequence(void *sync);
int32_t s31_linux_sync_wait(void *sync, uint32_t sequence,
				    TickType_t timeout, uint32_t reason);
void s31_linux_sync_wake(void *sync);

uint32_t s31_linux_critical_enter(void);
void s31_linux_critical_exit(uint32_t flags);
void s31_linux_critical_suspend(void);
void s31_linux_critical_resume(void);

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
				    BaseType_t *woken, BaseType_t copy);
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
