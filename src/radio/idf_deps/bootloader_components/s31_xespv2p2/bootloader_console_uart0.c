#include "esp_rom_serial_output.h"
#include "esp_rom_sys.h"
#include "hal/uart_ll.h"
#include "hal/uart_periph.h"
#include "soc/rtc.h"

/* Keep the second-stage bootloader off the app's USB Serial/JTAG console. */
void __wrap_bootloader_console_init(void)
{
    esp_rom_install_uart_printf();
    esp_rom_output_set_as_console(0);
    esp_rom_output_tx_wait_idle(0);

    uint32_t clock_hz = rtc_clk_apb_freq_get();
#if ESP_ROM_UART_CLK_IS_XTAL
    clock_hz = (uint32_t)rtc_clk_xtal_freq_get() * MHZ;
#endif
    _uart_ll_set_baudrate(UART_LL_GET_HW(0), 115200, clock_hz);
}
