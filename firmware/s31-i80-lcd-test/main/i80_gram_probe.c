#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "esp_err.h"
#include "esp_lcd_panel_commands.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"
#include "sdkconfig.h"
#include "i80_gram_probe.h"

static const char *TAG = "i80_gram";
static const gpio_num_t data_pins[] = {
    CONFIG_EXAMPLE_PIN_NUM_DATA0, CONFIG_EXAMPLE_PIN_NUM_DATA1,
    CONFIG_EXAMPLE_PIN_NUM_DATA2, CONFIG_EXAMPLE_PIN_NUM_DATA3,
    CONFIG_EXAMPLE_PIN_NUM_DATA4, CONFIG_EXAMPLE_PIN_NUM_DATA5,
    CONFIG_EXAMPLE_PIN_NUM_DATA6, CONFIG_EXAMPLE_PIN_NUM_DATA7,
};

enum {
    PROBE_PIXELS = 64,
    PROBE_READ_BYTES = PROBE_PIXELS * 3,
};

// RGB565 values with deliberately distinct channel and bit patterns.
static const uint16_t colors[] = {0xa2cd, 0x5d32, 0xf800, 0x07e0, 0x001f, 0xffff, 0, 0x94ab};

void i80_gram_probe_hold_rd_high(gpio_num_t rd)
{
    ESP_ERROR_CHECK(GPIO_IS_VALID_OUTPUT_GPIO(rd) && rd != CONFIG_EXAMPLE_PIN_NUM_PCLK &&
                    rd != CONFIG_EXAMPLE_PIN_NUM_DC && rd != CONFIG_EXAMPLE_PIN_NUM_CS &&
                    rd != CONFIG_EXAMPLE_PIN_NUM_RST && rd != CONFIG_EXAMPLE_PIN_NUM_BK_LIGHT
                    ? ESP_OK : ESP_ERR_INVALID_ARG);
    for (size_t i = 0; i < sizeof(data_pins) / sizeof(data_pins[0]); i++) {
        ESP_ERROR_CHECK(rd != data_pins[i] ? ESP_OK : ESP_ERR_INVALID_ARG);
    }
    ESP_ERROR_CHECK(gpio_set_level(rd, 1));
    const gpio_config_t cfg = {
        .pin_bit_mask = BIT64(rd),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&cfg));
}

void i80_gram_probe_write(esp_lcd_panel_io_handle_t io, esp_lcd_panel_handle_t panel)
{
    const size_t num_colors = sizeof(colors) / sizeof(colors[0]);
    uint8_t *pixels = esp_lcd_i80_alloc_draw_buffer(io, PROBE_PIXELS * sizeof(uint16_t), 0);
    ESP_ERROR_CHECK(pixels ? ESP_OK : ESP_ERR_NO_MEM);
    for (size_t i = 0; i < PROBE_PIXELS; i++) {
        const uint16_t color = colors[i % num_colors];
        pixels[2 * i] = color >> 8;
        pixels[2 * i + 1] = color;
    }
    ESP_ERROR_CHECK(esp_lcd_panel_draw_bitmap(panel, 0, 0, PROBE_PIXELS, 1, pixels));

    // tx_param drains queued color transfers before the DMA buffer can be freed.
    ESP_ERROR_CHECK(esp_lcd_panel_io_tx_param(io, LCD_CMD_NOP, NULL, 0));
    free(pixels);
    ESP_LOGI(TAG, "Sent %d RGB565 pixels through LCD_CAM DMA", PROBE_PIXELS);
}

static void set_data_bus_direction(gpio_mode_t mode, gpio_pull_mode_t pull)
{
    for (size_t i = 0; i < sizeof(data_pins) / sizeof(data_pins[0]); i++) {
        // The probe temporarily owns the idle LCD_CAM pads. Only change the
        // direction and pull, keeping the driver's pin reservations intact.
        ESP_ERROR_CHECK(gpio_set_direction(data_pins[i], mode));
        ESP_ERROR_CHECK(gpio_set_pull_mode(data_pins[i], pull));
    }
}

static uint8_t sample_data_bus(void)
{
    uint8_t value = 0;
    for (size_t bit = 0; bit < sizeof(data_pins) / sizeof(data_pins[0]); bit++) {
        value |= gpio_get_level(data_pins[bit]) << bit;
    }
    return value;
}

static void drive_data_bus(uint8_t value)
{
    for (size_t bit = 0; bit < sizeof(data_pins) / sizeof(data_pins[0]); bit++) {
        ESP_ERROR_CHECK(gpio_set_level(data_pins[bit], (value >> bit) & 1));
    }
}

