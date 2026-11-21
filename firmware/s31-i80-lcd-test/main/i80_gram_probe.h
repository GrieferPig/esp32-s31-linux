#pragma once

#include "driver/gpio.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"

void i80_gram_probe_hold_rd_high(gpio_num_t rd);
void i80_gram_probe_write(esp_lcd_panel_io_handle_t io, esp_lcd_panel_handle_t panel);
void i80_gram_probe_read(gpio_num_t rd);
