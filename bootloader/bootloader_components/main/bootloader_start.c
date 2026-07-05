/*
 * SPDX-FileCopyrightText: 2015-2024 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <inttypes.h>
#include "sdkconfig.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_partition.h"
#include "esp_rom_sys.h"
#include "esp_rom_caps.h"
#include "bootloader_init.h"
#include "bootloader_utility.h"
#include "bootloader_common.h"
#include "bootloader_flash_priv.h"
#include "bootloader_hooks.h"
#include "esp_private/esp_psram_impl.h"
#include "riscv/csr.h"
#include "hal/assist_debug_ll.h"
#include "hal/cache_hal.h"
#include "hal/cache_ll.h"
#include "hal/mmu_hal.h"
#include "hal/wdt_hal.h"
#include "soc/soc.h"
#include "soc/ext_mem_defs.h"
#include "soc/cpu_apm_reg.h"
#include "soc/hp_apm_reg.h"
#include "soc/hp_mem_apm_reg.h"

ESP_LOG_ATTR_TAG(TAG, "boot");

static int select_partition_number(bootloader_state_t *bs);
static int selected_boot_partition(const bootloader_state_t *bs);

#define PSRAM_BASE                    ((uintptr_t)0xC0000000U)
#define OPENSBI_XIP_ADDR              0x40030000U
#define LINUX_XIP_ADDR                0x400B0000U
#define OPENSBI_FDT_OFFSET_SLOT_SIZE  4U
#define FDT_MAGIC_LE                  0xEDFE0DD0U
#define INITRAMFS_LOAD_ADDR           0xC0800000U
#define INITRAMFS_PARTITION_SIZE      0x005E0000U
#define INITRAMFS_NEWC_MAGIC0         0x37303730U /* "0707" */
#define S31_PAYLOAD_SUBTYPE           ((esp_partition_subtype_t)0x40)
#define PMP_FULL_SPACE_NAPOT_ADDR     0x3FFFFFFFU
#define PMP_ENTRY_RWX_LOCK_NAPOT      0x9FU

static size_t s_psram_size;

static void fail_reset(const char *message) __attribute__((noreturn));

static void fail_reset(const char *message)
{
    ESP_LOGE(TAG, "%s", message);
    bootloader_reset();
    while (true) {
    }
}

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
    REG_WRITE(CPU_APM_REGION0_ADDR_START_REG, 0);
    REG_WRITE(CPU_APM_REGION0_ADDR_END_REG, 0xFFFFFFFF);
    REG_WRITE(CPU_APM_REGION0_ATTR_REG, 0x7777);
    REG_WRITE(CPU_APM_REGION_FILTER_EN_REG, 1);
    REG_WRITE(CPU_APM_FUNC_CTRL_REG, 0xFFFFFFFF);

    REG_WRITE(HP_APM_REGION0_ADDR_START_REG, 0);
    REG_WRITE(HP_APM_REGION0_ADDR_END_REG, 0xFFFFFFFF);
    REG_WRITE(HP_APM_REGION0_ATTR_REG, 0x7777);
    REG_WRITE(HP_APM_REGION_FILTER_EN_REG, 1);
    REG_WRITE(HP_APM_FUNC_CTRL_REG, 0xFFFFFFFF);

    REG_WRITE(HP_MEM_APM_REGION0_ADDR_START_REG, 0);
    REG_WRITE(HP_MEM_APM_REGION0_ADDR_END_REG, 0xFFFFFFFF);
    REG_WRITE(HP_MEM_APM_REGION0_ATTR_REG, 0x7777);
    REG_WRITE(HP_MEM_APM_REGION_FILTER_EN_REG, 1);
    REG_WRITE(HP_MEM_APM_FUNC_CTRL_REG, 0xFFFFFFFF);

    for (int i = 0; i < 4; i++) {
        REG_WRITE(DR_REG_FLASH_SPI0_BASE + 0x100 + i * 4, 0x1B);
        REG_WRITE(DR_REG_FLASH_SPI0_BASE + 0x130 + i * 4, 0x1B);
        REG_WRITE(DR_REG_PSRAM_MSPI0_BASE + 0x100 + i * 4, 0x1B);
        REG_WRITE(DR_REG_PSRAM_MSPI0_BASE + 0x130 + i * 4, 0x1B);
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
}

