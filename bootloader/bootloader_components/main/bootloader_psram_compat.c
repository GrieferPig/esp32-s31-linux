/*
 * Minimal non-OS glue for reusing the ESP-IDF S31 PSRAM device driver in the
 * second-stage bootloader without pulling in the full app startup stack.
 */
#include <stdbool.h>
#include <stdint.h>
#include "sdkconfig.h"
#include "esp_err.h"
#include "hal/clk_tree_hal.h"
#include "hal/ldo_ll.h"
#include "esp_private/rtc_clk.h"
#include "soc/periph_defs.h"
#include "soc/soc_caps.h"

#define HZ_PER_MHZ 1000000U

static uint32_t s_cur_mpll_freq_hz;
static uint32_t s_mpll_ref_count;

static esp_err_t acquire_psram_ldo(void)
{
#if CONFIG_ESP_LDO_RESERVE_PSRAM
    static bool s_ldo_acquired;

    if (!s_ldo_acquired) {
        if (!ldo_ll_is_valid_ldo_channel(CONFIG_ESP_LDO_CHAN_PSRAM_DOMAIN)) {
            return ESP_ERR_INVALID_ARG;
        }

        int unit_id = LDO_ID2UNIT(CONFIG_ESP_LDO_CHAN_PSRAM_DOMAIN);
        uint8_t dref = 0;
        uint8_t mul = 0;
        bool use_rail_voltage = false;

        ldo_ll_enable_current_limit(unit_id, true);
        ldo_ll_voltage_to_dref_mul(unit_id, CONFIG_ESP_LDO_VOLTAGE_PSRAM_DOMAIN,
                                   &dref, &mul, &use_rail_voltage);
        ldo_ll_adjust_voltage(unit_id, dref, mul, use_rail_voltage);
        ldo_ll_set_owner(unit_id, LDO_LL_UNIT_OWNER_SW);
        ldo_ll_enable_ripple_suppression(unit_id, true);
        ldo_ll_enable(unit_id, true);
        ldo_ll_enable_current_limit(unit_id, false);
        s_ldo_acquired = true;
    }
#endif

    return ESP_OK;
}

void periph_rcc_enter(void)
{
}

void periph_rcc_exit(void)
{
}

uint8_t periph_rcc_acquire_enter(shared_periph_module_t periph)
{
    (void)periph;
    periph_rcc_enter();
    return 0;
}

void periph_rcc_acquire_exit(shared_periph_module_t periph, uint8_t ref_count)
{
    (void)periph;
    (void)ref_count;
    periph_rcc_exit();
}

uint8_t periph_rcc_release_enter(shared_periph_module_t periph)
{
    (void)periph;
    periph_rcc_enter();
    return 0;
}

void periph_rcc_release_exit(shared_periph_module_t periph, uint8_t ref_count)
{
    (void)periph;
    (void)ref_count;
    periph_rcc_exit();
}

esp_err_t esp_clk_tree_mpll_acquire(void)
{
#if SOC_CLK_MPLL_SUPPORTED
    esp_err_t err = acquire_psram_ldo();
    if (err != ESP_OK) {
        return err;
    }

    if (s_mpll_ref_count == 0) {
        rtc_clk_mpll_enable();
    }
    s_mpll_ref_count++;
#endif
    return ESP_OK;
}

void esp_clk_tree_mpll_release(void)
{
    /*
     * Keep MPLL running after PSRAM init. OpenSBI/Linux inherit the external
     * memory controller state from this bootloader handoff.
     */
}

esp_err_t esp_clk_tree_mpll_freq_set(uint32_t expt_freq_hz, uint32_t *real_freq_hz)
{
#if SOC_CLK_MPLL_SUPPORTED
    uint32_t xtal_freq_mhz = clk_hal_xtal_get_freq_mhz();
    uint32_t target_freq_mhz = expt_freq_hz / HZ_PER_MHZ;

    if (s_mpll_ref_count == 0) {
        esp_err_t err = esp_clk_tree_mpll_acquire();
        if (err != ESP_OK) {
            return err;
        }
    }

    if (s_cur_mpll_freq_hz != expt_freq_hz) {
        rtc_clk_mpll_configure(xtal_freq_mhz, target_freq_mhz, false);
        s_cur_mpll_freq_hz = rtc_clk_mpll_get_freq() * HZ_PER_MHZ;
    }

    if (real_freq_hz) {
        *real_freq_hz = s_cur_mpll_freq_hz;
    }
#else
    if (real_freq_hz) {
        *real_freq_hz = 0;
    }
#endif
    return ESP_OK;
}

void mspi_timing_psram_tuning(void)
{
    /*
     * The full IDF timing tuner depends on app-side cache/RTOS plumbing.
     * Bootloader uses the controller's default delay settings after the IDF
     * device-mode init and keeps the path small enough for the 2nd stage.
     */
}
