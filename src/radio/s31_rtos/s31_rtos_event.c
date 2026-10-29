/* SPDX-License-Identifier: BSD-2-Clause */
/* Per-waiter set snapshots with sharded admission and atomic lifetimes. */
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include "s31_rtos.h"

#define S31_EG_FLAG_ALL_BITS (1U << 0)
#define S31_EG_FLAG_CLEAR    (1U << 1)

struct s31_event_state {
    struct s31_event_state *next;
    struct s31_event_group *group;
    void *sync;
    struct s31_event_waiter *waiters;
    uint32_t refs; /* published handle reference plus admitted calls */
    int deleted;  /* Object sync protects delivery and waiters. */
    int detached; /* Bucket exclusion closes handle admission. */
};

/* Compare raw handles without dereferencing potentially freed storage.
 * Keep detached records until the last admitted call/waiter retires. This
 * preserves dynamic-handle lifetime without promising stale-handle ABA
 * protection. No runtime-wide registry lock or per-operation put scan. */
#define S31_EVENT_BUCKETS 32U
#define S31_EVENT_MUX_FREE 0xb33fffffU
struct s31_event_bucket {
    struct { uint32_t owner, count; } mux;
    struct s31_event_state *head;
};
static struct s31_event_bucket s31_event_buckets[S31_EVENT_BUCKETS] = {
    [0 ... S31_EVENT_BUCKETS - 1] = { .mux.owner = S31_EVENT_MUX_FREE },
};
extern BaseType_t xPortEnterCriticalTimeout(void *mux, BaseType_t timeout);
extern void vPortExitCriticalMultiCore(void *mux);

static struct s31_event_bucket *s31_event_bucket_for(const void *handle)
{
    uintptr_t key = (uintptr_t)handle >> 3;
    key ^= key >> 7;
    return &s31_event_buckets[key & (S31_EVENT_BUCKETS - 1)];
}
static void s31_event_bucket_lock(struct s31_event_bucket *bucket)
{
    if (!xPortEnterCriticalTimeout(&bucket->mux, -1))
        s31_linux_sync_owner(NULL, S31_SYNC_OWNER_FATAL);
}
static void s31_event_bucket_unlock(struct s31_event_bucket *bucket)
{
    vPortExitCriticalMultiCore(&bucket->mux);
}
void s31_rtos_event_runtime_reset(void)
{
    /* Only after every task/work/IRQ owner joined and the old heap retired. */
    for (unsigned int i = 0; i < S31_EVENT_BUCKETS; i++) {
        s31_event_buckets[i].head = NULL;
        s31_event_buckets[i].mux.owner = S31_EVENT_MUX_FREE;
        s31_event_buckets[i].mux.count = 0;
    }
}
static struct s31_event_state *s31_event_get(void *group)
{
    struct s31_event_state *state;
    struct s31_event_bucket *bucket;
    uint32_t old;

    if (!group) return NULL;
    bucket = s31_event_bucket_for(group);
    s31_event_bucket_lock(bucket);
    for (state = bucket->head; state; state = state->next) {
        if (state->group == group && !state->detached) {
            old = __atomic_fetch_add(&state->refs, 1, __ATOMIC_RELAXED);
            if (!old || old == UINT32_MAX)
                s31_linux_sync_owner(NULL, S31_SYNC_OWNER_FATAL);
            break;
        }
    }
    s31_event_bucket_unlock(bucket);
    return state;
}
static void s31_event_put(struct s31_event_state *state)
{
    struct s31_event_bucket *bucket;
    struct s31_event_state **link;
    uint32_t old = __atomic_fetch_sub(&state->refs, 1, __ATOMIC_RELEASE);

    if (!old) s31_linux_sync_owner(NULL, S31_SYNC_OWNER_FATAL);
    if (old != 1) return;
    __atomic_thread_fence(__ATOMIC_ACQUIRE);
    bucket = s31_event_bucket_for(state->group);
    s31_event_bucket_lock(bucket);
    if (!state->detached)
        s31_linux_sync_owner(NULL, S31_SYNC_OWNER_FATAL);
    for (link = &bucket->head; *link && *link != state; link = &(*link)->next) {}
    if (!*link) s31_linux_sync_owner(NULL, S31_SYNC_OWNER_FATAL);
    *link = state->next;
    s31_event_bucket_unlock(bucket);
    s31_linux_sync_destroy(state->sync);
    s31_rtos_free(state->group);
    s31_rtos_free(state);
}

