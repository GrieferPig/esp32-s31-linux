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
#include "esp_mac.h"
#include "esp_cpu.h"
#include "esp_flash.h"
#include "esp_partition.h"
#include "esp_cache.h"
#include "spi_flash_mmap.h"
#include "esp_psram.h"
#include "esp_rom_gpio.h"
#include "esp_rom_sys.h"
#include "driver/gpio.h"
#include "driver/sdmmc_host.h"
#include "esp_private/gpio.h"
#include "riscv/csr.h"
#include "esp32s31/rom/cache.h"
#include "hal/cache_ll.h"
#include "hal/assist_debug_ll.h"
#include "hal/wdt_hal.h"
#include "soc/soc.h"
#include "soc/timer_group_struct.h"
#include "soc/usb_serial_jtag_reg.h"
#include "soc/usb_serial_jtag_struct.h"
#include "soc/cnnt_io_mux_struct.h"
#include "soc/cnnt_sys_struct.h"
#include "soc/hp_sys_clkrst_struct.h"
#include "soc/io_mux_reg.h"
#include "soc/gpio_sig_map.h"
#include "soc/cpu_apm_reg.h"
#include "soc/hp_apm_reg.h"
#include "soc/hp_mem_apm_reg.h"
#include "hal/mmu_ll.h"
#include "hal/mmu_types.h"
#include "hal/emac_hal.h"
#include "freertos/FreeRTOS.h"

#define OPENSBI_XIP_ADDR              0x40030000U
#define LINUX_XIP_ADDR                0x400B0000U
#define INITRAMFS_FLASH_ADDR          0x40A20000U
#define FLASH_MTD_XIP_ADDR            0x41000000U
#define FLASH_MTD_SIZE                0x01000000U
#define OPENSBI_FDT_OFFSET_SLOT_SIZE  4U
#define FDT_MAGIC_LE                  0xEDFE0DD0U
#define INITRAMFS_PARTITION_SIZE      0x005E0000U
#define ESP32S31_PSRAM_SIZE           0x01000000U
#define PMP_FULL_SPACE_NAPOT_ADDR     0x3FFFFFFFU
#define PMP_ENTRY_RWX_LOCK_NAPOT      0x9FU

/*
 * These are the fixed S31 IOMUX routes used by ESP-IDF's default RGMII
 * configuration.  ESP32-S31-Function-CoreBoard-1 routes these signals to its
 * onboard Motorcomm YT8531DC-CA Gigabit PHY.
 */
#define EMAC_RGMII_MDC_GPIO           5
#define EMAC_RGMII_MDIO_GPIO          6
#define EMAC_PHY_RESET_GPIO            7
#define EMAC_RGMII_TXD0_GPIO          8
#define EMAC_RGMII_TXD1_GPIO          9
#define EMAC_RGMII_TXD2_GPIO          10
#define EMAC_RGMII_TXD3_GPIO          11
#define EMAC_RGMII_TX_CTL_GPIO        12
#define EMAC_RGMII_TX_CLK_GPIO        13
#define EMAC_RGMII_RX_CLK_GPIO        14
#define EMAC_RGMII_RX_CTL_GPIO        15
#define EMAC_RGMII_RXD3_GPIO          16
#define EMAC_RGMII_RXD2_GPIO          17
#define EMAC_RGMII_RXD1_GPIO          18
#define EMAC_RGMII_RXD0_GPIO          19

static const char *TAG = "boot";

static void log_prepare_error(const char *what, esp_err_t err)
{
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "%s: %s", what, esp_err_to_name(err));
    }
}

/*
 * The Linux dw_mmc driver has no S31 clock/power-domain provider yet.  Keep
 * the controller's MPLL/8 (62.5 MHz) source, its gates, and its SRAM awake
 * across the firmware handoff.  sdmmc_host_init_slot() selects the native
 * IOMUX route: CLK/CMD/D0..D3 = GPIO24/25/20..23.
 */
