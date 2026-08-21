/* SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0 */
#ifndef S31_MEMORY_LAYOUT_H
#define S31_MEMORY_LAYOUT_H

/* Cached 16-MiB PSRAM alias shared by both HP harts. */
#define S31_PSRAM_BASE                 0x50000000U
#define S31_PSRAM_SIZE                 0x01000000U

/* OpenSBI owns only the final 64-KiB PSRAM MMU page. */
#define S31_OPENSBI_RW_BASE            0x50FF0000U
#define S31_OPENSBI_RW_SIZE            0x00010000U

/* Linux S-mode owns this complete Wi-Fi/BT area.  Blob allocations use the
 * heap portion; the tail is a synchronous-exception stack and guard area.
 *
 * NOTE: the blob data/BSS must stay at 0x2f030000.  Moving it down to
 * 0x2f018000 (to reclaim ~96 KiB of the parked factory-app heap as one
 * contiguous 242 KiB heap) looked safe in theory but hard-stalls the Wi-Fi
 * download after a few hundred KiB: the blob ends up spinning in the
 * compat-layer queue/sync path (s31_queue_send_locked / xQueueReceive /
 * s31_linux_blob_suspend) with no further RX/TX frames.  The 147 KiB heap
 * (0x2f04cc40..0x2f071800) reliably sustains the full download, so keep the
 * factory-app reservation intact at 0x2f00ea30..0x2f030000. */
#define S31_RADIO_HEAP_BASE            0x2F030000U
#define S31_RADIO_HEAP_END             0x2F071800U
#define S31_RADIO_HEAP_SIZE            (S31_RADIO_HEAP_END - S31_RADIO_HEAP_BASE)
#define S31_RADIO_EXC_BASE             S31_RADIO_HEAP_END
#define S31_RADIO_EXC_END              0x2F072380U

/* Reclaimed parked factory-app FreeRTOS heap as a second (low) blob heap
 * chunk.  The factory app keeps 0x2f00ea30..0x2f018000 (~37 KiB) for its
 * boot-time flash-mapping/PSRAM setup; everything above is idle once hart0
 * parks in WFI.  This region is explicitly reserved and cleared by the
 * loader before Linux starts (see clear_sram_for_linux()), unlike the earlier
 * low-chunk experiment which left stale FreeRTOS heap data in the D-cache. */
#define S31_RADIO_HEAP_LOW_BASE        0x2F018000U
#define S31_RADIO_HEAP_LOW_END         0x2F030000U
#define S31_RADIO_HEAP_LOW_SIZE        (S31_RADIO_HEAP_LOW_END - S31_RADIO_HEAP_LOW_BASE)

/* Linux DMA/status reservations immediately follow the radio world. */
#define S31_AXI_DESC_BASE              0x2F072380U
#define S31_AXI_DESC_SIZE              0x00003000U
#define S31_AHB_DESC_BASE              0x2F075380U
#define S31_AHB_DESC_SIZE              0x00001000U
#define S31_USB_LOCAL_BASE             0x2F076380U
#define S31_USB_LOCAL_SIZE             0x00000040U
#define S31_HART1_MAILBOX_BASE         0x2F0763A0U
#define S31_UART_DMA_BASE              0x2F076400U
#define S31_UART_DMA_SIZE              0x00002800U
#define S31_HP_SHARED_END              0x2F078C00U
#define S31_LINUX_DMA_END              S31_HP_SHARED_END

/* Idle SRAM above the Linux DMA reservations, up to the ROM stack start
 * (SOC_ROM_STACK_START = 0x2f07cfb0).  This covers the tail of the ROM's
 * UART/USB/SPI download-mode buffers (0x2f078c00..0x2f07afb0) plus the PRO
 * CPU startup stack (0x2f07afb0..0x2f07cfb0), both documented by the IDF
 * bootloader memory map as reclaimable after RTOS startup / in normal boot.
 * Added as a second (non-contiguous) gen_pool chunk AFTER the Linux rings
 * are allocated from the main chunk, so s31_radio_sram_linux_alias() is
 * never asked to translate a pointer into this chunk. */
#define S31_RADIO_HEAP2_BASE           0x2F078C00U
#define S31_RADIO_HEAP2_END            0x2F07CFB0U
#define S31_RADIO_HEAP2_SIZE           (S31_RADIO_HEAP2_END - S31_RADIO_HEAP2_BASE)

#if S31_RADIO_EXC_END != S31_AXI_DESC_BASE
#error "radio exception area and AXI descriptors must be contiguous"
#endif

#if S31_AXI_DESC_BASE + S31_AXI_DESC_SIZE != S31_AHB_DESC_BASE
#error "AXI and AHB descriptor regions must be contiguous"
#endif

#if S31_AHB_DESC_BASE + S31_AHB_DESC_SIZE != S31_USB_LOCAL_BASE
#error "AHB descriptors must end at USB local SRAM"
#endif

#if S31_UART_DMA_BASE + S31_UART_DMA_SIZE != S31_HP_SHARED_END
#error "UART DMA region must end at the shared reservation boundary"
#endif

#if S31_OPENSBI_RW_BASE + S31_OPENSBI_RW_SIZE != S31_PSRAM_BASE + S31_PSRAM_SIZE
#error "OpenSBI must occupy the final PSRAM MMU page"
#endif

#endif /* S31_MEMORY_LAYOUT_H */