static int s31_eg_match(EventBits_t have, EventBits_t want, uint32_t flags)
{
    if (flags & S31_EG_FLAG_ALL_BITS)
        return (have & want) == want;
    return (have & want) != 0;
}

void *xEventGroupCreate(void)
{
    struct s31_event_group *group = s31_rtos_malloc(sizeof(*group));
    struct s31_event_state *state;
    struct s31_event_bucket *bucket;

    if (!group)
        return NULL;
    state = s31_rtos_malloc(sizeof(*state));
    if (!state) {
        s31_rtos_free(group);
        return NULL;
    }
    memset(group, 0, sizeof(*group));
    memset(state, 0, sizeof(*state));
    state->sync = s31_linux_sync_create();
    if (!state->sync) {
        if (state->sync)
            s31_linux_sync_destroy(state->sync);
        s31_rtos_free(state);
        s31_rtos_free(group);
        return NULL;
    }
    state->group = group;
    state->refs = 1;
    group->wait_context = state;
    bucket = s31_event_bucket_for(group);
    s31_event_bucket_lock(bucket);
    state->next = bucket->head;
    bucket->head = state;
    s31_event_bucket_unlock(bucket);
    return group;
}

void vEventGroupDelete(void *group)
{
    struct s31_event_state *state;
    struct s31_event_waiter *waiter;
    struct s31_event_bucket *bucket;

    if (!group) return;
    bucket = s31_event_bucket_for(group);
    s31_event_bucket_lock(bucket);
    for (state = bucket->head; state; state = state->next) {
        if (state->group == group && !state->detached) {
            state->detached = 1;
            break;
        }
    }
    s31_event_bucket_unlock(bucket);
    if (!state)
        return;

    /* Retain the published handle reference throughout delete/wake.
     * Admitted calls retain their own references until they finish. */
    s31_linux_sync_lock(state->sync);
    state->deleted = 1;
    for (waiter = state->waiters; waiter; waiter = waiter->next) {
        if (!waiter->done) {
            waiter->result = 0;
            waiter->done = 1;
        }
    }
    s31_linux_sync_unlock(state->sync);
    s31_linux_sync_wake(state->sync);
    s31_event_put(state);
}

EventBits_t xEventGroupSetBits(void *group, EventBits_t bits)
{
    struct s31_event_state *state = s31_event_get(group);
    struct s31_event_waiter *waiter;
    EventBits_t snapshot, clear = 0, result = 0;

    if (!state)
        return 0;
    s31_linux_sync_lock(state->sync);
    if (!state->deleted) {
        state->group->bits |= bits;
        snapshot = state->group->bits;
        /* Evaluate EVERY blocked waiter against one immutable set snapshot.
         * Store each result before aggregate clear-on-exit consumption. */
        for (waiter = state->waiters; waiter; waiter = waiter->next) {
            if (!waiter->done &&
                s31_eg_match(snapshot, waiter->want, waiter->flags)) {
                waiter->result = snapshot;
                waiter->done = 1;
                if (waiter->flags & S31_EG_FLAG_CLEAR)
                    clear |= waiter->want;
            }
        }
        state->group->bits &= ~clear;
        result = state->group->bits;
    }
    s31_linux_sync_unlock(state->sync);
    s31_linux_sync_wake(state->sync);
    s31_event_put(state);
    return result;
}

EventBits_t xEventGroupClearBits(void *group, EventBits_t bits)
{
    struct s31_event_state *state = s31_event_get(group);
    EventBits_t previous = 0;

    if (!state)
        return 0;
    s31_linux_sync_lock(state->sync);
    if (!state->deleted) {
        previous = state->group->bits;
        state->group->bits &= ~bits;
    }
    s31_linux_sync_unlock(state->sync);
    s31_event_put(state);
    return previous;
}

/* A waiter owns the admitted state reference until normal return or the
 * real task-release continuation.  It must not reside on an abandoned stack.
 */
