#include <stdint.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_err.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "sdkconfig.h"

static const char *TAG = "i80_full_screen";

static bool frame_sent(esp_lcd_panel_io_handle_t io, esp_lcd_panel_io_event_data_t *event, void *context)
{
    BaseType_t wake = pdFALSE;
    xSemaphoreGiveFromISR((SemaphoreHandle_t)context, &wake);
    return wake == pdTRUE;
}

void example_i80_run_full_screen(esp_lcd_panel_io_handle_t io, esp_lcd_panel_handle_t panel)
{
    const int width = CONFIG_EXAMPLE_LCD_H_RES;
    const int height = CONFIG_EXAMPLE_LCD_V_RES;
    const size_t frame_bytes = (size_t)width * height * sizeof(uint16_t);
    const int bar_width = width < 16 ? width : 16;
    const uint16_t bar_color = __builtin_bswap16(0xffe0); // RGB565 in LCD wire byte order
    SemaphoreHandle_t complete = xSemaphoreCreateBinary();
    uint16_t *frame = esp_lcd_i80_alloc_draw_buffer(io, frame_bytes, 0);
    ESP_ERROR_CHECK(complete && frame ? ESP_OK : ESP_ERR_NO_MEM);

    const esp_lcd_panel_io_callbacks_t callbacks = {.on_color_trans_done = frame_sent};
    ESP_ERROR_CHECK(esp_lcd_panel_io_register_event_callbacks(io, &callbacks, complete));

    ESP_LOGI(TAG, "Moving bar: %dx%d RGB565, %zu bytes written for every frame", width, height, frame_bytes);
    int previous = 0;
    int current = 0;
    unsigned completed = 0;
    int64_t started = esp_timer_get_time();
    while (true) {
        for (int y = 0; y < height; y++) {
            uint16_t *row = frame + (size_t)y * width;
            for (int x = 0; x < bar_width; x++) {
                row[previous + x] = 0;
            }
            for (int x = 0; x < bar_width; x++) {
                row[current + x] = bar_color;
            }
        }
        // LCD_CAM streams the entire window through GDMA (ESP32-S31 TRM ch.24,
        // §24.5.2, LCD_CAM_LCD_DOUT and LCD_CAM_LCD_ALWAYS_OUT_EN).
        ESP_ERROR_CHECK(esp_lcd_panel_draw_bitmap(panel, 0, 0, width, height, frame));
        ESP_ERROR_CHECK(xSemaphoreTake(complete, pdMS_TO_TICKS(1000)) == pdTRUE ? ESP_OK : ESP_ERR_TIMEOUT);
        previous = current;
        current = current + 2 <= width - bar_width ? current + 2 : 0;

        if (++completed == 120) {
            const int64_t now = esp_timer_get_time();
            const double fps = completed * 1000000.0 / (now - started);
            ESP_LOGI(TAG, "Full-screen DMA: %u frames in %.3f s, %.2f FPS",
                     completed, (now - started) / 1000000.0, fps);
            started = now;
            completed = 0;
        }
    }
}
