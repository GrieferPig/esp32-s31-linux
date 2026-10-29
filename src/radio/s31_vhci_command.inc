/* SPDX-License-Identifier: BSD-2-Clause */
/* Called only by the existing core0 serialized radio owner. Return EAGAIN
 * without consuming the caller's frame when the command pool is exhausted.
 * In the pinned controller, r_btdm_hci_trans_rx rejects before publishing the
 * command on every nonzero return; the open transport also rejects before
 * dispatch if the controller has stopped. Thus a failed command still owns
 * its allocation and must release it exactly once before a retry.
 * BLE ACL uses its typed chain adapter; Classic ACL/SCO use bounded EMS
 * packet ownership. ISO retains its prior ABI pending a separate audit. */
int s31_vhci_try_send_packet(uint8_t *data, uint16_t length)
{
    hci_driver_packet_t *pkt;
    int rc;

    if (!data || !length)
        return -EINVAL;
    if (esp_bt_controller_get_status() != ESP_BT_CONTROLLER_STATUS_ENABLED)
        return -EAGAIN;
#if UC_BT_CTRL_BLE_IS_ENABLE || UC_BT_CTRL_BR_EDR_IS_ENABLE
    if (data[0] == HCI_DRIVER_TYPE_ACL) {
        uint16_t flags, handle;
        bool is_bredr;
        /* No header access or allocator entry before complete-frame bounds. */
        if (length < 5 || length > 1029 ||
            (uint32_t)length != 5U + data[3] + ((uint32_t)data[4] << 8))
            return -EINVAL;
        flags = data[1] | ((uint16_t)data[2] << 8);
        handle = flags & HCI_INTERNAL_CONN_MASK;
        is_bredr = HCI_INTERNAL_ACL_IS_BREDR_BCAST(flags) ||
                   !HCI_INTERNAL_CONN_IS_BLE(handle);
#if UC_BT_CTRL_BLE_IS_ENABLE
        if (!is_bredr)
            return s31_vhci_ble_acl_send(data, length);
#endif
#if UC_BT_CTRL_BR_EDR_IS_ENABLE
        if (is_bredr) {
            /* The pinned listener selects EMS ACL allocation by handle bit11,
             * including broadcast packets. A broadcast flag alone cannot
             * turn a BLE-domain/zero handle into a Classic allocation. */
            if (HCI_INTERNAL_CONN_IS_BLE(handle) ||
                (!HCI_INTERNAL_ACL_IS_BREDR_BCAST(flags) &&
                 !HCI_INTERNAL_CONN_IS_BREDR_ACL(handle)))
                return -EINVAL;
            return s31_vhci_bredr_send(data, length, handle,
                                       HCI_DRIVER_TYPE_ACL);
        }
#endif
        return -EOPNOTSUPP;
    }
#endif
#if UC_BT_CTRL_BR_EDR_IS_ENABLE
    if (data[0] == HCI_DRIVER_TYPE_SYNC) {
        uint16_t handle;
        if (length < 4 || length != (uint16_t)(4 + data[3]))
            return -EINVAL;
        handle = (data[1] | ((uint16_t)data[2] << 8)) & HCI_INTERNAL_CONN_MASK;
        if (!HCI_INTERNAL_CONN_IS_BREDR_SYNC(handle))
            return -EINVAL;
        return s31_vhci_bredr_send(data, length, handle,
                                   HCI_DRIVER_TYPE_SYNC);
    }
#endif
    if (data[0] != HCI_DRIVER_TYPE_CMD) {
        esp_vhci_host_send_packet(data, length);
        return 0;
    }
    /* H4 type + opcode + parameter length. Read no header before bounds. */
    if (length < 4 || length != (uint16_t)(4 + data[3]))
        return -EINVAL;
    if (!s_hci_driver_vhci_env.forward_cb)
        return -EAGAIN;
    pkt = btdm_hci_trans_buf_alloc(HCI_DRIVER_TYPE_CMD, 0);
    if (!pkt)
        return -EAGAIN;
    pkt->length = length - 1;
    memcpy(pkt->data, data + 1, pkt->length);
    rc = s_hci_driver_vhci_env.forward_cb(HCI_DRIVER_TYPE_CMD,
             (uint8_t *)pkt, pkt->length, HCI_DRIVER_DIR_H2C, 0xff);
    if (rc) {
        btdm_hci_trans_buf_free(pkt);
        return -EAGAIN;
    }
    return 0;
}