static void prepare_sdmmc_for_linux(void)
{
    sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();
    esp_err_t err;

    CNNT_SYS_REG.sys_sdmmc_mem_lp_ctrl.sys_sdmmc_mem_lp_force_ctrl = 1;
    CNNT_SYS_REG.sys_sdmmc_mem_lp_ctrl.sys_sdmmc_mem_lp_en = 0;

    err = sdmmc_host_init();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        log_prepare_error("SDMMC host clock/reset setup failed", err);
        return;
    }

    /*
     * The bring-up board is wired as a 1-bit SD bus (CLK/CMD/D0 only).
     * Advertising four data lines makes the SD core switch the card to
     * 4-bit mode even though D1-D3 are not connected.
     */
    slot.width = 1;
    slot.flags = SDMMC_SLOT_FLAG_INTERNAL_PULLUP;
    err = sdmmc_host_init_slot(SDMMC_HOST_SLOT_0, &slot);
    if (err != ESP_OK) {
        log_prepare_error("SDMMC slot0 IOMUX setup failed", err);
        return;
    }

    /* Use the same safe initial card clock as IDF's SDMMC host. */
    log_prepare_error("SDMMC initial clock setup failed",
                      sdmmc_host_set_card_clk(SDMMC_HOST_SLOT_0, 20000));
    ESP_LOGI(TAG, "SDMMC prepared: slot0 1-bit GPIO20/24/25, CIU=MPLL/8 (62.5 MHz)");
}

static void emac_rgmii_iomux_output(gpio_num_t gpio, int func)
{
    log_prepare_error("EMAC RGMII output IOMUX failed", gpio_iomux_output(gpio, func));
}

static void emac_rgmii_iomux_input(gpio_num_t gpio, int func, uint32_t signal)
{
    log_prepare_error("EMAC RGMII input IOMUX failed", gpio_iomux_input(gpio, func, signal));
}

static bool emac_read_phy(emac_hal_context_t *hal, unsigned int phy, unsigned int reg,
                          uint16_t *value)
{
    emac_hal_set_phy_cmd(hal, phy, reg, false);
    for (unsigned int timeout = 0; timeout < 1000; timeout++) {
        if (!emac_hal_is_mii_busy(hal)) {
            *value = emac_hal_get_phy_data(hal);
            return true;
        }
        esp_rom_delay_us(10);
    }
    return false;
}

/*
 * Use IDF's ESP32-S31 default RGMII IOMUX group.  MPLL is programmed to
 * 500 MHz by the IDF clock tree; divide-by-four supplies the initial 125 MHz
 * TX clock.  Linux reprograms the MAC after taking ownership.
 */
