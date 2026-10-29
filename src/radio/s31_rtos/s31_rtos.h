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

struct s31_tcb;
struct s31_task_cleanup {
    struct s31_task_cleanup *next;
    struct s31_tcb *owner;
    void (*release)(struct s31_task_cleanup *cleanup);
};

struct s31_queue_state;
struct s31_queue_call {
    struct s31_task_cleanup cleanup;
    struct s31_queue_state *state;
};

struct s31_event_state;
struct s31_event_waiter {
    struct s31_task_cleanup cleanup;
    struct s31_event_state *state;
    int linked;
    struct s31_event_waiter *next;
    EventBits_t want, result;
    uint32_t flags;
    int done;
};

struct s31_tcb {
	char name[S31_TASK_NAME_LEN];
	void (*entry)(void *arg);
	void *arg;
	uint32_t priority;      /* fixed level for bootstrap/foreign identity */
	int32_t core_id;          /* IDF affinity: 0, 1, or tskNO_AFFINITY */
	void *linux_task;       /* opaque Linux task object */
	/* Separate typed protocol; never a managed task/stop-frame pointer. */
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
    struct s31_task_cleanup *cleanups;
    uint32_t tls_finalizing;
    /* A durable task-local admission survives a body-stack unwind. */
    union {
        struct s31_queue_call native_call;
        struct s31_event_waiter event_call;
    };
};

#define S31_Q_TYPE_QUEUE 0
#define S31_Q_TYPE_MUTEX 1
#define S31_Q_TYPE_SEM   2

/* Native IDF freertos/queue.h constructor subtype values. */
#define S31_MUTEX_NORMAL 1
#define S31_MUTEX_RECURSIVE 4

#define S31_QUEUE_SEND_TO_BACK  0
#define S31_QUEUE_SEND_TO_FRONT 1
#define S31_QUEUE_OVERWRITE     2

/* Wait-purpose ABI used for native admission and cancellation. */
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
#define S31_SYNC_WAIT_TX_REPAIR (1U << 31)

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

#if UINTPTR_MAX == UINT32_MAX
_Static_assert(sizeof(struct s31_queue) <= 52, "S31 compact StaticQueue ABI");
#endif

/* 8 bytes, fits inside StaticEventGroup_t */
struct s31_event_group {
	EventBits_t bits;
	void *wait_context;     /* opaque Linux lock/wait object */
};

/* --- core --- */

void s31_rtos_init(void);
/* Only at new-runtime init after prior task/work/IRQ joins and heap reclaim. */
void s31_rtos_event_runtime_reset(void);
void s31_rtos_queue_runtime_reset(void);
void s31_rtos_timer_runtime_reset(void);
void s31_rtos_tick(void);             /* worker callback pass */
void s31_rtos_hard_tick(void);        /* legacy hard-IRQ no-op */
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
void *s31_linux_task_create_finalized(void (*entry)(void *), const char *name,
    uint32_t stack_size, void *stack_base, void *arg, uint32_t priority,
    void *cookie, int32_t core_id, void (*finalize)(void *), void **ready_slot);
void s31_linux_task_finalize_report(int32_t status);
void s31_linux_task_exit_current(void);
int32_t s31_linux_task_stop(void *task);
void *s31_linux_current_cookie(void);
void s31_linux_task_delay(TickType_t ticks);
uint32_t s31_linux_tick_count(void);
uint64_t s31_linux_time_ns(void);
/* Actual Linux hard/soft IRQ context; creation admission only. */
int32_t s31_linux_in_interrupt(void);
int32_t s31_linux_current_cpu(void);
uint64_t s31_linux_timer_next_deadline_ns(void);
void s31_linux_printf(const char *fmt, ...);
void s31_linux_task_yield(void);
/* Effective FreeRTOS level plus boost flag: boost=1 saves the Linux state
 * once and maps the level (never demoting below the live level); boost=0
 * restores the saved state when boosted, else keeps the legacy policy. */
void s31_linux_task_set_priority(void *task, UBaseType_t priority, uint32_t boost);
void s31_rtos_task_release(void *cookie);
BaseType_t s31_rtos_cleanup_push(struct s31_task_cleanup *cleanup);
void s31_rtos_cleanup_pop(struct s31_task_cleanup *cleanup);
void s31_rtos_cleanup_drain(struct s31_tcb *task);

/* Opaque caller window: compatibility callers pass NULL (their task owns
 * it); foreign callers pass per-invocation storage. Integer/pointer-only,
 * so the header stays ABI-compatible with IDF callers. */
struct s31_blob_context;

void *s31_linux_sync_create(void);
/* Private integer-only native identity protocol, never a public IDF ABI. */
enum {
	S31_SYNC_OWNER_CLAIM = 0,
	S31_SYNC_OWNER_MATCH = 1,
	S31_SYNC_OWNER_COOKIE = 2,
	S31_SYNC_OWNER_DROP = 3,
	S31_SYNC_OWNER_ABANDON = 4,
	S31_SYNC_OWNER_ALIVE = 5,
	S31_SYNC_OWNER_FORCE_DROP = 6,
	S31_SYNC_OWNER_FATAL = 7,
	S31_SYNC_MUX_ENTER = 8,
	S31_SYNC_MUX_EXIT = 9,
	S31_SYNC_CRITICAL_DEPTH = 10,
	S31_SYNC_OWNER_FOREIGN_TOKEN = 11,
	S31_SYNC_ERRNO_REGISTER = 16,
	S31_SYNC_NATIVE_MODE = 17,
	S31_SYNC_NATIVE_INIT = 18,
	S31_SYNC_NATIVE_BEGIN = 19,
	S31_SYNC_NATIVE_END = 20,
	S31_SYNC_NATIVE_BUSY = 21,
	S31_SYNC_NATIVE_AVAILABLE = 22,
	S31_SYNC_NATIVE_TAKE = 23,
	S31_SYNC_NATIVE_RELEASE = 24,
	S31_SYNC_NATIVE_DELETE = 25,
	S31_SYNC_NATIVE_DESTROY = 26,
	S31_SYNC_NATIVE_EFFECTIVE = 27,
	S31_SYNC_NATIVE_BASE = 28,
	S31_SYNC_NATIVE_BASE_QUERY = 29,
	S31_SYNC_STACK_RECLAIM = 36,
	S31_SYNC_TIMER_CHANGED = 39,
	S31_SYNC_TX_LOCK_BIND = 40,
	S31_SYNC_TX_FLIGHT_RETURN = 41,
	S31_SYNC_STACK_RAW_MINIMUM = 42,
	S31_SYNC_STACK_ALLOC = 43,
	S31_SYNC_STACK_FREE = 44,
	S31_SYNC_SOFTMAC_TX_COPY = 45,
	S31_SYNC_TX_REPAIR_CAP = 47,
	S31_SYNC_TX_POOL_BIND = 48,
	S31_SYNC_AUX_CONTEXT = 49,
};
uintptr_t s31_linux_sync_owner(void *sync, uint32_t op);
void s31_linux_sync_destroy(void *sync);
int s31_rtos_mutex_tx_credit_bind(void *handle);
void s31_rtos_tx_flight_return(void);
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
void vTaskPrioritySet(void *task, UBaseType_t priority);
UBaseType_t uxTaskPriorityGet(void *task);
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
void *xQueueCreateMutexStatic(uint8_t type, void *static_queue);
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