static void gpio_write_byte(uint8_t value)
{
    ESP_ERROR_CHECK(gpio_set_level(CONFIG_EXAMPLE_PIN_NUM_PCLK, 0));
    drive_data_bus(value);
    // ILI9341 section 19.3: 15 ns WR low and 10 ns data setup/hold.
    esp_rom_delay_us(1);
    ESP_ERROR_CHECK(gpio_set_level(CONFIG_EXAMPLE_PIN_NUM_PCLK, 1));
    esp_rom_delay_us(1);
}

static void gpio_begin_command(uint8_t cmd)
{
    set_data_bus_direction(GPIO_MODE_OUTPUT, GPIO_FLOATING);
    ESP_ERROR_CHECK(gpio_set_level(CONFIG_EXAMPLE_PIN_NUM_DC, 0));
    ESP_ERROR_CHECK(gpio_set_level(CONFIG_EXAMPLE_PIN_NUM_CS, 0));
    gpio_write_byte(cmd);
    ESP_ERROR_CHECK(gpio_set_level(CONFIG_EXAMPLE_PIN_NUM_DC, 1));
}

static void gpio_write_command(uint8_t cmd, const uint8_t *params, size_t count)
{
    gpio_begin_command(cmd);
    for (size_t i = 0; i < count; i++) {
        gpio_write_byte(params[i]);
    }
    ESP_ERROR_CHECK(gpio_set_level(CONFIG_EXAMPLE_PIN_NUM_CS, 1));
}

static void gpio_read_command(gpio_num_t rd, uint8_t cmd, gpio_pull_mode_t pull,
                              uint8_t *bytes, size_t size)
{
    gpio_begin_command(cmd);
    set_data_bus_direction(GPIO_MODE_INPUT, pull);
    // ILI9341 section 19.3: TRCSFM >= 355 ns, TRATFM <= 340 ns, TRCFM >= 450 ns.
    esp_rom_delay_us(1);
    for (size_t i = 0; i < size; i++) {
        ESP_ERROR_CHECK(gpio_set_level(rd, 0));
        esp_rom_delay_us(1);
        bytes[i] = sample_data_bus();
        ESP_ERROR_CHECK(gpio_set_level(rd, 1));
        esp_rom_delay_us(1);
    }
    ESP_ERROR_CHECK(gpio_set_level(CONFIG_EXAMPLE_PIN_NUM_CS, 1));
}

static void read_register(gpio_num_t rd, uint8_t cmd, gpio_pull_mode_t pull,
                          uint8_t *bytes, size_t count)
{
    // ILI9341 sections 8.2 and 8.3.23: these parallel reads begin with a dummy byte.
    gpio_read_command(rd, cmd, pull, bytes, count + 1);
}

#if CONFIG_EXAMPLE_I80_FULL_SCREEN_DEMO
static void measure_panel_scan(gpio_num_t rd)
{
    // ILI9341 §8.1: GET_SCANLINE (45h) returns a dummy byte and a 10-bit line index.
    const int64_t deadline = esp_timer_get_time() + 500000;
    int64_t first_crossing = 0;
    int64_t last_crossing = 0;
    int64_t last_sample = 0;
    int64_t max_gap = 0;
    unsigned crossings = 0;
    bool armed = false;
    unsigned previous_line = 0;

    while (esp_timer_get_time() < deadline) {
        uint8_t bytes[3];
        read_register(rd, 0x45, GPIO_PULLDOWN_ONLY, bytes, sizeof(bytes) - 1);
        const unsigned line = ((bytes[1] & 3) << 8) | bytes[2];
        const int64_t now = esp_timer_get_time();
        if (last_sample && now - last_sample > max_gap) {
            max_gap = now - last_sample;
        }
        last_sample = now;

        // A rising crossing near the top of the screen identifies one scan.
        if (armed && previous_line < 32 && line >= 32 && line < 96) {
            if (!first_crossing) {
                first_crossing = now;
            }
            last_crossing = now;
            crossings++;
            armed = false;
        } else if (line >= 128) {
            armed = true;
        }
        previous_line = line;
    }

    if (crossings >= 8 && max_gap < 2500) {
        const double scan_hz = (crossings - 1) * 1000000.0 / (last_crossing - first_crossing);
        ESP_LOGI(TAG, "Panel scan from GET_SCANLINE: %.2f Hz (%u complete intervals)",
                 scan_hz, crossings - 1);
    } else {
        ESP_LOGW(TAG, "GET_SCANLINE insufficient for rate measurement: %u crossings, max gap %lld us",
                 crossings, (long long)max_gap);
    }
}
#endif