static void prepare_emac_for_linux(void)
{
    emac_hal_context_t hal;
    uint8_t eth_mac[6];
    uint16_t phy_id1 = UINT16_MAX;
    uint16_t phy_id2 = UINT16_MAX;

    CNNT_SYS_REG.sys_gmac_mem_lp_ctrl.sys_gmac_mem_lp_force_ctrl = 1;
    CNNT_SYS_REG.sys_gmac_mem_lp_ctrl.sys_gmac_mem_lp_en = 0;
    CNNT_SYS_REG.sys_gmac_ctrl0.sys_gmac_mem_clk_force_on = 1;
    HP_SYS_CLKRST.emac_ctrl0.reg_emac_sys_clk_en = 1;
    CNNT_SYS_REG.sys_hp_emac_ctrl.sys_emac_rst_en = 1;
    CNNT_SYS_REG.sys_hp_emac_ctrl.sys_emac_rst_en = 0;

    /*
     * Function-CoreBoard-1: YT8531 RESET_N is GPIO7.  Hold it low while the
     * MAC clocks and RGMII pads are prepared, then leave it deasserted for
     * Linux.  PHYAD[2:0] are strapped low on this board (address 0).
     */
    log_prepare_error("YT8531 reset GPIO setup failed",
                      gpio_reset_pin(EMAC_PHY_RESET_GPIO));
    log_prepare_error("YT8531 reset GPIO direction failed",
                      gpio_set_direction(EMAC_PHY_RESET_GPIO, GPIO_MODE_OUTPUT));
    log_prepare_error("YT8531 reset assert failed",
                      gpio_set_level(EMAC_PHY_RESET_GPIO, 0));

    /* GPIO13-19 are CNNT pads and need the dedicated GMAC control path. */
    CNNT_PAD_CTRL.ctrl.gmac_pad_pin_ctrl_ded_sel = 1;
    emac_rgmii_iomux_output(EMAC_RGMII_TXD0_GPIO, FUNC_GPIO8_GMAC_PHY_TXD0_PAD);
    emac_rgmii_iomux_output(EMAC_RGMII_TXD1_GPIO, FUNC_GPIO9_GMAC_PHY_TXD1_PAD);
    emac_rgmii_iomux_output(EMAC_RGMII_TXD2_GPIO, FUNC_GPIO10_GMAC_PHY_TXD2_PAD);
    emac_rgmii_iomux_output(EMAC_RGMII_TXD3_GPIO, FUNC_GPIO11_GMAC_PHY_TXD3_PAD);
    emac_rgmii_iomux_output(EMAC_RGMII_TX_CTL_GPIO, FUNC_GPIO12_GMAC_PHY_TXEN_PAD);
    emac_rgmii_iomux_output(EMAC_RGMII_TX_CLK_GPIO, FUNC_GPIO13_GMAC_RMII_CLK_PAD);
    emac_rgmii_iomux_input(EMAC_RGMII_RX_CLK_GPIO, FUNC_GPIO14_GMAC_RX_CLK_PAD,
                           GMAC_RX_CLK_PAD_IN_IDX);
    emac_rgmii_iomux_input(EMAC_RGMII_RX_CTL_GPIO, FUNC_GPIO15_GMAC_PHY_RXDV_PAD,
                           GMAC_PHY_RXDV_PAD_IN_IDX);
    emac_rgmii_iomux_input(EMAC_RGMII_RXD3_GPIO, FUNC_GPIO16_GMAC_PHY_RXD3_PAD,
                           GMAC_PHY_RXD3_PAD_IN_IDX);
    emac_rgmii_iomux_input(EMAC_RGMII_RXD2_GPIO, FUNC_GPIO17_GMAC_PHY_RXD2_PAD,
                           GMAC_PHY_RXD2_PAD_IN_IDX);
    emac_rgmii_iomux_input(EMAC_RGMII_RXD1_GPIO, FUNC_GPIO18_GMAC_PHY_RXD1_PAD,
                           GMAC_PHY_RXD1_PAD_IN_IDX);
    emac_rgmii_iomux_input(EMAC_RGMII_RXD0_GPIO, FUNC_GPIO19_GMAC_PHY_RXD0_PAD,
                           GMAC_PHY_RXD0_PAD_IN_IDX);

    /*
     * MDC is output; MDIO is bidirectional and uses the GPIO matrix.  Keep
     * the MDIO input buffer explicitly enabled: this mirrors IDF's
     * emac_esp_gpio_init_smi() and is required for GMII_MDI_PAD_IN_IDX to see
     * the PHY's turnaround/read data.
     */
    esp_rom_gpio_connect_out_signal(EMAC_RGMII_MDC_GPIO, GMII_MDC_PAD_OUT_IDX, false, false);
    esp_rom_gpio_connect_out_signal(EMAC_RGMII_MDIO_GPIO, GMII_MDO_PAD_OUT_IDX, false, false);
    log_prepare_error("EMAC MDIO input enable failed",
                      gpio_input_enable(EMAC_RGMII_MDIO_GPIO));
    esp_rom_gpio_connect_in_signal(EMAC_RGMII_MDIO_GPIO, GMII_MDI_PAD_IN_IDX, false);
    log_prepare_error("EMAC MDC GPIO pull setup failed",
                      gpio_set_pull_mode(EMAC_RGMII_MDC_GPIO, GPIO_FLOATING));
    log_prepare_error("EMAC MDIO GPIO pull setup failed",
                      gpio_set_pull_mode(EMAC_RGMII_MDIO_GPIO, GPIO_FLOATING));
    log_prepare_error("EMAC MDC GPIO function setup failed",
                      gpio_func_sel(EMAC_RGMII_MDC_GPIO, PIN_FUNC_GPIO));
    log_prepare_error("EMAC MDIO GPIO function setup failed",
                      gpio_func_sel(EMAC_RGMII_MDIO_GPIO, PIN_FUNC_GPIO));

    /* RGMII, MPLL 500 MHz / 4 = 125 MHz TXC, and 40 MHz PTP reference. */
    CNNT_SYS_REG.sys_gmac_ctrl0.sys_phy_intf_sel = 1;
    CNNT_SYS_REG.sys_hp_emac_ref_ctrl.sys_emac_ref_clk_sel = 0;
    CNNT_SYS_REG.sys_hp_emac_ref_ctrl.sys_emac_ref_clk_div_num = 3;
    CNNT_SYS_REG.sys_hp_emac_ref_ctrl.sys_emac_ref_clk_en = 1;
    CNNT_SYS_REG.sys_hp_emac_rmii_pad_ctrl.sys_emac_rmii_pad_clk_en = 0;
    CNNT_SYS_REG.sys_hp_emac_rmii_ctrl.sys_emac_rmii_clk_en = 0;
    CNNT_SYS_REG.sys_hp_emac_rmii_ctrl.sys_emac_rmii_pad_out_clk_en = 1;
    CNNT_SYS_REG.sys_hp_emac_rx_ctrl.sys_emac_rx_pad_clk_en = 1;
    CNNT_SYS_REG.sys_hp_emac_rx_ctrl.sys_emac_rx_clk_sel = 1;
    CNNT_SYS_REG.sys_hp_emac_rx_ctrl.sys_emac_rx_180_clk_en = 1;
    CNNT_SYS_REG.sys_hp_emac_tx_ctrl.sys_emac_tx_180_clk_en = 1;
    CNNT_SYS_REG.sys_hp_emac_ptp_ctrl.sys_emac_ptp_ref_clk_sel = 0;
    CNNT_SYS_REG.sys_hp_emac_ptp_ctrl.sys_emac_ptp_ref_clk_en = 1;

    esp_rom_delay_us(10000);
    log_prepare_error("YT8531 reset release failed",
                      gpio_set_level(EMAC_PHY_RESET_GPIO, 1));
    /* Allow the 25 MHz crystal, PLL, and MDIO block to settle before probing. */
    esp_rom_delay_us(100000);
    ESP_LOGI(TAG, "YT8531 MDIO idle level after reset: %d",
             gpio_get_level(EMAC_RGMII_MDIO_GPIO));

    emac_hal_init(&hal);
    emac_hal_reset(&hal);
    for (unsigned int timeout = 0; timeout < 100; timeout++) {
        if (emac_hal_is_reset_done(&hal)) {
            break;
        }
        esp_rom_delay_us(1000);
    }
    if (!emac_hal_is_reset_done(&hal)) {
        ESP_LOGW(TAG, "EMAC software reset did not complete before MDIO probe");
    }
    emac_hal_set_csr_clock_range(&hal, 80000000);
    if (esp_read_mac(eth_mac, ESP_MAC_ETH) == ESP_OK) {
        /* Leave the unique eFuse-derived ETH MAC for stmmac to discover. */
        emac_hal_set_address(&hal, eth_mac);
        ESP_LOGI(TAG, "EMAC MAC from eFuse: %02x:%02x:%02x:%02x:%02x:%02x",
                 eth_mac[0], eth_mac[1], eth_mac[2], eth_mac[3],
                 eth_mac[4], eth_mac[5]);
    } else {
        ESP_LOGW(TAG, "Unable to read eFuse ETH MAC");
    }
    ESP_LOGI(TAG, "EMAC prepared: YT8531@0 reset GPIO7, RGMII GPIO8-19, TXC=125 MHz, PTP=40 MHz");

    for (unsigned int phy = 0; phy < 32; phy++) {
        bool id1_read = emac_read_phy(&hal, phy, 2, &phy_id1);
        bool id2_read = emac_read_phy(&hal, phy, 3, &phy_id2);

        if (phy == 0) {
            ESP_LOGI(TAG, "YT8531 MDIO@0: id1 %s=0x%04x id2 %s=0x%04x",
                     id1_read ? "ok" : "timeout", phy_id1,
                     id2_read ? "ok" : "timeout", phy_id2);
        }
        if (id1_read && id2_read &&
            phy_id1 != 0 && phy_id1 != UINT16_MAX &&
            phy_id2 != 0 && phy_id2 != UINT16_MAX) {
            ESP_LOGI(TAG, "EMAC PHY addr %u: id 0x%04x 0x%04x", phy, phy_id1, phy_id2);
        }
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
    prepare_sdmmc_for_linux();
    prepare_emac_for_linux();
    clear_mstatus_mprv();
    install_global_pmp();

    /* Keep XIP mappings intact and expose the full flash at a second alias. */
    if (!map_flash_range(FLASH_MTD_XIP_ADDR, 0, FLASH_MTD_SIZE)) {
        ESP_LOGE(TAG, "failed to map complete Flash MTD window");
        esp_restart();
    }
    ESP_LOGI(TAG, "Flash MTD window mapped: flash=0x00000000 size=0x%08" PRIx32
                  " vaddr=0x%08" PRIx32,
             (uint32_t)FLASH_MTD_SIZE, (uint32_t)FLASH_MTD_XIP_ADDR);

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
