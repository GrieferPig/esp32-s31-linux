/* SPDX-License-Identifier: BSD-2-Clause */
#if UC_BT_CTRL_BR_EDR_IS_ENABLE
/* Pinned controller ABI: this global accessor copies eight bytes starting at
 * pool+20; the final little-endian u16 is the element size used by EMS calloc.
 * Actual-ELF tests pin that contract. It avoids calling the private pool getter
 * or assuming that the controller's advertised payload size is allocation size.
 * Both pools exist throughout an enabled BR/EDR controller lifetime. */
void r_orca_ems_get_pool_info(uint8_t pool_id, void *info);
void r_orca_ems_tx_acl_free(void *pkt);
void r_orca_ems_tx_sync_free(void *pkt);
static void s31_vhci_bredr_release_owned(hci_driver_packet_t *pkt, uint8_t type)
{
    /* Generic btdm_hci_trans_buf_free only returns its command/LE objects.
     * The Classic listener has no 0x80000001 free event: returning an EMS
     * packet there asserts. Pair each TX allocator with its actual EMS free. */
    if (type == HCI_DRIVER_TYPE_ACL)
        r_orca_ems_tx_acl_free(pkt);
    else
        r_orca_ems_tx_sync_free(pkt);
}

static int s31_vhci_bredr_send(uint8_t *data, uint16_t length,
                             uint16_t handle, uint8_t type)
{
    uint8_t info[8] __attribute__((aligned(4)));
    hci_driver_packet_t *pkt;
    uintptr_t offset;
    uint16_t element_size, pkt_len = length - 1;
    uint8_t pool_id = type == HCI_DRIVER_TYPE_ACL ? 9 : 11;
    uint8_t source = type == HCI_DRIVER_TYPE_ACL ?
                     HCI_DRIVER_BREDR_ACL : HCI_DRIVER_BREDR_SYNC;
    int rc;

    if (!s_hci_driver_vhci_env.forward_cb)
        return -EAGAIN;
    pkt = btdm_hci_trans_buf_alloc(type, handle);
    if (!pkt)
        return -EAGAIN;
    r_orca_ems_get_pool_info(pool_id, info);
    element_size = info[6] | ((uint16_t)info[7] << 8);
    offset = (uintptr_t)pkt->data - (uintptr_t)pkt;
    if (!pkt->data || element_size < sizeof(*pkt) ||
        offset < sizeof(*pkt) || offset > element_size) {
        s31_vhci_bredr_release_owned(pkt, type);
        return -EIO;
    }
    if (pkt_len > element_size - offset) {
        s31_vhci_bredr_release_owned(pkt, type);
        return -EMSGSIZE;
    }
    memcpy(pkt->data, data + 1, pkt_len);
    pkt->length = pkt_len;
    rc = s_hci_driver_vhci_env.forward_cb(type, (uint8_t *)pkt, pkt_len,
                                        HCI_DRIVER_DIR_H2C, source);
    if (rc) {
        /* The open forwarder rejects before dispatch if disabled. The pinned
         * Classic ACL/SYNC consumers always transfer ownership and return 0.
         * Use the matching EMS TX release, never an RX-object release. */
        s31_vhci_bredr_release_owned(pkt, type);
        return -EAGAIN;
    }
    return 0;
}
#endif
