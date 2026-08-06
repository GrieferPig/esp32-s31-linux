/*
 * ESP32-S31 MMU Linux - Factory App Loader
 *
 * The second-stage bootloader stays minimal. This factory app runs with the
 * normal app startup path so ESP-IDF initializes PSRAM, maps OpenSBI and Linux
 * for flash XIP, maps the rootfs flash window, then jumps into OpenSBI in
 * M-mode.
 */

#include <stdbool.h>
#include <stdint.h>
#include <inttypes.h>
#include "sdkconfig.h"
#include "esp_log.h"
#include "esp_err.h"
#include "esp_system.h"
#include "esp_cpu.h"
#include "esp_flash.h"
#include "esp_partition.h"
#include "spi_flash_mmap.h"
#include "esp_psram.h"
#include "esp_rom_caps.h"
#include "esp_rom_serial_output.h"
#include "esp_rom_sys.h"
#include "esp32s31/rom/ets_sys.h"
#include "riscv/csr.h"
#include "esp32s31/rom/cache.h"
#include "hal/cache_ll.h"
#include "hal/cpu_utility_ll.h"
#include "hal/assist_debug_ll.h"
#include "hal/wdt_hal.h"
#include "heap_memory_layout.h"
#include "esp_heap_caps.h"
#include "nvs_flash.h"
#include "esp_hosted_coprocessor.h"
#include "slave_bt.h"
#include "driver/uart.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "soc/soc.h"
#include "soc/timer_group_struct.h"
#include "soc/cpu_apm_reg.h"
#include "soc/hp_apm_reg.h"
#include "soc/hp_mem_apm_reg.h"
#include "hal/mmu_ll.h"
#include "hal/mmu_types.h"
#include "hosted_sram.h"
#include "s31_hosted_sram.h"

#define OPENSBI_XIP_ADDR              0x40030000U
#define LINUX_XIP_ADDR                0x400B0000U
#define ROOTFS_FLASH_ADDR             0x40A20000U
#define FLASH_MTD_XIP_ADDR            0x41000000U
#define FLASH_MTD_SIZE                0x01000000U
#define OPENSBI_FDT_OFFSET_SLOT_SIZE  4U
#define FDT_MAGIC_LE                  0xEDFE0DD0U
#define ROOTFS_PARTITION_SIZE         0x005DC000U
#define ROOTFS_MMU_MAP_SIZE           0x005E0000U
#define ESP32S31_PSRAM_SIZE           0x01000000U
#define LINUX_PSRAM_START             0x50000000U
/*
 * Place the shared ring immediately below the hardware-owned DMA/status
 * buffers. This leaves one large primary HP SRAM region for FreeRTOS; the
 * separate ROM-tail region remains available through the IDF heap layout.
 */
#define LINUX_SRAM_START              S31_HOSTED_SRAM_BASE
#define LINUX_SRAM_RING_END           (S31_HOSTED_SRAM_BASE + S31_HOSTED_SRAM_SIZE)
#define LINUX_DMA_START               LINUX_SRAM_RING_END
#define LINUX_DMA_END                 0x2F07AFC0U
#define LINUX_SRAM_END                0x2F07AFC0U
#define HART1_EARLY_MAILBOX_ADDR      0x2F07AFA0U
SOC_RESERVE_MEMORY_REGION(LINUX_SRAM_START, LINUX_SRAM_RING_END, hosted_ring);
SOC_RESERVE_MEMORY_REGION(LINUX_DMA_START, LINUX_DMA_END, linux_devices);

static const char *TAG = "boot";
volatile uint32_t g_core1_fdt;

volatile uint32_t g_core1_trampoline_entered;
volatile uint32_t g_core1_trap_mcause;
volatile uint32_t g_core1_trap_mtval;
volatile uint32_t g_core1_trap_mepc;

extern void core1_linux_trampoline(void);

/* Keep all loader output on the dedicated USB Serial/JTAG peripheral. */
static __attribute__((noreturn)) void loader_restart(const char *reason)
{
    static const char prefix[] = "S31 loader failure: ";

    esp_rom_output_set_as_console(ESP_ROM_USB_SERIAL_DEVICE_NUM);
    for (const char *p = prefix; *p; p++)
        esp_rom_output_tx_one_char((uint8_t)*p);
    for (const char *p = reason; *p; p++)
        esp_rom_output_tx_one_char((uint8_t)*p);
    esp_rom_output_tx_one_char('\r');
    esp_rom_output_tx_one_char('\n');
    esp_rom_output_tx_wait_idle(ESP_ROM_USB_SERIAL_DEVICE_NUM);
    esp_restart();
}