static void disable_watchdogs_and_interrupts(void)
{
    volatile uint32_t *timg0 = (volatile uint32_t *)0x20580000;
    volatile uint32_t *timg1 = (volatile uint32_t *)0x20581000;

    timg0[0x64 / 4] = 0x50D83AA1;
    timg0[0x48 / 4] = 0;
    timg0[0x64 / 4] = 0;

    timg1[0x64 / 4] = 0x50D83AA1;
    timg1[0x48 / 4] = 0;
    timg1[0x64 / 4] = 0;

    wdt_hal_context_t rtc_wdt_ctx;
    wdt_hal_init(&rtc_wdt_ctx, WDT_RWDT, 0, false);
    wdt_hal_write_protect_disable(&rtc_wdt_ctx);
    wdt_hal_disable(&rtc_wdt_ctx);

    for (int i = 0; i < 128; i++) {
        volatile uint8_t *ie = (volatile uint8_t *)(0x10801000 + i * 4 + 1);
        volatile uint8_t *attr = (volatile uint8_t *)(0x10801000 + i * 4 + 2);

        *ie = 0;
        *attr = 0;
    }

    __asm__ volatile ("csrw 0x307, zero");
}

static void clear_mstatus_mprv(void)
{
    RV_CLEAR_CSR(mstatus, MSTATUS_MPRV);
}

static void clear_hw_stack_guard(void)
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

static esp_partition_t find_required_partition(const char *label)
{
    const esp_partition_t *found =
        esp_partition_find_first(ESP_PARTITION_TYPE_DATA, S31_PAYLOAD_SUBTYPE, label);

    if (!found) {
        ESP_LOGE(TAG, "%s partition not found", label);
        bootloader_reset();
    }

    return *found;
}

static void init_psram(void)
{
    esp_err_t err = esp_psram_impl_enable();

    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "PSRAM init failed: 0x%x", err);
        bootloader_reset();
    }

    uint32_t psram_size = 0;
    err = esp_psram_impl_get_available_size(&psram_size);
    if (err != ESP_OK || psram_size == 0) {
        fail_reset("PSRAM size probe failed");
    }

    uint32_t mapped_len = 0;
#if SOC_MMU_PER_EXT_MEM_TARGET
    mmu_hal_map_region(1, MMU_TARGET_PSRAM0, SOC_DRAM_PSRAM_ADDRESS_LOW,
                       0, psram_size, &mapped_len);
#else
    mmu_hal_map_region(0, MMU_TARGET_PSRAM0, SOC_DRAM_PSRAM_ADDRESS_LOW,
                       0, psram_size, &mapped_len);
#endif
    if (mapped_len == 0) {
        fail_reset("PSRAM MMU mapping failed");
    }

    cache_bus_mask_t bus_mask = cache_ll_l1_get_bus(0, SOC_DRAM_PSRAM_ADDRESS_LOW, mapped_len);
    cache_ll_l1_enable_bus(0, bus_mask);
    s_psram_size = mapped_len;

    ESP_LOGI(TAG, "PSRAM initialized: size=0x%08x mapped=0x%08" PRIx32,
             (unsigned)psram_size, mapped_len);
}

