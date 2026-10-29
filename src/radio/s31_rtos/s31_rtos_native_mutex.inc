/* SPDX-License-Identifier: BSD-2-Clause */
/* Included after generic queue admission helpers. Each native task owns one
 * durable call node; ISR mutex APIs refuse admission, and callbacks retire a
 * body call before invoking TLS destructors. No heap allocation per take/give.
 */
static void s31_native_call_drop(struct s31_queue_call *call)
{
    struct s31_queue_state *state = call->state;
    if (!state) s31_linux_sync_owner(NULL, S31_SYNC_OWNER_FATAL);
    s31_queue_put(state);
    call->state = NULL;
    call->cleanup.release = NULL;
}

static void s31_native_call_release(struct s31_task_cleanup *cleanup)
{
    struct s31_queue_call *call = (struct s31_queue_call *)
        ((char *)cleanup - offsetof(struct s31_queue_call, cleanup));
    s31_native_call_drop(call);
}

static void s31_native_attempt_release(struct s31_task_cleanup *cleanup)
{
    struct s31_queue_call *call = (struct s31_queue_call *)
        ((char *)cleanup - offsetof(struct s31_queue_call, cleanup));
    /* A failed normal self-take still owns the old published span. RELEASE
     * unwinds only a new, unpublished reservation, never that older owner.
     */
    s31_linux_sync_owner(call->state->sync, S31_SYNC_NATIVE_RELEASE);
    s31_linux_sync_owner(call->state->sync, S31_SYNC_NATIVE_END);
    s31_native_call_drop(call);
}

/* Publication transferred the reservation to the independent owner lease,
 * or the last give already completed the real unlock. Only the attempt and
 * admitted-call references remain. Failure/stop before either boundary keeps
 * s31_native_attempt_release, which still returns unpublished reservations. */
static void s31_native_attempt_settled(struct s31_task_cleanup *cleanup)
{
    struct s31_queue_call *call = (struct s31_queue_call *)
        ((char *)cleanup - offsetof(struct s31_queue_call, cleanup));
    s31_linux_sync_owner(call->state->sync, S31_SYNC_NATIVE_END);
    s31_native_call_drop(call);
}

static int s31_native_call_prepare(struct s31_queue_state *state,
                                   struct s31_queue_call **result)
{
    /* Owner-only cleanup publication cannot sleep or acquire another object
     * lock. Like owner-lease publication in s31_mutex_claim_locked(), it is
     * safe inside this object's metadata fence. Keep preparation and BEGIN
     * in that fence instead of dropping/reacquiring it on every take/give. */
    struct s31_tcb *t = s31_rtos_current();
    struct s31_queue_call *call;
    if (!t) return 0;
    call = &t->native_call;
    if (call->state) s31_linux_sync_owner(NULL, S31_SYNC_OWNER_FATAL);
    call->state = state;
    call->cleanup.release = s31_native_call_release;
    if (!s31_rtos_cleanup_push(&call->cleanup)) {
        call->state = NULL;
        call->cleanup.release = NULL;
        return 0;
    }
    *result = call; /* Transfer this admitted call's reference. */
    return 1;
}

static int s31_native_call_begin_locked(struct s31_queue_state *state,
                                        struct s31_queue_call *call)
{
    /* Both callers already checked deletion/abandonment in this same
     * metadata fence; owner-only cleanup preparation cannot sleep. */
    if (!s31_linux_sync_owner(state->sync, S31_SYNC_NATIVE_BEGIN))
        return 0;
    call->cleanup.release = s31_native_attempt_release;
    return 1;
}

static BaseType_t s31_native_mutex_take(struct s31_queue_state *state,
                                       struct s31_queue_call **call,
                                       TickType_t timeout, int recursive)
{
    struct s31_queue *q = state->queue;
    TickType_t start = timeout && timeout != portMAX_DELAY ?
        s31_rtos_get_tick() : 0, remaining;
    BaseType_t result = pdFALSE;

    s31_linux_sync_lock(state->sync);
    if (state->deleted || state->owner_abandoned) {
        s31_linux_sync_unlock(state->sync);
        return pdFALSE;
    }
    if (recursive && q->depth &&
        s31_linux_sync_owner(state->sync, S31_SYNC_OWNER_MATCH)) {
        if (q->depth != UINT32_MAX) { q->depth++; result = pdTRUE; }
        s31_linux_sync_unlock(state->sync);
        return result;
    }
    result = s31_native_call_prepare(state, call) &&
        s31_native_call_begin_locked(state, *call);
    s31_linux_sync_unlock(state->sync);
    if (!result) return pdFALSE;
    remaining = s31_wait_remaining(timeout, start);
    if (s31_linux_sync_wait(state->sync, 0, remaining,
                           S31_BLOB_RELEASE_SEMAPHORE_TAKE) <= 0)
        return pdFALSE;
    s31_linux_sync_lock(state->sync);
    result = !state->deleted && !state->owner_abandoned &&
        s31_mutex_claim_locked(state, s31_rtos_current());
    if (result)
        (*call)->cleanup.release = s31_native_attempt_settled;
    s31_linux_sync_unlock(state->sync);
    return result;
}

static BaseType_t s31_native_mutex_give(struct s31_queue_state *state,
                                       struct s31_queue_call **call)
{
    struct s31_queue *q = state->queue;
    BaseType_t result;

    s31_linux_sync_lock(state->sync);
    if (state->deleted || state->owner_abandoned || !q->depth ||
        !s31_linux_sync_owner(state->sync, S31_SYNC_OWNER_MATCH)) {
        s31_linux_sync_unlock(state->sync);
        return pdFALSE;
    }
    if (q->depth > 1) {
        q->depth--;
        s31_linux_sync_unlock(state->sync);
        return pdTRUE;
    }
    result = s31_native_call_prepare(state, call) &&
        s31_native_call_begin_locked(state, *call) &&
        state->owner_cleanup_registered;
    if (result) {
        s31_rtos_cleanup_pop(&state->owner_cleanup);
        state->owner_cleanup_registered = 0;
        if (!s31_linux_sync_owner(state->sync, S31_SYNC_OWNER_DROP))
            s31_linux_sync_owner(NULL, S31_SYNC_OWNER_FATAL);
        q->owner = NULL;
        q->count = q->depth = 0;
    }
    s31_linux_sync_unlock(state->sync);
    if (result) {
        if (!s31_linux_sync_owner(state->sync, S31_SYNC_NATIVE_RELEASE))
            s31_linux_sync_owner(NULL, S31_SYNC_OWNER_FATAL);
        (*call)->cleanup.release = s31_native_attempt_settled;
        s31_queue_put(state); /* Published owner lease ends after unlock. */
    }
    return result;
}