static void fence_i(void)
{
    __asm__ volatile ("fence.i" ::: "memory");
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
    cache_ll_l1_enable_bus(1, bus);
    return true;
}

static bool prepare_core1_cached_psram(void)
{
    if (mmu_ll_get_page_size(MMU_LL_PSRAM_MMU_ID) != MMU_PAGE_64KB) {
        ESP_LOGE(TAG, "unexpected PSRAM MMU page size");
        return false;
    }

    /*
     * The PSRAM MMU and D-cache are shared by both HP harts. IDF maps PSRAM
     * only for its single-core owner, so make the Linux ownership contract
     * explicit before releasing hart1.
     */
    for (uint32_t offset = 0; offset < ESP32S31_PSRAM_SIZE;
         offset += SOC_MMU_PAGE_SIZE) {
        uint32_t entry_id = mmu_ll_get_entry_id(MMU_LL_PSRAM_MMU_ID,
                                                LINUX_PSRAM_START + offset);
        mmu_ll_write_entry(MMU_LL_PSRAM_MMU_ID, entry_id,
                           offset / SOC_MMU_PAGE_SIZE, MMU_TARGET_PSRAM0);
    }

    /*
     * FreeRTOS never allocates PSRAM. Flush any lines left by IDF's PSRAM
     * initialization, then discard them before Linux becomes the sole owner.
     */
    if (Cache_WriteBack_Invalidate_Addr(CACHE_MAP_L1_DCACHE,
                                        LINUX_PSRAM_START,
                                        ESP32S31_PSRAM_SIZE) != 0) {
        ESP_LOGE(TAG, "PSRAM D-cache handoff failed");
        return false;
    }
    cache_ll_l1_invalidate_icache_addr(1, LINUX_PSRAM_START,
                                      ESP32S31_PSRAM_SIZE);

    /*
     * S31 has a shared D-cache and private I-caches. Clear the PSRAM I-bus
     * shutdown bit explicitly for hart1; DBUS0 must remain enabled globally.
     */
    REG_CLR_BIT(CACHE_L1_ICACHE_CTRL_REG, CACHE_L1_ICACHE_SHUT_IBUS1);
    REG_CLR_BIT(CACHE_L1_DCACHE_CTRL_REG, CACHE_L1_DCACHE_SHUT_DBUS0);
    fence_i();
    return true;
}

static void enable_core1_external_memory_bus(uint32_t vaddr, uint32_t size)
{
    cache_bus_mask_t bus = cache_ll_l1_get_bus(1, vaddr, size);

    cache_ll_l1_enable_bus(1, bus);
}

static void disable_core1_stack_protector(void)
{
    assist_debug_ll_sp_spill_monitor_disable(1);
    assist_debug_ll_sp_spill_interrupt_disable(1);
    assist_debug_ll_sp_spill_set_min(1, 0);
    assist_debug_ll_sp_spill_set_max(1, 0xffffffff);
}

static void start_linux_on_core1(uint32_t fdt)
{
    g_core1_fdt = fdt;
    g_core1_trampoline_entered = 0;
    for (uint32_t addr = HART1_EARLY_MAILBOX_ADDR;
         addr < LINUX_SRAM_END; addr += sizeof(uint32_t)) {
        *(volatile uint32_t *)addr = 0;
    }
    __asm__ volatile ("fence rw, rw" ::: "memory");

    /*
     * Flash MMU entries are shared, but each core has a private I-cache bus.
     * The D-cache/PSRAM data bus is shared.
     */
    enable_core1_external_memory_bus(OPENSBI_XIP_ADDR, 0x00080000U);
    enable_core1_external_memory_bus(LINUX_XIP_ADDR, ROOTFS_FLASH_ADDR - LINUX_XIP_ADDR);
    enable_core1_external_memory_bus(ROOTFS_FLASH_ADDR, ROOTFS_MMU_MAP_SIZE);
    enable_core1_external_memory_bus(FLASH_MTD_XIP_ADDR, FLASH_MTD_SIZE);
    if (!prepare_core1_cached_psram()) {
        loader_restart("hart1 cached PSRAM setup");
    }
    disable_core1_stack_protector();
    esp_cpu_stall(1);
    cpu_utility_ll_enable_clock_and_reset_app_cpu();
    cpu_utility_ll_enable_clock_and_reset_app_cpu_int_matrix();
    ets_set_appcpu_boot_addr((uint32_t)(uintptr_t)core1_linux_trampoline);
    fence_i();
    esp_cpu_reset(1);
    esp_cpu_unstall(1);

    for (unsigned int i = 0; i < 1000 && !g_core1_trampoline_entered; i++) {
        esp_rom_delay_us(100);
    }
    if (!g_core1_trampoline_entered) {
        loader_restart("core1 trampoline timeout");
    }
}