static void load_initramfs_partition(const esp_partition_t *initramfs_part)
{
    if (initramfs_part->size != INITRAMFS_PARTITION_SIZE) {
        ESP_LOGE(TAG, "initramfs partition size 0x%08" PRIx32
                      " != expected 0x%08" PRIx32,
                 initramfs_part->size, (uint32_t)INITRAMFS_PARTITION_SIZE);
        bootloader_reset();
    }

    if (INITRAMFS_LOAD_ADDR < PSRAM_BASE ||
        INITRAMFS_LOAD_ADDR + initramfs_part->size > PSRAM_BASE + s_psram_size) {
        ESP_LOGE(TAG, "initramfs load range [0x%08" PRIx32 "..0x%08" PRIx32
                      ") outside PSRAM size 0x%08x",
                 (uint32_t)INITRAMFS_LOAD_ADDR,
                 (uint32_t)(INITRAMFS_LOAD_ADDR + initramfs_part->size),
                 (unsigned)s_psram_size);
        bootloader_reset();
    }

    esp_err_t err = esp_partition_read(initramfs_part, 0,
                                       (void *)INITRAMFS_LOAD_ADDR,
                                       initramfs_part->size);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "reading initramfs partition failed: 0x%x", err);
        bootloader_reset();
    }

    uint32_t magic0 = *(const volatile uint32_t *)INITRAMFS_LOAD_ADDR;
    if (magic0 != INITRAMFS_NEWC_MAGIC0) {
        ESP_LOGE(TAG, "initramfs magic 0x%08" PRIx32
                      " != expected newc prefix 0x%08" PRIx32,
                 magic0, (uint32_t)INITRAMFS_NEWC_MAGIC0);
        bootloader_reset();
    }

    ESP_LOGI(TAG, "initramfs copied: flash=0x%08" PRIx32
                  " size=0x%08" PRIx32 " psram=[0x%08" PRIx32 "..0x%08" PRIx32 ")",
             initramfs_part->address, initramfs_part->size,
             (uint32_t)INITRAMFS_LOAD_ADDR,
             (uint32_t)(INITRAMFS_LOAD_ADDR + initramfs_part->size));
}

static uint32_t read_fdt_addr(const esp_partition_t *opensbi_part)
{
    if (opensbi_part->size < OPENSBI_FDT_OFFSET_SLOT_SIZE) {
        fail_reset("OpenSBI partition too small for FDT offset slot");
    }

    uint32_t fdt_offset = 0;
    esp_err_t err = esp_partition_read(opensbi_part,
                                       opensbi_part->size - OPENSBI_FDT_OFFSET_SLOT_SIZE,
                                       &fdt_offset,
                                       sizeof(fdt_offset));
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "reading FDT offset failed: 0x%x", err);
        bootloader_reset();
    }

    if (fdt_offset >= opensbi_part->size - OPENSBI_FDT_OFFSET_SLOT_SIZE ||
        (fdt_offset & (sizeof(uint32_t) - 1)) != 0) {
        ESP_LOGE(TAG, "invalid FDT offset 0x%08" PRIx32, fdt_offset);
        bootloader_reset();
    }

    uint32_t fdt_magic = 0;
    err = esp_partition_read(opensbi_part, fdt_offset, &fdt_magic, sizeof(fdt_magic));
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "reading FDT magic failed: 0x%x", err);
        bootloader_reset();
    }
    if (fdt_magic != FDT_MAGIC_LE) {
        ESP_LOGE(TAG, "invalid FDT magic 0x%08" PRIx32, fdt_magic);
        bootloader_reset();
    }

    uint32_t fdt = OPENSBI_XIP_ADDR + fdt_offset;
    ESP_LOGI(TAG, "OpenSBI FDT offset 0x%08" PRIx32 ", addr 0x%08" PRIx32,
             fdt_offset, fdt);
    return fdt;
}

