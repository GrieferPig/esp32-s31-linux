#include <inttypes.h>
#include <stdbool.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "driver/uart.h"
#include "esp_check.h"
#include "esp_err.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/task.h"

#define BENCH_UART          UART_NUM_0
#define BENCH_TX_GPIO       58
#define BENCH_RX_GPIO       59
#define BENCH_BAUD          2000000
#define TX_TEST_BYTES       (2U * 1024U * 1024U)
#define RX_TEST_BYTES       (1U * 1024U * 1024U)
#define UART_RX_RING_BYTES  (64U * 1024U)
#define DATA_CHUNK_BYTES    4096U
#define TEST_SEED           UINT64_C(0x123456789abcdef0)

static uint8_t data_buf[DATA_CHUNK_BYTES];

typedef struct {
    int64_t start_us;
    uint64_t idle_start;
} cpu_sample_t;

static uint16_t crc16_ccitt(uint16_t crc, const uint8_t *data, size_t len)
{
    while (len--) {
        crc ^= (uint16_t)*data++ << 8;
        for (unsigned bit = 0; bit < 8; ++bit) {
            crc = (crc & 0x8000U) ? (uint16_t)((crc << 1) ^ 0x1021U)
                                  : (uint16_t)(crc << 1);
        }
    }
    return crc;
}

static uint64_t xorshift64(uint64_t *state)
{
    uint64_t value = *state;
    value ^= value << 13;
    value ^= value >> 7;
    value ^= value << 17;
    *state = value;
    return value;
}

static void fill_test_data(uint8_t *data, size_t len, uint64_t *state)
{
    while (len != 0) {
        uint64_t value = xorshift64(state);
        size_t count = len < sizeof(value) ? len : sizeof(value);
        for (size_t i = 0; i < count; ++i) {
            data[i] = (uint8_t)(value >> (i * 8));
        }
        data += count;
        len -= count;
    }
}

static void uart_printf(const char *format, ...)
{
    char line[192];
    va_list args;
    va_start(args, format);
    int len = vsnprintf(line, sizeof(line), format, args);
    va_end(args);
    if (len > 0) {
        size_t out_len = (size_t)len < sizeof(line) ? (size_t)len : sizeof(line) - 1;
        uart_write_bytes(BENCH_UART, line, out_len);
    }
}

static cpu_sample_t cpu_sample_begin(void)
{
    cpu_sample_t sample = {
        .start_us = esp_timer_get_time(),
        .idle_start = (uint64_t)ulTaskGetIdleRunTimeCounterForCore(0),
    };
    return sample;
}

static double cpu_sample_end(cpu_sample_t sample, int64_t *elapsed_us)
{
    uint64_t idle_end = (uint64_t)ulTaskGetIdleRunTimeCounterForCore(0);
    *elapsed_us = esp_timer_get_time() - sample.start_us;
    uint64_t idle_us = idle_end - sample.idle_start;
    if (*elapsed_us <= 0) {
        return 0.0;
    }
    double busy = 100.0 * (1.0 - (double)idle_us / (double)*elapsed_us);
    if (busy < 0.0) {
        busy = 0.0;
    } else if (busy > 100.0) {
        busy = 100.0;
    }
    return busy;
}

static void run_tx_test(void)
{
    uint64_t state = TEST_SEED;
    uint16_t crc = 0;

    /* Compute the expected CRC before the timed section. */
    for (size_t offset = 0; offset < TX_TEST_BYTES; offset += sizeof(data_buf)) {
        size_t count = TX_TEST_BYTES - offset;
        if (count > sizeof(data_buf)) {
            count = sizeof(data_buf);
        }
        fill_test_data(data_buf, count, &state);
        crc = crc16_ccitt(crc, data_buf, count);
    }

    uart_printf("TXR CRC=%04x bytes=%u\n", crc, (unsigned)TX_TEST_BYTES);
    uart_wait_tx_done(BENCH_UART, portMAX_DELAY);
    vTaskDelay(pdMS_TO_TICKS(200));

    state = TEST_SEED;
    cpu_sample_t sample = cpu_sample_begin();
    for (size_t offset = 0; offset < TX_TEST_BYTES; offset += sizeof(data_buf)) {
        size_t count = TX_TEST_BYTES - offset;
        if (count > sizeof(data_buf)) {
            count = sizeof(data_buf);
        }
        fill_test_data(data_buf, count, &state);
        if (uart_write_bytes(BENCH_UART, data_buf, count) != (int)count) {
            uart_printf("TXR ERROR offset=%u\n", (unsigned)offset);
            return;
        }
    }
    uart_wait_tx_done(BENCH_UART, portMAX_DELAY);

    int64_t elapsed_us;
    double cpu = cpu_sample_end(sample, &elapsed_us);
    double speed = (double)TX_TEST_BYTES * 1000000.0 / (double)elapsed_us;
    uart_printf("TXR DONE bytes=%u time_us=%" PRId64 " speed=%.1f cpu=%.2f%%\n",
                (unsigned)TX_TEST_BYTES, elapsed_us, speed, cpu);
}