static void check_read_drive(gpio_num_t rd)
{
    gpio_begin_command(0xd3);
    set_data_bus_direction(GPIO_MODE_INPUT, GPIO_PULLDOWN_ONLY);
    // Skip the RDID4 dummy cycle, then hold the first payload byte selected.
    ESP_ERROR_CHECK(gpio_set_level(rd, 0));
    esp_rom_delay_us(1);
    ESP_ERROR_CHECK(gpio_set_level(rd, 1));
    esp_rom_delay_us(1);
    ESP_ERROR_CHECK(gpio_set_level(rd, 0));
    esp_rom_delay_us(1);
    const uint8_t down = sample_data_bus();
    for (size_t i = 0; i < sizeof(data_pins) / sizeof(data_pins[0]); i++) {
        ESP_ERROR_CHECK(gpio_set_pull_mode(data_pins[i], GPIO_PULLUP_ONLY));
    }
    const uint8_t up = sample_data_bus();
    const int rst_level = gpio_get_level(CONFIG_EXAMPLE_PIN_NUM_RST);
    const int cs_level = gpio_get_level(CONFIG_EXAMPLE_PIN_NUM_CS);
    const int wr_level = gpio_get_level(CONFIG_EXAMPLE_PIN_NUM_PCLK);
    const int dc_level = gpio_get_level(CONFIG_EXAMPLE_PIN_NUM_DC);
    const int rd_level = gpio_get_level(rd);
    ESP_ERROR_CHECK(gpio_set_level(rd, 1));
    esp_rom_delay_us(1);
    ESP_ERROR_CHECK(gpio_set_level(CONFIG_EXAMPLE_PIN_NUM_CS, 1));

    ESP_LOGI(TAG, "Selected read pads: RESET#=%d CS#=%d WR#=%d D/C=%d RD#=%d",
             rst_level, cs_level, wr_level, dc_level, rd_level);
    ESP_LOGI(TAG, "One held RDID4 byte: down=%02x up=%02x changed-mask=%02x", down, up, down ^ up);
}

static uint8_t read_status_byte(gpio_num_t rd, uint8_t command, gpio_pull_mode_t pull)
{
    uint8_t bytes[2];
    read_register(rd, command, pull, bytes, 1);
    return bytes[1];
}

static void check_panel_registers(gpio_num_t rd)
{
    // ILI9341 sections 8.2.5–8.2.7; these reads are available without EXTC.
    const uint8_t power_down = read_status_byte(rd, 0x0a, GPIO_PULLDOWN_ONLY);
    const uint8_t access_down = read_status_byte(rd, 0x0b, GPIO_PULLDOWN_ONLY);
    const uint8_t color_down = read_status_byte(rd, 0x0c, GPIO_PULLDOWN_ONLY);
    const uint8_t power_up = read_status_byte(rd, 0x0a, GPIO_PULLUP_ONLY);
    const uint8_t access_up = read_status_byte(rd, 0x0b, GPIO_PULLUP_ONLY);
    const uint8_t color_up = read_status_byte(rd, 0x0c, GPIO_PULLUP_ONLY);
    ESP_LOGI(TAG, "RDDPM=%02x/%02x RDDMADCTL=%02x/%02x RDDCOLMOD=%02x/%02x (down/up)",
             power_down, power_up, access_down, access_up, color_down, color_up);
    if (power_down != power_up || access_down != access_up || color_down != color_up) {
        ESP_LOGE(TAG, "Panel register response depends on host data pulls");
    }

    const uint8_t write_565 = 0x55;
    gpio_write_command(LCD_CMD_COLMOD, &write_565, sizeof(write_565));
    const uint8_t read_565 = read_status_byte(rd, 0x0c, GPIO_PULLDOWN_ONLY);
    const uint8_t write_666 = 0x66;
    gpio_write_command(LCD_CMD_COLMOD, &write_666, sizeof(write_666));
    const uint8_t read_666 = read_status_byte(rd, 0x0c, GPIO_PULLDOWN_ONLY);
    ESP_LOG_LEVEL((read_565 & 7) == 5 && (read_666 & 7) == 6 ? ESP_LOG_INFO : ESP_LOG_ERROR, TAG,
                  "COLMOD change: DBI expected 5 -> 6, read %u -> %u (raw %02x -> %02x)",
                  read_565 & 7, read_666 & 7, read_565, read_666);
}