static void map_flash_region(uint32_t flash_addr,
                             uint32_t vaddr,
                             uint32_t size,
                             const char *name)
{
    if ((flash_addr & (CONFIG_MMU_PAGE_SIZE - 1)) != 0 ||
        (vaddr & (CONFIG_MMU_PAGE_SIZE - 1)) != 0) {
        ESP_LOGE(TAG, "%s mapping is not MMU-page aligned", name);
        bootloader_reset();
    }

    uint32_t mapped_len = 0;
    mmu_hal_map_region(0, MMU_TARGET_FLASH0, vaddr, flash_addr, size, &mapped_len);
    ESP_LOGI(TAG, "%s mapped: flash=0x%08" PRIx32
                  " vaddr=0x%08" PRIx32 " size=0x%08" PRIx32,
             name, flash_addr, vaddr, mapped_len);
}

static void map_payloads_for_xip(const esp_partition_t *opensbi_part,
                                 const esp_partition_t *linux_part)
{
    if (opensbi_part->address != 0x220000U ||
        opensbi_part->size != (LINUX_XIP_ADDR - OPENSBI_XIP_ADDR)) {
        ESP_LOGE(TAG, "unexpected OpenSBI partition layout: addr=0x%08" PRIx32
                      " size=0x%08" PRIx32,
                 opensbi_part->address, opensbi_part->size);
        bootloader_reset();
    }

    cache_hal_disable(CACHE_LL_LEVEL_EXT_MEM, CACHE_TYPE_ALL);
    map_flash_region(opensbi_part->address, OPENSBI_XIP_ADDR, opensbi_part->size, "OpenSBI");
    map_flash_region(linux_part->address, LINUX_XIP_ADDR, linux_part->size, "Linux");
    cache_ll_invalidate_all(CACHE_LL_LEVEL_ALL, CACHE_TYPE_ALL, CACHE_LL_ID_ALL);
    cache_hal_enable(CACHE_LL_LEVEL_EXT_MEM, CACHE_TYPE_ALL);
    fence_i();
}

static void handoff_to_opensbi(uint32_t entry, uint32_t fdt) __attribute__((noreturn));

static void handoff_to_opensbi(uint32_t entry, uint32_t fdt)
{
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
        "mv    a1, %1\n\t"
        "mret"
        :
        : "r"(entry), "r"(fdt)
        : "a0", "a1", "t0", "t1", "memory");

    __builtin_unreachable();
}

static void boot_opensbi(void) __attribute__((noreturn));

static void boot_opensbi(void)
{
    esp_partition_t opensbi_part = find_required_partition("opensbi");
    esp_partition_t linux_part = find_required_partition("linux");
    esp_partition_t initramfs_part = find_required_partition("initramfs");

    uint32_t fdt = read_fdt_addr(&opensbi_part);

    init_psram();
    load_initramfs_partition(&initramfs_part);

    disable_apm();
    disable_watchdogs_and_interrupts();
    clear_mstatus_mprv();
    install_global_pmp();
    clear_hw_stack_guard();

    map_payloads_for_xip(&opensbi_part, &linux_part);
    flush_l1_cache_before_handoff();
    handoff_to_opensbi(OPENSBI_XIP_ADDR, fdt);
}

// Select the number of boot partition
static int select_partition_number(bootloader_state_t *bs)
{
    // 1. Load partition table
    if (!bootloader_utility_load_partition_table(bs)) {
        ESP_LOGE(TAG, "load partition table error!");
        return INVALID_INDEX;
    }

    // 2. Select the number of boot partition
    return selected_boot_partition(bs);
}

/*
 * Keep ESP-IDF's normal partition selection behavior. Only the final image
 * load is replaced below, so factory/test/OTA selection still follows IDF.
 */
