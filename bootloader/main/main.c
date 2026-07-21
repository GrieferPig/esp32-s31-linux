/*
 * ESP32-S31 MMU Linux - Factory App Loader
 *
 * The second-stage bootloader stays minimal. This factory app runs with the
 * normal app startup path so ESP-IDF initializes PSRAM, maps OpenSBI and Linux
 * for flash XIP, maps the initramfs flash window, then jumps into OpenSBI in
 * M-mode.
 */

#include <stdbool.h>
#include <stdint.h>
#include <inttypes.h>
#include "sdkconfig.h"
#include "esp_log.h"
#include "esp_err.h"
#include "esp_system.h"
#include "esp_chip_info.h"
#include "esp_cpu.h"
#include "esp_flash.h"
#include "esp_partition.h"
#include "esp_cache.h"
#include "spi_flash_mmap.h"
#include "esp_psram.h"
#include "riscv/csr.h"
#include "esp32s31/rom/cache.h"
#include "hal/cache_ll.h"
#include "hal/assist_debug_ll.h"
#include "hal/wdt_hal.h"
#include "soc/soc.h"
#include "soc/timer_group_struct.h"
#include "soc/usb_serial_jtag_reg.h"
#include "soc/usb_serial_jtag_struct.h"
#include "soc/cpu_apm_reg.h"
#include "soc/hp_apm_reg.h"
#include "soc/hp_mem_apm_reg.h"
#include "hal/mmu_ll.h"
#include "hal/mmu_types.h"
#include "freertos/FreeRTOS.h"

#define OPENSBI_XIP_ADDR              0x40030000U
#define LINUX_XIP_ADDR                0x400B0000U
#define INITRAMFS_FLASH_ADDR          0x40A20000U
#define OPENSBI_FDT_OFFSET_SLOT_SIZE  4U
#define FDT_MAGIC_LE                  0xEDFE0DD0U
#define INITRAMFS_PARTITION_SIZE      0x005E0000U
#define ESP32S31_PSRAM_SIZE           0x01000000U
#define PMP_FULL_SPACE_NAPOT_ADDR     0x3FFFFFFFU
#define PMP_ENTRY_RWX_LOCK_NAPOT      0x9FU

static const char *TAG = "boot";
static void fence_i(void)
{
    __asm__ volatile ("fence.i" ::: "memory");
}

static void flush_l1_cache_before_handoff(void)
{
    cache_ll_writeback_all(CACHE_LL_LEVEL_ALL, CACHE_TYPE_DATA, CACHE_LL_ID_ALL);
    cache_ll_invalidate_all(CACHE_LL_LEVEL_ALL, CACHE_TYPE_ALL, CACHE_LL_ID_ALL);
    fence_i();
}

