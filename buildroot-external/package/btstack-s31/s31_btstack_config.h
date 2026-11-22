/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef BTSTACK_CONFIG_H
#define BTSTACK_CONFIG_H

/* Upstream Linux HCI port and manually run Classic/BLE examples. */
#define HAVE_ASSERT
#define HAVE_BTSTACK_STDIN
#define HAVE_MALLOC
#define HAVE_POSIX_TIME
/* The upstream CMake library includes mesh objects; unused sections are dropped. */
#define ENABLE_MESH
#define ENABLE_MESH_ADV_BEARER
#define ENABLE_MESH_GATT_BEARER
#define ENABLE_MESH_PB_ADV
#define ENABLE_MESH_PB_GATT
#define ENABLE_MESH_PROVISIONER
#define ENABLE_MESH_PROXY_SERVER
#define MAX_NR_MESH_SUBNETS 2
#define MAX_NR_MESH_TRANSPORT_KEYS 16
#define MAX_NR_MESH_VIRTUAL_ADDRESSES 16
#define MAX_NR_MESH_NETWORK_KEYS (MAX_NR_MESH_SUBNETS + 1)
#define ENABLE_BLE
#define ENABLE_CLASSIC
#define ENABLE_LE_PERIPHERAL
#define ENABLE_LE_DATA_LENGTH_EXTENSION
#define ENABLE_PRINTF_HEXDUMP
#define ENABLE_LOG_ERROR
#define ENABLE_SCO_OVER_HCI
#define HCI_ACL_PAYLOAD_SIZE (1691 + 4)
#define HCI_INCOMING_PRE_BUFFER_SIZE 14
#define NVM_NUM_LINK_KEYS 4
#define NVM_NUM_DEVICE_DB_ENTRIES 2

#endif