static void s31_event_waiter_release(struct s31_task_cleanup *cleanup)
{
    struct s31_event_waiter *waiter =
        (struct s31_event_waiter *)((char *)cleanup -
                                   offsetof(struct s31_event_waiter, cleanup));
    struct s31_event_state *state = waiter->state;
    struct s31_event_waiter **link;

    s31_linux_sync_lock(state->sync);
    if (waiter->linked) {
        for (link = &state->waiters; *link; link = &(*link)->next) {
            if (*link == waiter) {
                *link = waiter->next;
                break;
            }
        }
        waiter->linked = 0;
    }
    s31_linux_sync_unlock(state->sync);
    s31_event_put(state);
    waiter->state = NULL;
    waiter->cleanup.release = NULL;
}

EventBits_t xEventGroupWaitBits(void *group, EventBits_t bits,
                              BaseType_t clear_on_exit,
                              BaseType_t wait_all_bits,
                              TickType_t timeout)
{
    struct s31_event_state *state;
    struct s31_event_waiter *waiter;
    struct s31_tcb *task;
    TickType_t start;
    EventBits_t result = 0;
    int enrolled;
    int32_t waited = 1;

    /* WaitBits is process-context only, including immediate matches and
     * zero-time polls.  Reject before any admission, allocation or TCB read. */
    if (s31_rtos_in_isr())
        return 0;
    state = s31_event_get(group);
    start = s31_rtos_get_tick();
    if (!state)
        return 0;
    /* Immediate matches and zero-time polls need no cancellation record.
     * Linearize this observation/consumption under the object lock. A true
     * blocking call uses its durable task-local waiter and rechecks below. */
    s31_linux_sync_lock(state->sync);
    result = state->deleted ? 0 : state->group->bits;
    if (state->deleted || !timeout ||
        s31_eg_match(state->group->bits, bits,
                     wait_all_bits ? S31_EG_FLAG_ALL_BITS : 0)) {
        if (!state->deleted) {
            result = state->group->bits;
            if (clear_on_exit && s31_eg_match(result, bits,
                    wait_all_bits ? S31_EG_FLAG_ALL_BITS : 0))
                state->group->bits &= ~bits;
        }
        s31_linux_sync_unlock(state->sync);
        s31_event_put(state);
        return result;
    }
    s31_linux_sync_unlock(state->sync);
    task = s31_rtos_current();
    if (!task || !task->linux_task) {
        s31_event_put(state);
        return result; /* Foreign callers retain their immediate-poll ABI. */
    }
    waiter = &task->event_call;
    if (waiter->state) s31_linux_sync_owner(NULL, S31_SYNC_OWNER_FATAL);
    memset(waiter, 0, sizeof(*waiter));
    waiter->state = state;
    waiter->want = bits;
    waiter->flags = (wait_all_bits ? S31_EG_FLAG_ALL_BITS : 0) |
                    (clear_on_exit ? S31_EG_FLAG_CLEAR : 0);
    waiter->cleanup.release = s31_event_waiter_release;
    enrolled = s31_rtos_cleanup_push(&waiter->cleanup);
    for (;;) {
        TickType_t remaining = timeout;
        uint32_t sequence;

        s31_linux_sync_lock(state->sync);
        if (waiter->done) {
            result = waiter->result;
            break;
        }
        if (state->deleted)
            break;
        result = state->group->bits;
        if (s31_eg_match(result, bits, waiter->flags)) {
            if (clear_on_exit)
                state->group->bits &= ~bits;
            break;
        }
        if (timeout != portMAX_DELAY) {
            TickType_t elapsed = s31_rtos_get_tick() - start;
            remaining = elapsed >= timeout ? 0 : timeout - elapsed;
        }
        /* Foreign workers have no compatibility-task final continuation.
         * They may poll/immediately consume bits, but must not create an
         * untracked blocking reference.  Do not rely on caller discipline.
         */
        if (!remaining || waited <= 0 || s31_rtos_in_isr() || !enrolled)
            break;
        if (!waiter->linked) {
            waiter->next = state->waiters;
            state->waiters = waiter;
            waiter->linked = 1;
        }
        sequence = s31_linux_sync_sequence(state->sync);
        s31_linux_sync_unlock(state->sync);
        waited = s31_linux_sync_wait(state->sync, sequence, remaining,
                                     S31_BLOB_RELEASE_EVENT_WAIT);
    }
    s31_linux_sync_unlock(state->sync);
    s31_rtos_cleanup_pop(&waiter->cleanup);
    s31_event_waiter_release(&waiter->cleanup);
    return result;
}