static void disable_apm(void)
{
    // Instead of disabling, we must ENABLE APM with permissive regions for S-mode.
    
    // CPU APM
    REG_WRITE(CPU_APM_REGION0_ADDR_START_REG, 0);
    REG_WRITE(CPU_APM_REGION0_ADDR_END_REG, 0xFFFFFFFF);
    REG_WRITE(CPU_APM_REGION0_ATTR_REG, 0x7777);
    REG_WRITE(CPU_APM_REGION_FILTER_EN_REG, 1);
    REG_WRITE(CPU_APM_FUNC_CTRL_REG, 0xFFFFFFFF);

    // HP APM
    REG_WRITE(HP_APM_REGION0_ADDR_START_REG, 0);
    REG_WRITE(HP_APM_REGION0_ADDR_END_REG, 0xFFFFFFFF);
    REG_WRITE(HP_APM_REGION0_ATTR_REG, 0x7777);
    REG_WRITE(HP_APM_REGION_FILTER_EN_REG, 1);
    REG_WRITE(HP_APM_FUNC_CTRL_REG, 0xFFFFFFFF);

    // HP MEM APM
    REG_WRITE(HP_MEM_APM_REGION0_ADDR_START_REG, 0);
    REG_WRITE(HP_MEM_APM_REGION0_ADDR_END_REG, 0xFFFFFFFF);
    REG_WRITE(HP_MEM_APM_REGION0_ATTR_REG, 0x7777);
    REG_WRITE(HP_MEM_APM_REGION_FILTER_EN_REG, 1);
    REG_WRITE(HP_MEM_APM_FUNC_CTRL_REG, 0xFFFFFFFF);

    // MSPI PMS Bypass for S-mode PSRAM/Flash Access
    // Grant RD/WR/NONSECURE_RD/NONSECURE_WR (0x1B) to all regions
    for (int i = 0; i < 4; i++) {
        REG_WRITE(DR_REG_FLASH_SPI0_BASE + 0x100 + i*4, 0x1B); // Flash C PMS
        REG_WRITE(DR_REG_FLASH_SPI0_BASE + 0x130 + i*4, 0x1B); // PSRAM C PMS
        REG_WRITE(DR_REG_PSRAM_MSPI0_BASE + 0x100 + i*4, 0x1B); // Flash S PMS
        REG_WRITE(DR_REG_PSRAM_MSPI0_BASE + 0x130 + i*4, 0x1B); // PSRAM S PMS
    }

    REG_WRITE(HP_APM_M0_STATUS_CLR_REG, 1);
    REG_WRITE(HP_APM_M1_STATUS_CLR_REG, 1);
    REG_WRITE(HP_APM_M2_STATUS_CLR_REG, 1);
    REG_WRITE(HP_APM_M3_STATUS_CLR_REG, 1);
    REG_WRITE(HP_APM_M4_STATUS_CLR_REG, 1);
    REG_WRITE(HP_APM_M5_STATUS_CLR_REG, 1);
    REG_WRITE(HP_APM_M6_STATUS_CLR_REG, 1);

    REG_WRITE(HP_MEM_APM_M0_STATUS_CLR_REG, 1);
    REG_WRITE(HP_MEM_APM_M1_STATUS_CLR_REG, 1);
    REG_WRITE(HP_MEM_APM_M2_STATUS_CLR_REG, 1);
    REG_WRITE(HP_MEM_APM_M3_STATUS_CLR_REG, 1);
    REG_WRITE(HP_MEM_APM_M4_STATUS_CLR_REG, 1);
    REG_WRITE(HP_MEM_APM_M5_STATUS_CLR_REG, 1);

    REG_WRITE(CPU_APM_M0_STATUS_CLR_REG, 1);
    REG_WRITE(CPU_APM_M1_STATUS_CLR_REG, 1);
    REG_WRITE(CPU_APM_M2_STATUS_CLR_REG, 1);
    REG_WRITE(CPU_APM_M3_STATUS_CLR_REG, 1);
}

static void install_global_pmp(void)
{
    RV_WRITE_CSR(pmpaddr1, 0);
    RV_WRITE_CSR(pmpaddr2, 0);
    RV_WRITE_CSR(pmpaddr3, 0);
    RV_WRITE_CSR(pmpaddr4, 0);
    RV_WRITE_CSR(pmpaddr5, 0);
    RV_WRITE_CSR(pmpaddr6, 0);
    RV_WRITE_CSR(pmpaddr7, 0);
    RV_WRITE_CSR(pmpaddr8, 0);
    RV_WRITE_CSR(pmpaddr9, 0);
    RV_WRITE_CSR(pmpaddr10, 0);
    RV_WRITE_CSR(pmpaddr11, 0);
    RV_WRITE_CSR(pmpaddr12, 0);
    RV_WRITE_CSR(pmpaddr13, 0);
    RV_WRITE_CSR(pmpaddr14, 0);
    RV_WRITE_CSR(pmpaddr15, 0);

    RV_WRITE_CSR(pmpcfg1, 0);
    RV_WRITE_CSR(pmpcfg2, 0);
    RV_WRITE_CSR(pmpcfg3, 0);
    RV_WRITE_CSR(pmpaddr0, PMP_FULL_SPACE_NAPOT_ADDR);
    RV_WRITE_CSR(pmpcfg0, PMP_ENTRY_RWX_LOCK_NAPOT);

    uint32_t cfg0 = RV_READ_CSR(pmpcfg0);
    uint32_t cfg1 = RV_READ_CSR(pmpcfg1);
    uint32_t cfg2 = RV_READ_CSR(pmpcfg2);
    uint32_t cfg3 = RV_READ_CSR(pmpcfg3);
    ESP_LOGI(TAG, "PMP install: cfg0=0x%08" PRIx32 " cfg1=0x%08" PRIx32
                  " cfg2=0x%08" PRIx32 " cfg3=0x%08" PRIx32
                  " pmpaddr0=0x%08" PRIx32,
             cfg0, cfg1, cfg2, cfg3, (uint32_t)RV_READ_CSR(pmpaddr0));
}