static int selected_boot_partition(const bootloader_state_t *bs)
{
    int boot_index = bootloader_utility_get_selected_boot_partition(bs);
    if (boot_index == INVALID_INDEX) {
        return boot_index; // Unrecoverable failure (not due to corrupt ota data or bad partition contents)
    }
    if (esp_rom_get_reset_reason(0) != RESET_REASON_CORE_DEEP_SLEEP) {
        // Factory firmware.
#ifdef CONFIG_BOOTLOADER_FACTORY_RESET
        bool reset_level = false;
#if CONFIG_BOOTLOADER_FACTORY_RESET_PIN_HIGH
        reset_level = true;
#endif
        if (bootloader_common_check_long_hold_gpio_level(CONFIG_BOOTLOADER_NUM_PIN_FACTORY_RESET, CONFIG_BOOTLOADER_HOLD_TIME_GPIO, reset_level) == GPIO_LONG_HOLD) {
            ESP_LOGI(TAG, "Detect a condition of the factory reset");
            bool ota_data_erase = false;
#ifdef CONFIG_BOOTLOADER_OTA_DATA_ERASE
            ota_data_erase = true;
#endif
            const char *list_erase = CONFIG_BOOTLOADER_DATA_FACTORY_RESET;
            ESP_LOGI(TAG, "Data partitions to erase: %s", list_erase);
            if (bootloader_common_erase_part_type_data(list_erase, ota_data_erase) == false) {
                ESP_LOGE(TAG, "Not all partitions were erased");
            }
#ifdef CONFIG_BOOTLOADER_RESERVE_RTC_MEM
            bootloader_common_set_rtc_retain_mem_factory_reset_state();
#endif
            return bootloader_utility_get_selected_boot_partition(bs);
        }
#endif // CONFIG_BOOTLOADER_FACTORY_RESET
        // TEST firmware.
#ifdef CONFIG_BOOTLOADER_APP_TEST
        bool app_test_level = false;
#if CONFIG_BOOTLOADER_APP_TEST_PIN_HIGH
        app_test_level = true;
#endif
        if (bootloader_common_check_long_hold_gpio_level(CONFIG_BOOTLOADER_NUM_PIN_APP_TEST, CONFIG_BOOTLOADER_HOLD_TIME_GPIO, app_test_level) == GPIO_LONG_HOLD) {
            ESP_LOGI(TAG, "Detect a boot condition of the test firmware");
            if (bs->test.offset != 0) {
                boot_index = TEST_APP_INDEX;
                return boot_index;
            } else {
                ESP_LOGE(TAG, "Test firmware is not found in partition table");
                return INVALID_INDEX;
            }
        }
#endif // CONFIG_BOOTLOADER_APP_TEST
        // Customer implementation.
        // if (gpio_pin_1 == true && ...){
        //     boot_index = required_boot_partition;
        // } ...
    }
    return boot_index;
}

/*
 * We arrive here after the ROM bootloader finished loading this second stage
 * bootloader from flash. The hardware is mostly uninitialized, flash cache is
 * down and the app CPU is in reset. Keep the normal ESP-IDF setup and partition
 * selection, then replace only the final app image load with Linux/OpenSBI.
 */
void __attribute__((noreturn)) call_start_cpu0(void)
{
    // (0. Call the before-init hook, if available)
    if (bootloader_before_init) {
        bootloader_before_init();
    }

    // 1. Hardware initialization
    if (bootloader_init() != ESP_OK) {
        bootloader_reset();
    }

    // (1.1 Call the after-init hook, if available)
    if (bootloader_after_init) {
        bootloader_after_init();
    }

    // 2. Select the number of boot partition
    bootloader_state_t bs = {0};
    int boot_index = select_partition_number(&bs);
    if (boot_index == INVALID_INDEX) {
        bootloader_reset();
    }

    // 2.1 Load the TEE image
#if CONFIG_SECURE_ENABLE_TEE
    bootloader_utility_load_tee_image(&bs);
#endif

    // 3. Replace ESP app image loading with the Linux/OpenSBI handoff.
    ESP_LOGI(TAG, "Booting Linux payload");
    boot_opensbi();
}

#if CONFIG_LIBC_NEWLIB
struct _reent *__getreent(void)
{
    return _GLOBAL_REENT;
}
#endif
