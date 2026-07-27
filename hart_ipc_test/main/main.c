/*
 * ESP32-S31 跨Hart IPC双向测试 (esp-hosted transport基础)
 * 
 * Test 1: Core1启动
 * Test 2: 共享SRAM轮询 (Core0->Core1)
 * Test 3: Core0->Core1 硬件中断 (HP_SYSTEM FROM_CPU_0)
 * Test 4: Core1->Core0 硬件中断 (HP_SYSTEM FROM_CPU_1)  ← NEW
 */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <inttypes.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_cpu.h"
#include "esp_system.h"
#include "esp_rom_sys.h"

#include "esp32s31/rom/ets_sys.h"
#include "riscv/csr.h"

#include "soc/soc.h"
#include "soc/clic_reg.h"
#include "soc/hp_system_reg.h"
#include "soc/interrupt_core0_reg.h"
#include "hal/cpu_utility_ll.h"
#include "hal/assist_debug_ll.h"

#include "s31_hosted_sram.h"

static const char *TAG = "hart_ipc";

/* ---- 共享内存布局 ---- */
#define SHARED_BASE              S31_HOSTED_SRAM_BASE
#define SHARED_MAGIC_OFFSET      0x000
#define SHARED_H1_IRQ_ACK_OFF    0x100
#define SHARED_H1_SEND_IRQ_CNT   0x108   /* Core1发送中断计数 */
#define SHARED_MSG_REQ_FLAG      0x30    /* Core0消息请求flag */
#define SHARED_MSG_H0_TO_H1      0x200   /* Core0→Core1消息 (64B) */
#define SHARED_MSG_H1_TO_H0      0x240   /* Core1→Core0响应 (64B) */
#define SHARED_MSG_RESP_FLAG     0x2C0   /* Core1响应就绪flag */
#define SHARED_MAGIC_VAL         0xDEADBEEF

/* ---- 中断触发寄存器 ---- */
#define TRIG_HART1()  REG_WRITE(HP_SYSTEM_CPU_INT_FROM_CPU_0_REG, 1)

/* ---- Core0中断配置 (接收Core1发来的FROM_CPU_1) ---- */
#define H0_IPC_CLIC_ID           29      /* Core0用CLIC ID 29接收FROM_CPU_1 */

extern void core1_entry(void);
volatile uint32_t g_core1_trap_mcause;
volatile uint32_t g_core1_trap_mepc;
volatile uint32_t g_core1_trap_mtval;

/* Core0中断计数 */
static volatile uint32_t g_h0_irq_count;

static inline void shared_write(uint32_t off, uint32_t val) {
    *(volatile uint32_t *)(SHARED_BASE + off) = val;
    __asm__ volatile ("fence rw, rw" ::: "memory");
}
static inline uint32_t shared_read(uint32_t off) {
    uint32_t v = *(volatile uint32_t *)(SHARED_BASE + off);
    __asm__ volatile ("fence rw, rw" ::: "memory");
    return v;
}

static bool g_core1_started;

static void prepare_core1(void)
{
    assist_debug_ll_sp_spill_monitor_disable(1);
    assist_debug_ll_sp_spill_interrupt_disable(1);
    assist_debug_ll_sp_spill_set_min(1, 0);
    assist_debug_ll_sp_spill_set_max(1, 0xffffffff);
}

/* ---- Test 4: 通过轮询Core1的发送计数器来验证Core1→Core0中断发送 ---- */
/* 注：Core0不配置CLIC handler（避免与IDF vector_table冲突），
 * 只验证Core1能否成功写HP_SYSTEM_CPU_INT_FROM_CPU_1_REG */