#define OPENSBI_XIP_ADDR              0x40030000U
#define LINUX_XIP_ADDR                0x400B0000U
#define ROOTFS_FLASH_ADDR             0x40A20000U
#define FLASH_MTD_XIP_ADDR            0x41000000U
#define FLASH_MTD_SIZE                0x01000000U
#define OPENSBI_FDT_OFFSET_SLOT_SIZE  4U
#define FDT_MAGIC_LE                  0xEDFE0DD0U
#define ROOTFS_PARTITION_SIZE         0x005DC000U
#define ROOTFS_MMU_MAP_SIZE           0x005E0000U
#define ESP32S31_PSRAM_SIZE           0x01000000U
#define LINUX_PSRAM_START             0x50000000U

static bool map_flash_range(uint32_t vaddr, uint32_t paddr, uint32_t size);
static bool prepare_core1_cached_psram(void);
static void enable_core1_external_memory_bus(uint32_t vaddr, uint32_t size);
static void disable_core1_stack_protector(void);
static void start_linux_on_core1(uint32_t fdt);

static void prepare_linux_uart0(void)
{
    const uart_config_t config = {
        .baud_rate = 115200,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };

    /*
     * Linux uart0_default uses the S31 native UART0 IOMUX on GPIO58/59.
     * Configure it before entering OpenSBI so both OpenSBI and Linux
     * earlycon can use 0x2038a000 before Linux pinctrl is available.
     */
    ESP_ERROR_CHECK(uart_param_config(UART_NUM_0, &config));
    ESP_ERROR_CHECK(uart_set_pin(UART_NUM_0, 58, 59,
                                 UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));
}