static void disable_watchdogs(void)
{
    /* Disable TIMG0 and TIMG1 watchdogs so OpenSBI isn't reset.
     * ESP32-S31: TIMG0 @ 0x20580000, TIMG1 @ 0x20581000 */
    volatile uint32_t *timg0 = (volatile uint32_t *)0x20580000;
    volatile uint32_t *timg1 = (volatile uint32_t *)0x20581000;

    /* TIMG_WDTWPROTECT = 0x50D83AA1 then TIMG_WDTCONFIG0 = 0 */
    timg0[0x64/4] = 0x50D83AA1;
    timg0[0x48/4] = 0;
    timg0[0x64/4] = 0;

    timg1[0x64/4] = 0x50D83AA1;
    timg1[0x48/4] = 0;
    timg1[0x64/4] = 0;

    wdt_hal_context_t rtc_wdt_ctx;
    wdt_hal_init(&rtc_wdt_ctx, WDT_RWDT, 0, false);
    wdt_hal_write_protect_disable(&rtc_wdt_ctx);
    wdt_hal_disable(&rtc_wdt_ctx);

    // Disable all CLIC interrupts
    for (int i = 0; i < 128; i++) {
        volatile uint8_t *ie = (volatile uint8_t *)(0x10801000 + i*4 + 1);
        *ie = 0;
        volatile uint8_t *attr = (volatile uint8_t *)(0x10801000 + i*4 + 2);
        *attr = 0;
    }

    asm volatile ("csrw 0x307, zero"); // Clear mtvt
}

static void clear_mstatus_mprv(void)
{
    RV_CLEAR_CSR(mstatus, MSTATUS_MPRV);
}

static bool map_flash_range(uint32_t vaddr, uint32_t paddr, uint32_t size)
{
    if (((vaddr | paddr | size) & (SOC_MMU_PAGE_SIZE - 1)) != 0) {
        ESP_LOGE(TAG, "unaligned flash MMU range: vaddr=0x%08" PRIx32
                      " paddr=0x%08" PRIx32 " size=0x%08" PRIx32,
                 vaddr, paddr, size);
        return false;
    }

    for (uint32_t offset = 0; offset < size; offset += SOC_MMU_PAGE_SIZE) {
        uint32_t entry_id = mmu_ll_get_entry_id(MMU_LL_FLASH_MMU_ID,
                                                vaddr + offset);
        mmu_ll_write_entry(MMU_LL_FLASH_MMU_ID, entry_id,
                           (paddr + offset) / SOC_MMU_PAGE_SIZE,
                           MMU_TARGET_FLASH0);
    }

    cache_bus_mask_t bus = cache_ll_l1_get_bus(0, vaddr, size);
    cache_ll_l1_enable_bus(0, bus);
    return true;
}

static void disable_stack_protector(void)
{
    assist_debug_ll_sp_spill_monitor_disable(0);
    assist_debug_ll_sp_spill_monitor_disable(1);
    assist_debug_ll_sp_spill_interrupt_disable(0);
    assist_debug_ll_sp_spill_interrupt_disable(1);
    assist_debug_ll_sp_spill_set_min(0, 0);
    assist_debug_ll_sp_spill_set_max(0, 0xffffffff);
    assist_debug_ll_sp_spill_set_min(1, 0);
    assist_debug_ll_sp_spill_set_max(1, 0xffffffff);
}

