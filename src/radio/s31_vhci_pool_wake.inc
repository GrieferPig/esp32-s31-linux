/* SPDX-License-Identifier: BSD-2-Clause */
/* CPU0 owns the rollback marker; nested calls restore the previous marker.
 * Classify by exact block so an IRQ returning a different block still
 * publishes its real availability edge.
 * Rollback of the current failed attempt creates no net pool availability. */
static void *s31_vhci_undo_block;
void *s31_vhci_pool_undo_enter(void *block)
{
    void *previous = s31_vhci_undo_block;
    s31_vhci_undo_block = block;
    return previous;
}
void s31_vhci_pool_undo_leave(void *previous)
{
    s31_vhci_undo_block = previous;
}
#define S31_MSYS_RUNTIME_FLAG 0x02U /* r__os_msys_find_pool tests bit1. */
btdm_osal_error_t __real_r_os_memblock_put(struct btdm_mempool *pool, void *block);
btdm_osal_error_t __real_r_os_memblock_put_from_cb(struct btdm_mempool *pool, void *block);
void s31_radio_vhci_send_available(void);
/* The HCI packet allocator selects runtime MSYS pools (the same runtime flag
 * checked by r__os_msys_find_pool). A successful return is a retry hint, not
 * fabricated controller credit. RX may return these shared blocks without a
 * Number-of-Completed-Packets event. The existing native callback only wakes
 * a pending TX ring and follows the executor's producer/stopping protocol.
 * Snapshot eligibility before a custom callback; do not inspect its pool after
 * returning. No pool lock is held by this wrapper around the native wake. */
btdm_osal_error_t __wrap_r_os_memblock_put(struct btdm_mempool *pool, void *block)
{
    int eligible = pool && (pool->mp_flags & S31_MSYS_RUNTIME_FLAG) &&
                   (!s31_vhci_undo_block || block != s31_vhci_undo_block);
    btdm_osal_error_t rc = __real_r_os_memblock_put(pool, block);
    if (!rc && eligible)
        s31_radio_vhci_send_available();
    return rc;
}
btdm_osal_error_t __wrap_r_os_memblock_put_from_cb(struct btdm_mempool *pool, void *block)
{
    int eligible = pool && (pool->mp_flags & S31_MSYS_RUNTIME_FLAG) &&
                   (!s31_vhci_undo_block || block != s31_vhci_undo_block);
    btdm_osal_error_t rc = __real_r_os_memblock_put_from_cb(pool, block);
    if (!rc && eligible)
        s31_radio_vhci_send_available();
    return rc;
}