static void run_rx_test(size_t expected)
{
    uint16_t crc = 0;
    size_t received = 0;

    uart_flush_input(BENCH_UART);
    uart_printf("RXR READY bytes=%u\n", (unsigned)expected);
    uart_wait_tx_done(BENCH_UART, portMAX_DELAY);
    cpu_sample_t sample = cpu_sample_begin();

    while (received < expected) {
        size_t want = expected - received;
        if (want > sizeof(data_buf)) {
            want = sizeof(data_buf);
        }
        int count = uart_read_bytes(BENCH_UART, data_buf, want, pdMS_TO_TICKS(3000));
        if (count <= 0) {
            uart_printf("RXR ERROR timeout received=%u\n", (unsigned)received);
            return;
        }
        crc = crc16_ccitt(crc, data_buf, (size_t)count);
        received += (size_t)count;
    }

    int64_t elapsed_us;
    double cpu = cpu_sample_end(sample, &elapsed_us);
    double speed = (double)received * 1000000.0 / (double)elapsed_us;
    uart_printf("RXR DONE bytes=%u crc=%04x time_us=%" PRId64
                " speed=%.1f cpu=%.2f%%\n",
                (unsigned)received, crc, elapsed_us, speed, cpu);
}

static int read_command(char *line, size_t capacity)
{
    size_t used = 0;
    while (used + 1 < capacity) {
        uint8_t ch;
        int count = uart_read_bytes(BENCH_UART, &ch, 1, portMAX_DELAY);
        if (count != 1) {
            continue;
        }
        if (ch == '\n' || ch == '\r') {
            if (used == 0) {
                continue;
            }
            line[used] = '\0';
            return (int)used;
        }
        line[used++] = (char)ch;
    }
    line[used] = '\0';
    return (int)used;
}

void app_main(void)
{
    const uart_config_t config = {
        .baud_rate = BENCH_BAUD,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };

    if (uart_is_driver_installed(BENCH_UART)) {
        uart_driver_delete(BENCH_UART);
    }
    /* ESP-IDF's documented blocking TX path: zero means no software TX ring. */
    ESP_ERROR_CHECK(uart_driver_install(BENCH_UART, UART_RX_RING_BYTES,
                                        0, 0, NULL, 0));
    ESP_ERROR_CHECK(uart_param_config(BENCH_UART, &config));
    ESP_ERROR_CHECK(uart_set_pin(BENCH_UART, BENCH_TX_GPIO, BENCH_RX_GPIO,
                                 UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));
    uart_flush_input(BENCH_UART);

    char command[64];
    while (true) {
        uart_printf("IDF UART BENCH READY baud=%d\n", BENCH_BAUD);
        uart_wait_tx_done(BENCH_UART, portMAX_DELAY);
        read_command(command, sizeof(command));
        if (strcmp(command, "tx") == 0) {
            run_tx_test();
        } else if (strncmp(command, "rx", 2) == 0) {
            unsigned requested = RX_TEST_BYTES;
            if (sscanf(command + 2, "%u", &requested) != 1) {
                requested = RX_TEST_BYTES;
            }
            if (requested == 0 || requested > 16U * 1024U * 1024U) {
                uart_printf("RXR ERROR invalid_size=%u\n", requested);
            } else {
                run_rx_test(requested);
            }
        } else {
            uart_printf("ERROR unknown_command=%s\n", command);
        }
        uart_wait_tx_done(BENCH_UART, portMAX_DELAY);
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}
