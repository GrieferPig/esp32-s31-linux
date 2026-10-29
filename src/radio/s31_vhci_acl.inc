/* SPDX-License-Identifier: BSD-2-Clause */
#if UC_BT_CTRL_BLE_IS_ENABLE
/* Serialized core0 H2C BLE ACL ownership. Append may partially build a chain;
 * a failed append still owns the whole head+suffix. The pinned BLE consumer
 * transfers ownership and returns0. Its open forward wrapper's status failure
 * is before transfer, so a nonzero forward result still owns this chain. */
void *s31_vhci_pool_undo_enter(void *block);
void s31_vhci_pool_undo_leave(void *previous);
static void s31_ble_acl_release_owned(struct ble_mbuf *head, int undo)
{
    /* Detach each still-owned node before returning it. Do not traverse freed
     * memory and do not publish a retry for recycling the attempt's own block.
     * A nested IRQ returning another block remains eligible for its wake. */
    while (head) {
        struct ble_mbuf *next = SLIST_NEXT(head, om_next);
        void *previous = undo ? s31_vhci_pool_undo_enter(head) : NULL;
        int rc;
        SLIST_NEXT(head, om_next) = NULL;
        rc = ble_mbuf_free_chain(head);
        if (undo) s31_vhci_pool_undo_leave(previous);
        /* Depletion is retriable; pool corruption is an invariant violation. */
        assert(rc == 0);
        (void)rc;
        head = next;
    }
}
static int s31_vhci_ble_acl_send(uint8_t *data, uint16_t length)
{
    struct ble_mbuf *head;
    uint16_t pkt_len = length - 1;
    int rc;

    if (!s_hci_driver_vhci_env.forward_cb)
        return -EAGAIN;
    head = ble_msys_get_pkthdr(pkt_len, ESP_HCI_INTERNAL_ACL_MBUF_LEADINGSPCAE);

    if (!head) {

        return -EAGAIN;
    }
    if (ble_mbuf_append(head, data + 1, pkt_len)) {

        s31_ble_acl_release_owned(head, 1);
        return -EAGAIN;
    }
    rc = s_hci_driver_vhci_env.forward_cb(HCI_DRIVER_TYPE_ACL,
             (uint8_t *)head, pkt_len, HCI_DRIVER_DIR_H2C, HCI_DRIVER_LE_ACL);
    if (rc) {

        s31_ble_acl_release_owned(head, 1);
        return -EAGAIN;
    }
    /* Consumer owns the chain now; do not free it or retain a diagnostic pointer. */

    return 0;
}
#endif