void app_main(void)
{
    int tests_passed = 0, tests_total = 0;

    ESP_LOGI(TAG, "=== ESP32-S31 Hart IPC Bidirectional Test ===");
    memset((void *)SHARED_BASE, 0, 0x1000);

    g_core1_started = false;
    g_core1_trap_mcause = 0;
    g_core1_trap_mepc = 0;
    g_core1_trap_mtval = 0;
    g_h0_irq_count = 0;

    prepare_core1();

    /* ---- 启动Core1 ---- */
    esp_cpu_stall(1);
    cpu_utility_ll_enable_clock_and_reset_app_cpu();
    cpu_utility_ll_enable_clock_and_reset_app_cpu_int_matrix();
    ESP_LOGI(TAG, "Core1 boot addr: %p", &core1_entry);
    ets_set_appcpu_boot_addr((uint32_t)(uintptr_t)core1_entry);
    __asm__ volatile ("fence.i" ::: "memory");
    esp_cpu_reset(1);
    esp_cpu_unstall(1);

    /* 等待Core1就绪 */
    for (int i = 0; i < 2000; i++) {
        if (shared_read(SHARED_MAGIC_OFFSET) == SHARED_MAGIC_VAL) {
            g_core1_started = true;
            break;
        }
        if (g_core1_trap_mcause) break;
        esp_rom_delay_us(500);
    }

    /* Test 1: Core1启动 */
    tests_total++;
    if (g_core1_started) {
        ESP_LOGI(TAG, "[PASS] Test 1: Core1 started (magic=0x%08" PRIx32 ")",
                 shared_read(SHARED_MAGIC_OFFSET));
        tests_passed++;
    } else {
        ESP_LOGE(TAG, "[FAIL] Test 1: Core1 start failed (magic=0x%08" PRIx32
                 " trap=0x%08" PRIx32 ")",
                 shared_read(SHARED_MAGIC_OFFSET), g_core1_trap_mcause);
        while (1) vTaskDelay(pdMS_TO_TICKS(5000));
    }

    /* Core1诊断 */
    ESP_LOGI(TAG, "Core1: CLIC28=0x%08" PRIx32 " mtvec=0x%08" PRIx32
             " INTR_MAP=0x%08" PRIx32,
             shared_read(0x10), shared_read(0x18), shared_read(0x1C));

    /* Test 2: 共享SRAM轮询 */
    tests_total++;
    ESP_LOGI(TAG, "Test 2: Shared SRAM polling (Core0->Core1)...");
    uint32_t before = shared_read(SHARED_H1_IRQ_ACK_OFF);
    for (int i = 0; i < 5; i++) {
        shared_write(0x20, 0xDEAD);
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    uint32_t delta = shared_read(SHARED_H1_IRQ_ACK_OFF) - before;
    if (delta >= 3) {
        ESP_LOGI(TAG, "[PASS] Test 2: SRAM polling (delta=%" PRIu32 ")", delta);
        tests_passed++;
    } else {
        ESP_LOGE(TAG, "[FAIL] Test 2: SRAM polling (delta=%" PRIu32 ")", delta);
    }

    /* Test 3: 硬件中断 Core0->Core1 */
    tests_total++;
    ESP_LOGI(TAG, "Test 3: IRQ Core0->Core1 (HP_SYS FROM_CPU_0)...");
    before = shared_read(SHARED_H1_IRQ_ACK_OFF);
    for (int i = 0; i < 5; i++) {
        TRIG_HART1();
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    delta = shared_read(SHARED_H1_IRQ_ACK_OFF) - before;
    if (delta >= 3) {
        ESP_LOGI(TAG, "[PASS] Test 3: Core0->Core1 IRQ (delta=%" PRIu32 ")", delta);
        tests_passed++;
    } else {
        ESP_LOGE(TAG, "[FAIL] Test 3: Core0->Core1 IRQ (delta=%" PRIu32 ")", delta);
    }

    /* Test 4: Core1→Core0中断发送验证 (Core1写FROM_CPU_1寄存器) */
    tests_total++;
    ESP_LOGI(TAG, "Test 4: Core1->Core0 IRQ send (HP_SYS FROM_CPU_1)...");
    uint32_t before_send = shared_read(SHARED_H1_SEND_IRQ_CNT);
    for (int i = 0; i < 5; i++) {
        shared_write(0x24, 0xBEEF);  /* 请求Core1发IRQ给Core0 */
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    uint32_t h1_sent = shared_read(SHARED_H1_SEND_IRQ_CNT) - before_send;
    /* 也读HP_SYSTEM FROM_CPU_1_REG确认 */
    uint32_t from_cpu1_val = REG_READ(HP_SYSTEM_CPU_INT_FROM_CPU_1_REG);
    ESP_LOGI(TAG, "Test 4: Core1 sent=%" PRIu32 " FROM_CPU_1_REG=0x%08" PRIx32,
             h1_sent, from_cpu1_val);

    if (h1_sent >= 3) {
        ESP_LOGI(TAG, "[PASS] Test 4: Core1->Core0 IRQ send (sent=%" PRIu32 ")", h1_sent);
        tests_passed++;
    } else {
        ESP_LOGE(TAG, "[FAIL] Test 4: Core1->Core0 IRQ send (sent=%" PRIu32 ")", h1_sent);
    }

    /* Test 5: Ping-Pong (双向中断 + 共享SRAM消息交换) */
    tests_total++;
    ESP_LOGI(TAG, "Test 5: Ping-Pong message exchange...");

    const char *ping_msgs[] = {
        "Hello 1 from Hart0!",
        "Hello 2 from Hart0!",
        "Hello 3 from Hart0!",
        "Hello 4 from Hart0!",
        "Hello 5 from Hart0!",
    };
    int ping_ok = 0;

    for (int i = 0; i < 5; i++) {
        /* 清零响应flag */
        shared_write(SHARED_MSG_RESP_FLAG, 0);

        /* 写消息到共享SRAM */
        volatile char *msg_dst = (volatile char *)(SHARED_BASE + SHARED_MSG_H0_TO_H1);
        strncpy((char *)msg_dst, ping_msgs[i], 64);

        /* 设置请求flag并触发Core1中断 */
        shared_write(SHARED_MSG_REQ_FLAG, 0xBEEF);
        TRIG_HART1();

        /* 轮询等待Core1响应 (Core0不设CLIC handler, 用轮询) */
        bool got_response = false;
        for (int j = 0; j < 50; j++) {
            uint32_t resp_flag = shared_read(SHARED_MSG_RESP_FLAG);
            if (resp_flag == 0xCAFE) {
                got_response = true;
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(20));
        }

        /* 读取响应 */
        volatile char *resp_src = (volatile char *)(SHARED_BASE + SHARED_MSG_H1_TO_H0);
        char response[65] = {0};
        for (int k = 0; k < 64; k++) response[k] = resp_src[k];

        if (got_response) {
            ESP_LOGI(TAG, "Ping #%d: got response='%s'", i+1, response);
            ping_ok++;
        } else {
            ESP_LOGW(TAG, "Ping #%d: timeout (resp_flag=0x%08" PRIx32 ")",
                     i+1, shared_read(SHARED_MSG_RESP_FLAG));
        }
    }

    if (ping_ok >= 5) {
        ESP_LOGI(TAG, "[PASS] Test 5: Ping-Pong (5/5 rounds OK)");
        tests_passed++;
    } else {
        ESP_LOGE(TAG, "[FAIL] Test 5: Ping-Pong (%d/5 rounds OK)", ping_ok);
    }

    /* 总结 */
    ESP_LOGI(TAG, "========================================");
    ESP_LOGI(TAG, "RESULTS: %d/%d tests passed", tests_passed, tests_total);
    if (tests_passed == tests_total) {
        ESP_LOGI(TAG, "ALL TESTS PASSED - Bidirectional Hart IPC ready!");
    } else {
        ESP_LOGW(TAG, "%d tests FAILED", tests_total - tests_passed);
    }
    ESP_LOGI(TAG, "========================================");

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(5000));
        ESP_LOGI(TAG, "Alive: h0_irq=%" PRIu32 " h1_ack=%" PRIu32
                 " h1_sent=%" PRIu32,
                 g_h0_irq_count,
                 shared_read(SHARED_H1_IRQ_ACK_OFF),
                 shared_read(SHARED_H1_SEND_IRQ_CNT));
    }
}