void app_main(void)
{
    /* Allow the USB Serial/JTAG device to enumerate before loader output. */
    vTaskDelay(pdMS_TO_TICKS(5000));

    prepare_linux_uart0();

    if (!esp_psram_is_initialized() ||
        esp_psram_get_size() < ESP32S31_PSRAM_SIZE) {
        ESP_LOGE(TAG, "16 MiB PSRAM was not initialized");
        loader_restart("PSRAM initialization");
    }

    disable_apm();
    disable_watchdogs();

    /* Keep XIP mappings intact and expose the full flash at a second alias. */
    if (!map_flash_range(FLASH_MTD_XIP_ADDR, 0, FLASH_MTD_SIZE)) {
        ESP_LOGE(TAG, "failed to map complete Flash MTD window");
        loader_restart("Flash MTD mapping");
    }
    ESP_LOGI(TAG, "Flash MTD window mapped: flash=0x00000000 size=0x%08" PRIx32
                  " vaddr=0x%08" PRIx32,
             (uint32_t)FLASH_MTD_SIZE, (uint32_t)FLASH_MTD_XIP_ADDR);

    /* Map OpenSBI at its linked Flash XIP address. */
    const esp_partition_t *part = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, 0x40, "opensbi");
    if (!part) {
        ESP_LOGE(TAG, "OpenSBI partition not found");
        loader_restart("OpenSBI partition lookup");
    }

    if (!map_flash_range(OPENSBI_XIP_ADDR, part->address, part->size)) {
        ESP_LOGE(TAG, "failed to map OpenSBI at linked address");
        loader_restart("OpenSBI MMU mapping");
    }
    const void *opensbi_ptr = (const void *)(uintptr_t)OPENSBI_XIP_ADDR;
    ESP_LOGI(TAG, "OpenSBI mapped at %p (size %" PRIu32 ")",
             opensbi_ptr, part->size);

    if (part->size < OPENSBI_FDT_OFFSET_SLOT_SIZE) {
        ESP_LOGE(TAG, "OpenSBI partition too small for FDT offset slot");
        loader_restart("OpenSBI partition size");
    }

    uint32_t fdt_offset = 0;
    esp_err_t err = esp_flash_read(
        part->flash_chip, &fdt_offset,
        part->address + part->size - OPENSBI_FDT_OFFSET_SLOT_SIZE,
        sizeof(fdt_offset));
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "reading FDT offset failed: %s", esp_err_to_name(err));
        loader_restart("FDT offset read");
    }

    if (fdt_offset >= part->size - OPENSBI_FDT_OFFSET_SLOT_SIZE ||
        (fdt_offset & (sizeof(uint32_t) - 1)) != 0) {
        ESP_LOGE(TAG, "invalid FDT offset 0x%08" PRIx32, fdt_offset);
        loader_restart("FDT offset validation");
    }

    uint32_t fdt = (uint32_t)(uintptr_t)opensbi_ptr + fdt_offset;
    uint32_t fdt_magic = *(const volatile uint32_t *)(uintptr_t)fdt;
    if (fdt_magic != FDT_MAGIC_LE) {
        ESP_LOGE(TAG, "invalid FDT magic 0x%08" PRIx32 " at 0x%08" PRIx32,
                 fdt_magic, fdt);
        loader_restart("FDT magic validation");
    }

    ESP_LOGI(TAG, "OpenSBI FDT offset 0x%08" PRIx32 ", addr 0x%08" PRIx32,
             fdt_offset, fdt);

    /* --- Find and mmap Linux partition --- */
    const esp_partition_t *linux_part = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, 0x40, "linux");
    if (!linux_part) {
        ESP_LOGE(TAG, "Linux partition not found");
        loader_restart("Linux partition lookup");
    }
    if (!map_flash_range(LINUX_XIP_ADDR, linux_part->address,
                         linux_part->size)) {
        ESP_LOGE(TAG, "failed to map Linux at linked address");
        loader_restart("Linux MMU mapping");
    }
    const void *linux_ptr = (const void *)(uintptr_t)LINUX_XIP_ADDR;
    ESP_LOGI(TAG, "Linux mapped at %p (size %" PRIu32 ")",
             linux_ptr, linux_part->size);

    const esp_partition_t *rootfs_part = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, 0x40, "rootfs");
    if (!rootfs_part ||
        rootfs_part->size != ROOTFS_PARTITION_SIZE ||
        !map_flash_range(ROOTFS_FLASH_ADDR, rootfs_part->address,
                         ROOTFS_MMU_MAP_SIZE)) {
        ESP_LOGE(TAG, "failed to map 0x%08" PRIx32 "-byte rootfs",
                 (uint32_t)ROOTFS_PARTITION_SIZE);
        loader_restart("rootfs mapping");
    }

    ESP_LOGI(TAG, "rootfs mapped: flash=0x%08" PRIx32
                  " size=0x%08" PRIx32 " vaddr=0x%08" PRIx32,
             rootfs_part->address, rootfs_part->size,
             (uint32_t)ROOTFS_FLASH_ADDR);

    ESP_LOGI(TAG, "FreeRTOS HP SRAM primary region; hosted ring 0x%08" PRIx32
                  "..0x%08" PRIx32 "; DMA/status 0x%08" PRIx32 "..0x%08" PRIx32,
             (uint32_t)LINUX_SRAM_START, (uint32_t)LINUX_SRAM_RING_END,
             (uint32_t)LINUX_DMA_START, (uint32_t)LINUX_DMA_END);

    /* Publish the complete transport before hart1 can touch its rings.
     * Hart0 receives Linux traffic through the IDF-owned CPU3 doorbell;
     * the RX task blocks between notifications, so Linux cannot alter its
     * interrupt-matrix route by resetting hart1.
     */
    if (s31_hosted_sram_start() != ESP_OK)
        loader_restart("hosted SRAM transport");
    ESP_LOGI(TAG, "hosted SRAM transport started");

    err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES ||
        err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    if (err != ESP_OK || esp_hosted_coprocessor_init() != ESP_OK)
        loader_restart("ESP-Hosted co-processor");
    ESP_LOGI(TAG, "ESP-Hosted co-processor started");

#ifdef CONFIG_ESP_HOSTED_CP_BT
    if (init_bluetooth() != ESP_OK || enable_bluetooth() != ESP_OK)
        loader_restart("Bluetooth controller");
    ESP_LOGI(TAG, "Bluetooth controller enabled over Hosted VHCI");
#endif

    start_linux_on_core1(fdt);
    ESP_LOGI(TAG, "hart1 released to OpenSBI; hart0 FreeRTOS continues");
}