static bool all_bytes_are(const uint8_t *data, size_t size, uint8_t value)
{
    for (size_t i = 0; i < size; i++) {
        if (data[i] != value) {
            return false;
        }
    }
    return true;
}

static size_t compare_pixels(const uint8_t *data)
{
    const size_t num_colors = sizeof(colors) / sizeof(colors[0]);
    size_t matches = 0;
    for (size_t i = 0; i < PROBE_PIXELS; i++) {
        const uint8_t *rgb = &data[3 * i];
        // RAMRD returns left-aligned RGB666. Ignore the added low bit of R and B.
        const uint16_t actual = ((rgb[0] & 0xf8) << 8) | ((rgb[1] & 0xfc) << 3) | (rgb[2] >> 3);
        const uint16_t expected = colors[i % num_colors];
        if (actual == expected) {
            matches++;
        } else if (i < num_colors) {
            ESP_LOGW(TAG, "Pixel %u: expected %04x, got %04x (raw %02x %02x %02x)",
                     (unsigned)i, expected, actual, rgb[0], rgb[1], rgb[2]);
        }
    }

    return matches;
}

static void probe_gpio_write(gpio_num_t rd, uint8_t *readback)
{
    // Repeat the write over GPIO to separate LCD_CAM/DMA from the panel bus.
    const uint8_t columns[] = {0, 0, 0, PROBE_PIXELS - 1};
    const uint8_t page[] = {0, 0, 0, 0};
    const uint8_t write_colmod[] = {0x55};
    gpio_write_command(LCD_CMD_CASET, columns, sizeof(columns));
    gpio_write_command(LCD_CMD_RASET, page, sizeof(page));
    gpio_write_command(LCD_CMD_COLMOD, write_colmod, sizeof(write_colmod));
    gpio_begin_command(LCD_CMD_RAMWR);
    for (size_t i = 0; i < PROBE_PIXELS; i++) {
        const uint16_t color = colors[i % (sizeof(colors) / sizeof(colors[0]))];
        gpio_write_byte(color >> 8);
        gpio_write_byte(color);
    }
    ESP_ERROR_CHECK(gpio_set_level(CONFIG_EXAMPLE_PIN_NUM_CS, 1));
    read_register(rd, 0x2e, GPIO_PULLDOWN_ONLY, readback, PROBE_READ_BYTES);
    const size_t matches = compare_pixels(readback + 1);
    ESP_LOG_LEVEL(matches == PROBE_PIXELS ? ESP_LOG_INFO : ESP_LOG_ERROR, TAG,
                  "GPIO write/RAMRD comparison: %u/%d pixels match", (unsigned)matches, PROBE_PIXELS);
}

static void check_mcu_pads(gpio_num_t rd)
{
    // The panel is deselected for this local pad readback.
    set_data_bus_direction(GPIO_MODE_INPUT_OUTPUT, GPIO_FLOATING);
    uint8_t errors = 0;
    for (size_t bit = 0; bit < sizeof(data_pins) / sizeof(data_pins[0]); bit++) {
        const uint8_t expected = 1U << bit;
        drive_data_bus(expected);
        errors |= sample_data_bus() ^ expected;
    }
    set_data_bus_direction(GPIO_MODE_INPUT, GPIO_PULLDOWN_ONLY);

    ESP_ERROR_CHECK(gpio_input_enable(rd));
    ESP_ERROR_CHECK(gpio_set_level(rd, 0));
    const int rd_low = gpio_get_level(rd);
    ESP_ERROR_CHECK(gpio_set_level(rd, 1));
    const int rd_high = gpio_get_level(rd);
    ESP_LOG_LEVEL(errors == 0 && rd_low == 0 && rd_high == 1 ? ESP_LOG_INFO : ESP_LOG_ERROR, TAG,
                  "MCU pad check: D0..D7 mismatch mask=%02x, RD# sampled low/high=%d/%d",
                  errors, rd_low, rd_high);
}