void app_main(void)
{
    if (!esp_psram_is_initialized() ||
        esp_psram_get_size() < ESP32S31_PSRAM_SIZE) {
        ESP_LOGE(TAG, "16 MiB PSRAM was not initialized");
        esp_restart();
    }

    disable_apm();
    disable_watchdogs();
    clear_mstatus_mprv();
    install_global_pmp();

    /* Map OpenSBI at its linked Flash XIP address. */
    const esp_partition_t *part = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, 0x40, "opensbi");
    if (!part) {
        ESP_LOGE(TAG, "OpenSBI partition not found");
        esp_restart();
    }

    const void *opensbi_ptr;
    esp_partition_mmap_handle_t mmap_handle;
    esp_err_t err = esp_partition_mmap(part, 0, part->size,
                                       ESP_PARTITION_MMAP_INST,
                                       &opensbi_ptr, &mmap_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_partition_mmap failed: %s", esp_err_to_name(err));
        esp_restart();
    }
    ESP_LOGI(TAG, "OpenSBI mmap at %p (size %" PRIu32 ")",
             opensbi_ptr, part->size);

    if ((uintptr_t)opensbi_ptr != OPENSBI_XIP_ADDR) {
        ESP_LOGE(TAG, "OpenSBI mmap address %p != linked address 0x%08" PRIx32,
                 opensbi_ptr, (uint32_t)OPENSBI_XIP_ADDR);
        esp_restart();
    }

    if (part->size < OPENSBI_FDT_OFFSET_SLOT_SIZE) {
        ESP_LOGE(TAG, "OpenSBI partition too small for FDT offset slot");
        esp_restart();
    }

    uint32_t fdt_offset = 0;
    err = esp_flash_read(part->flash_chip, &fdt_offset,
                         part->address + part->size - OPENSBI_FDT_OFFSET_SLOT_SIZE,
                         sizeof(fdt_offset));
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "reading FDT offset failed: %s", esp_err_to_name(err));
        esp_restart();
    }

    if (fdt_offset >= part->size - OPENSBI_FDT_OFFSET_SLOT_SIZE ||
        (fdt_offset & (sizeof(uint32_t) - 1)) != 0) {
        ESP_LOGE(TAG, "invalid FDT offset 0x%08" PRIx32, fdt_offset);
        esp_restart();
    }

    uint32_t fdt = (uint32_t)(uintptr_t)opensbi_ptr + fdt_offset;
    uint32_t fdt_magic = *(const volatile uint32_t *)(uintptr_t)fdt;
    if (fdt_magic != FDT_MAGIC_LE) {
        ESP_LOGE(TAG, "invalid FDT magic 0x%08" PRIx32 " at 0x%08" PRIx32,
                 fdt_magic, fdt);
        esp_restart();
    }

    ESP_LOGI(TAG, "OpenSBI FDT offset 0x%08" PRIx32 ", addr 0x%08" PRIx32,
             fdt_offset, fdt);

    /* --- Find and mmap Linux partition --- */
    const esp_partition_t *linux_part = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, 0x40, "linux");
    if (!linux_part) {
        ESP_LOGE(TAG, "Linux partition not found");
        esp_restart();
    }
    const void *linux_ptr;
    esp_partition_mmap_handle_t linux_mmap_handle;
    err = esp_partition_mmap(linux_part, 0, linux_part->size,
                             ESP_PARTITION_MMAP_INST,
                             &linux_ptr, &linux_mmap_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_partition_mmap linux failed: %s", esp_err_to_name(err));
        esp_restart();
    }
    ESP_LOGI(TAG, "Linux mmap INST at %p (size %" PRIu32 ")", linux_ptr, linux_part->size);

    if ((uintptr_t)linux_ptr != LINUX_XIP_ADDR) {
        ESP_LOGE(TAG, "Linux mmap address %p != expected 0x%08" PRIx32,
                 linux_ptr, (uint32_t)LINUX_XIP_ADDR);
        esp_restart();
    }

    const esp_partition_t *initramfs_part = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, 0x40, "initramfs");
    if (!initramfs_part ||
        initramfs_part->size != INITRAMFS_PARTITION_SIZE ||
        !map_flash_range(INITRAMFS_FLASH_ADDR, initramfs_part->address,
                         initramfs_part->size)) {
        ESP_LOGE(TAG, "failed to map 0x%08" PRIx32 "-byte initramfs",
                 (uint32_t)INITRAMFS_PARTITION_SIZE);
        esp_restart();
    }

    ESP_LOGI(TAG, "initramfs mapped: flash=0x%08" PRIx32
                  " size=0x%08" PRIx32 " vaddr=0x%08" PRIx32,
             initramfs_part->address, initramfs_part->size,
             (uint32_t)INITRAMFS_FLASH_ADDR);

    uint32_t entry = (uint32_t)(uintptr_t)opensbi_ptr;

    disable_stack_protector();
    flush_l1_cache_before_handoff();

    __asm__ volatile (
        "csrw  mepc, %0\n\t"
        "csrw  mie, zero\n\t"
        "csrr  t0, mstatus\n\t"
        "li    t1, 0x21888\n\t"
        "not   t1, t1\n\t"
        "and   t0, t0, t1\n\t"
        "li    t1, 0x1800\n\t"
        "or    t0, t0, t1\n\t"
        "csrw  mstatus, t0\n\t"
        "mv    a0, zero\n\t"
        "mv    a1, %2\n\t"
        "mret"
        : : "r"(entry), "r"(0), "r"(fdt) : "a0","a1","t0","t1","memory");
    __builtin_unreachable();
}
