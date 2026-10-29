/* SPDX-License-Identifier: BSD-2-Clause */
/* The bundled os/os_mempool.h references an absent os_error.h. Use the same
 * public BTDM descriptor that ble_mbuf.h supplies to the r_os_* binary ABI. */
#include "btdm_mempool.h"
#include "s31_vhci_pool_wake.inc"