void i80_gram_probe_read(gpio_num_t rd)
{
    // ESP32-S31 TRM ch.24 LCD_CAM, section 24.3.2 and Table 24.3-1:
    // LCD master is TX-only (LCD_PCLK/DC/CS are outputs). The native transfer
    // has finished; gpio_config moves these pads from LCD_CAM to GPIO ownership.
    ESP_ERROR_CHECK(gpio_set_level(CONFIG_EXAMPLE_PIN_NUM_CS, 1));
    ESP_ERROR_CHECK(gpio_set_level(CONFIG_EXAMPLE_PIN_NUM_PCLK, 1));
    ESP_ERROR_CHECK(gpio_set_level(CONFIG_EXAMPLE_PIN_NUM_DC, 1));
    const gpio_config_t control = {
        .pin_bit_mask = BIT64(CONFIG_EXAMPLE_PIN_NUM_CS) | BIT64(CONFIG_EXAMPLE_PIN_NUM_PCLK) |
                        BIT64(CONFIG_EXAMPLE_PIN_NUM_DC),
        .mode = GPIO_MODE_INPUT_OUTPUT,
    };
    ESP_ERROR_CHECK(gpio_config(&control));
    ESP_ERROR_CHECK(gpio_input_enable(CONFIG_EXAMPLE_PIN_NUM_RST));

    check_mcu_pads(rd);
    ESP_LOGI(TAG, "Idle control pads: RESET#=%d CS#=%d WR#=%d D/C=%d",
             gpio_get_level(CONFIG_EXAMPLE_PIN_NUM_RST), gpio_get_level(CONFIG_EXAMPLE_PIN_NUM_CS),
             gpio_get_level(CONFIG_EXAMPLE_PIN_NUM_PCLK), gpio_get_level(CONFIG_EXAMPLE_PIN_NUM_DC));
    const uint8_t idle_pull_down = sample_data_bus();
    set_data_bus_direction(GPIO_MODE_INPUT, GPIO_PULLUP_ONLY);
    ESP_LOGI(TAG, "Data bus idle (CS#/RD# high), pull-down=%02x pull-up=%02x",
             idle_pull_down, sample_data_bus());

    const uint8_t power = read_status_byte(rd, LCD_CMD_RDDPM, GPIO_PULLDOWN_ONLY);
    const uint8_t madctl = read_status_byte(rd, LCD_CMD_RDD_MADCTL, GPIO_PULLDOWN_ONLY);
    const uint8_t colmod = read_status_byte(rd, LCD_CMD_RDD_COLMOD, GPIO_PULLDOWN_ONLY);
    ESP_LOGI(TAG, "After native init: RDDPM=%02x RDDMADCTL=%02x RDDCOLMOD=%02x", power, madctl, colmod);
    uint8_t id[4], readback[1 + PROBE_READ_BYTES];
    read_register(rd, 0xd3, GPIO_PULLDOWN_ONLY, id, sizeof(id) - 1);
    // ILI9341 section 8.3.23: dummy, IC version, model 0x93 0x41. Requires EXTC high.
    ESP_LOGI(TAG, "RDID4 (pull-down): %02x %02x %02x", id[1], id[2], id[3]);
    read_register(rd, 0x2e, GPIO_PULLDOWN_ONLY, readback, PROBE_READ_BYTES);
    const size_t matches = compare_pixels(readback + 1);
    if (matches == PROBE_PIXELS) {
        ESP_LOGI(TAG, "GRAM PASS: %u/%d pixels match", (unsigned)matches, PROBE_PIXELS);
#if CONFIG_EXAMPLE_I80_FULL_SCREEN_DEMO
        measure_panel_scan(rd);
#endif
    } else {
        const bool zero_with_pull_down = all_bytes_are(readback + 1, PROBE_READ_BYTES, 0);
        ESP_LOGE(TAG, "GRAM FAIL: %u/%d pixels match", (unsigned)matches, PROBE_PIXELS);
        read_register(rd, 0xd3, GPIO_PULLUP_ONLY, id, sizeof(id) - 1);
        ESP_LOGI(TAG, "RDID4 (pull-up): %02x %02x %02x", id[1], id[2], id[3]);
        read_register(rd, 0x2e, GPIO_PULLUP_ONLY, readback, PROBE_READ_BYTES);
        ESP_LOGI(TAG, "RAMRD first pixel (pull-up): %02x %02x %02x",
                 readback[1], readback[2], readback[3]);
        if (zero_with_pull_down && all_bytes_are(readback + 1, PROBE_READ_BYTES, 0xff)) {
            ESP_LOGE(TAG, "GRAM data follows both GPIO pulls; no LCD read drive detected");
        }
        check_read_drive(rd);
        check_panel_registers(rd);
        probe_gpio_write(rd, readback);
    }

    set_data_bus_direction(GPIO_MODE_INPUT, GPIO_FLOATING);
}
