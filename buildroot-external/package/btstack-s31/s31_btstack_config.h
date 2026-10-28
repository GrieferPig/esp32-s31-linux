/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef BTSTACK_CONFIG_H
#define BTSTACK_CONFIG_H

/* Minimal Linux BR/EDR A2DP sink plus BLE GAP/GATT peripheral. */
#define HAVE_ASSERT
#define HAVE_MALLOC
#define HAVE_POSIX_TIME

#define ENABLE_BLE
#define ENABLE_CLASSIC
#define ENABLE_HCI_CONTROLLER_TO_HOST_FLOW_CONTROL
#define ENABLE_LE_PERIPHERAL
/* Negotiate LE data length with the peer; honor the returned link capability. */
#define ENABLE_LE_DATA_LENGTH_EXTENSION
#define ENABLE_LOG_ERROR

#define HCI_ACL_PAYLOAD_SIZE (1691 + 4)
#define HCI_HOST_ACL_PACKET_LEN 1691
#define HCI_HOST_ACL_PACKET_NUM 4
#define HCI_HOST_SCO_PACKET_LEN 255
#define HCI_HOST_SCO_PACKET_NUM 10
#define HCI_INCOMING_PRE_BUFFER_SIZE 14
#define NVM_NUM_LINK_KEYS 4
#define NVM_NUM_DEVICE_DB_ENTRIES 2

#endif
