/*
 * ESP32-P4 NOMMU Linux - Factory App Loader
 *
 * The second-stage bootloader stays minimal. This factory app runs with the
 * normal app startup path, maps OpenSBI for flash XIP, finds the appended FDT,
 * then jumps into OpenSBI in M-mode.
 */

#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <inttypes.h>
#include "sdkconfig.h"
#include "esp_log.h"
#include "esp_err.h"
#include "esp_system.h"
#include "esp_chip_info.h"
#include "esp_cpu.h"
#include "esp_flash.h"
#include "esp_partition.h"
#include "esp_cache.h"
#include "spi_flash_mmap.h"
#include "esp_psram.h"
#include "riscv/csr.h"
#include "esp32s31/rom/cache.h"
#include "hal/cache_ll.h"
#include "hal/assist_debug_ll.h"
#include "hal/wdt_hal.h"
#include "soc/soc.h"
#include "soc/timer_group_struct.h"
#include "soc/usb_serial_jtag_reg.h"
#include "soc/usb_serial_jtag_struct.h"
#include "soc/cpu_apm_reg.h"
#include "soc/hp_apm_reg.h"
#include "soc/hp_mem_apm_reg.h"
#include "hal/mmu_ll.h"
#include "hal/mmu_types.h"
#include "freertos/FreeRTOS.h"

#define PSRAM_BASE    ((uintptr_t)0xC0000000U) // must be uncached

typedef struct
{
    uint8_t magic[4];
    uint8_t total_size[4];
} fdt_header_t;


#define OPENSBI_XIP_ADDR              0x40030000U
#define OPENSBI_FDT_OFFSET_SLOT_SIZE  4U
#define FDT_MAGIC_LE                  0xEDFE0DD0U
#define HP_SRAM_LOW                   0x2F000000U
#define HP_SRAM_HIGH                  0x2F080000U
#define PSRAM_SIZE                    0x4000000U  /* 64 MB max mapping window */
#define FLASH_XIP_LOW                 0x40000000U
#define FLASH_XIP_HIGH                0x44000000U
#define OPENSBI_EARLY_DATA_LMA        0x40060F50U
#define OPENSBI_EARLY_DATA_START      0x2F040000U
#define OPENSBI_EARLY_DATA_END        0x2F042378U
#define OPENSBI_EARLY_BSS_START       0x2F043000U
#define OPENSBI_EARLY_BSS_END         0x2F043D18U
#define OPENSBI_EARLY_STACK_TOP       0x2F046000U
#define OPENSBI_EARLY_TRAP_MAILBOX    0x2F07E000U
#define OPENSBI_EARLY_TRAP_MAGIC      0x53335450U
#define INITRAMFS_LOAD_ADDR           0xC0800000U
#define INITRAMFS_PARTITION_SIZE      0x00200000U
#define INITRAMFS_NEWC_MAGIC0         0x37303730U /* "0707" */
#define PROBE_RESULT                  0x5AU
#define PROBE_WORD0                   0x05A00513U /* addi a0, zero, 0x5a */
#define PROBE_WORD1                   0x00008067U /* ret */
#define PROBE_SIZE                    32U
#define PMP_FULL_SPACE_NAPOT_ADDR     0x3FFFFFFFU
#define PMP_ENTRY_RWX_LOCK_NAPOT      0x9FU
#define TRAP_REDIRECT_ENTER_MAGIC     0x54524431U
#define TRAP_REDIRECT_RETURN_MAGIC    0x54524432U
#define TRAP_REDIRECT_EXPECTED_SCAUSE 0x00000002U
#define S_MODE_PROBE_RETURN_MAGIC     0x534d5031U
#define S_MODE_PROBE_RESULT_PASS      1U
#define S_MODE_PROBE_RESULT_TRAP      2U
#define S_MODE_PROBE_OP_BASELINE_ECALL            1U
#define S_MODE_PROBE_OP_FENCE_RW_RW               2U
#define S_MODE_PROBE_OP_READ_SATP                 3U
#define S_MODE_PROBE_OP_WRITE_SATP_ZERO           4U
#define S_MODE_PROBE_OP_ENABLE_MMU_FENCE_FENCE    5U
#define S_MODE_PROBE_OP_ENABLE_MMU_SFENCE_SFENCE  6U
#define S_MODE_PROBE_OP_SFENCE_ONLY               7U
#define S_MODE_PROBE_FINAL_OP         S_MODE_PROBE_OP_ENABLE_MMU_SFENCE_SFENCE
#define SV32_SATP_MODE                0x80000000U
#define SV32_PTE_V                    0x001U
#define SV32_PTE_R                    0x002U
#define SV32_PTE_W                    0x004U
#define SV32_PTE_X                    0x008U
#define SV32_PTE_G                    0x020U
#define SV32_PTE_A                    0x040U
#define SV32_PTE_D                    0x080U
#define SV32_PTE_RWXGAD              (SV32_PTE_V | SV32_PTE_R | SV32_PTE_W | \
                                      SV32_PTE_X | SV32_PTE_G | SV32_PTE_A | \
                                      SV32_PTE_D)
#define SV32_L1_MEGAPAGE_SHIFT        22U
#define SV32_ROOT_ENTRIES             1024U
#define S_MODE_PROBE_PSRAM_ROOT_ADDR  (PSRAM_BASE + 0x00FFE000U)

static const char *TAG = "boot";
static uint32_t sram_exec_probe_buf[PROBE_SIZE / sizeof(uint32_t)] __attribute__((aligned(PROBE_SIZE)));

typedef uint32_t (*probe_fn_t)(void);

volatile uint32_t m_resume_pc __attribute__((unused));
volatile uint32_t m_resume_sp __attribute__((unused));
volatile uint32_t test_s_mode_result __attribute__((unused)) = 0;
volatile uint32_t test_s_clicbase_val __attribute__((unused)) = 0xDEADBEEF;
static volatile bool s_mode_int_handled __attribute__((unused)) = false;
volatile uint32_t trap_redirect_resume_pc;
volatile uint32_t trap_redirect_resume_sp;
volatile uint32_t trap_redirect_stvec_programmed;
volatile uint32_t trap_redirect_stvec_readback;
volatile uint32_t trap_redirect_forwarded_scause;
volatile uint32_t trap_redirect_forwarded_sepc;
volatile uint32_t trap_redirect_forwarded_stval;
volatile uint32_t trap_redirect_s_scause;
volatile uint32_t trap_redirect_s_sepc;
volatile uint32_t trap_redirect_s_stval;
volatile uint32_t trap_redirect_s_stvec;
volatile uint32_t trap_redirect_result;
volatile uint32_t s_mode_probe_resume_pc;
volatile uint32_t s_mode_probe_resume_sp;
volatile uint32_t s_mode_probe_op;
volatile uint32_t s_mode_probe_requested_satp;
volatile uint32_t s_mode_probe_result;
volatile uint32_t s_mode_probe_stage;
volatile uint32_t s_mode_probe_return_a1;
volatile uint32_t s_mode_probe_mcause;
volatile uint32_t s_mode_probe_mepc;
volatile uint32_t s_mode_probe_mtval;
volatile uint32_t s_mode_probe_mstatus;
volatile uint32_t s_mode_probe_mtvec;
static uint32_t s_mode_sv32_root[SV32_ROOT_ENTRIES] __attribute__((aligned(4096)));

static void IRAM_ATTR __attribute__((interrupt("supervisor"), aligned(64), unused)) test_s_clic_handler(void)
{
    s_mode_int_handled = true;
    uint32_t sclicbase;
    __asm__ volatile ("csrr %0, 0x150" : "=r"(sclicbase));
    test_s_clicbase_val = sclicbase;
    
    volatile uint8_t *clicint1_ip = (volatile uint8_t *)(sclicbase + 0x1004);
    *clicint1_ip = 0;
}

IRAM_ATTR static void __attribute__((aligned(64), unused)) test_s_mode_entry(void)
{
    register uint32_t a0 __asm__("a0") = 99;
    register uint32_t a1 __asm__("a1") = s_mode_int_handled ? 1 : 0;
    __asm__ volatile ("ecall" :: "r"(a0), "r"(a1));
}

IRAM_ATTR static void __attribute__((naked, aligned(64), unused)) test_m_trap_handler(void)
{
    __asm__ volatile (
        "csrr t0, mcause\n\t"
        "li t1, 0xFFF\n\t"
        "and t0, t0, t1\n\t"
        "li t1, 9\n\t"
        "bne t0, t1, 1f\n\t"
        
        "li t1, 99\n\t"
        "bne a0, t1, 1f\n\t"
        "la t0, test_s_mode_result\n\t"
        "sw a1, 0(t0)\n\t"
        "la t0, m_resume_pc\n\t"
        "lw t0, 0(t0)\n\t"
        "csrw mepc, t0\n\t"
        "la t0, m_resume_sp\n\t"
        "lw sp, 0(t0)\n\t"
        "li t0, 0x1800\n\t"
        "csrs mstatus, t0\n\t"
        "mret\n\t"
        
        "1:\n\t"
        "j 1b\n\t"
    );
}

IRAM_ATTR static void __attribute__((unused)) run_s_mode_test(void) {
    ESP_LOGI(TAG, "Starting S-mode interrupt test...");

    // Configure CPU_APM to allow S-mode access
    REG_WRITE(CPU_APM_REGION0_ADDR_START_REG, 0);
    REG_WRITE(CPU_APM_REGION0_ADDR_END_REG, 0xFFFFFFFF);
    REG_WRITE(CPU_APM_REGION0_ATTR_REG, 0x7777);
    REG_WRITE(CPU_APM_REGION_FILTER_EN_REG, 1);
    REG_WRITE(CPU_APM_FUNC_CTRL_REG, 0x0F);

    __asm__ volatile ("csrw mtvec, %0" :: "r"(((uint32_t)(uintptr_t)test_m_trap_handler & ~0x3F)));

    volatile uint32_t *mcliccfg = (volatile uint32_t *)0x10800000;
    *mcliccfg = (*mcliccfg & ~(3U << 5)) | (1U << 5); 

    volatile uint8_t *clicint1_attr = (volatile uint8_t *)(0x10801006);
    volatile uint8_t *clicint1_ie   = (volatile uint8_t *)(0x10801005);
    volatile uint8_t *clicint1_ip   = (volatile uint8_t *)(0x10801004);
    volatile uint8_t *clicint1_ctrl = (volatile uint8_t *)(0x10801007);
    
    *clicint1_attr = 0x42; // Mode=1 (S-mode), Edge-triggered
    *clicint1_ctrl = 0xFF; // Max priority
    *clicint1_ie = 1;      // Enable
    *clicint1_ip = 1;      // Trigger

    __asm__ volatile ("csrw stvec, %0" :: "r"(((uint32_t)(uintptr_t)test_s_clic_handler & ~0x3F) | 3));

    uint32_t mstatus_val;
    __asm__ volatile ("csrr %0, mstatus" : "=r"(mstatus_val));
    mstatus_val |= (1 << 1);  // SIE = 1
    mstatus_val |= (1 << 5);  // SPIE = 1
    mstatus_val &= ~(3 << 11);
    mstatus_val |= (1 << 11); // MPP = S-mode
    
    __asm__ volatile (
        "csrw 0x347, zero\n\t"
        "csrw 0x147, zero\n\t"
    );

    __asm__ volatile (
        "la t0, m_resume_pc\n\t"
        "la t1, 1f\n\t"
        "sw t1, 0(t0)\n\t"
        "la t0, m_resume_sp\n\t"
        "sw sp, 0(t0)\n\t"
        "csrw mepc, %0\n\t"
        "csrw mstatus, %1\n\t"
        "mret\n\t"
        "1:\n\t"
        :: "r"(test_s_mode_entry), "r"(mstatus_val)
        : "t0", "t1", "memory"
    );

    ESP_LOGI(TAG, "S-mode interrupt test finished, result: %d", (int)test_s_mode_result);
    ESP_LOGI(TAG, "S-mode sclicbase read as: 0x%08" PRIx32, test_s_clicbase_val);
}

static void disable_test_s_mode_clic_interrupt(void)
{
    volatile uint8_t *clicint1_attr = (volatile uint8_t *)(0x10801006);
    volatile uint8_t *clicint1_ie   = (volatile uint8_t *)(0x10801005);
    volatile uint8_t *clicint1_ip   = (volatile uint8_t *)(0x10801004);
    volatile uint8_t *clicint1_ctrl = (volatile uint8_t *)(0x10801007);

    *clicint1_ie = 0;
    *clicint1_ip = 0;
    *clicint1_attr = 0;
    *clicint1_ctrl = 0;

    ESP_LOGI(TAG, "CLIC ID1 disabled before handoff: ip=0x%02x ie=0x%02x attr=0x%02x ctrl=0x%02x",
             *clicint1_ip, *clicint1_ie, *clicint1_attr, *clicint1_ctrl);
}

IRAM_ATTR static void __attribute__((noreturn, aligned(64))) test_s_trap_redirect_handler(void)
{
    uint32_t scause;
    uint32_t sepc;
    uint32_t stval;
    uint32_t stvec;
    uint32_t result;

    __asm__ volatile ("csrr %0, scause" : "=r"(scause));
    __asm__ volatile ("csrr %0, sepc" : "=r"(sepc));
    __asm__ volatile ("csrr %0, stval" : "=r"(stval));
    __asm__ volatile ("csrr %0, stvec" : "=r"(stvec));

    trap_redirect_s_scause = scause;
    trap_redirect_s_sepc = sepc;
    trap_redirect_s_stval = stval;
    trap_redirect_s_stvec = stvec;

    result = (scause == TRAP_REDIRECT_EXPECTED_SCAUSE && stval == 0U) ? 1U : 0U;

    __asm__ volatile (
        "mv a0, %0\n\t"
        "mv a1, %1\n\t"
        "ecall\n\t"
        :
        : "r"(TRAP_REDIRECT_RETURN_MAGIC), "r"(result)
        : "a0", "a1", "memory"
    );

    while (true) {
    }
}

IRAM_ATTR static void __attribute__((noreturn, aligned(64))) test_s_trap_redirect_entry(void)
{
    __asm__ volatile (
        "mv a0, %0\n\t"
        "ecall\n\t"
        :
        : "r"(TRAP_REDIRECT_ENTER_MAGIC)
        : "a0", "memory"
    );

    while (true) {
    }
}

IRAM_ATTR static void __attribute__((naked, aligned(64))) test_m_trap_redirect_handler(void)
{
    __asm__ volatile (
        "csrr t0, mcause\n\t"
        "li t1, 0xFFF\n\t"
        "and t0, t0, t1\n\t"
        "li t1, 9\n\t"
        "bne t0, t1, 3f\n\t"

        "li t1, %0\n\t"
        "beq a0, t1, 1f\n\t"
        "li t1, %1\n\t"
        "beq a0, t1, 2f\n\t"
        "j 3f\n\t"

        "1:\n\t"
        "csrr t2, mepc\n\t"
        "la t3, trap_redirect_forwarded_sepc\n\t"
        "sw t2, 0(t3)\n\t"
        "la t3, trap_redirect_forwarded_scause\n\t"
        "li t4, %2\n\t"
        "sw t4, 0(t3)\n\t"
        "la t3, trap_redirect_forwarded_stval\n\t"
        "sw zero, 0(t3)\n\t"
        "csrw sepc, t2\n\t"
        "csrw stval, zero\n\t"
        "li t4, %2\n\t"
        "csrw scause, t4\n\t"
        "csrr t5, stvec\n\t"
        "la t3, trap_redirect_stvec_readback\n\t"
        "sw t5, 0(t3)\n\t"
        "andi t5, t5, -4\n\t"
        "csrw mepc, t5\n\t"
        "csrr t3, mstatus\n\t"
        "li t4, ~(3 << 11)\n\t"
        "and t3, t3, t4\n\t"
        "li t4, (1 << 11)\n\t"
        "or t3, t3, t4\n\t"
        "csrw mstatus, t3\n\t"
        "mret\n\t"

        "2:\n\t"
        "la t0, trap_redirect_result\n\t"
        "sw a1, 0(t0)\n\t"
        "la t0, trap_redirect_resume_pc\n\t"
        "lw t0, 0(t0)\n\t"
        "csrw mepc, t0\n\t"
        "la t0, trap_redirect_resume_sp\n\t"
        "lw sp, 0(t0)\n\t"
        "li t0, 0x1800\n\t"
        "csrs mstatus, t0\n\t"
        "mret\n\t"

        "3:\n\t"
        "j 3b\n\t"
        :
        : "i"(TRAP_REDIRECT_ENTER_MAGIC),
          "i"(TRAP_REDIRECT_RETURN_MAGIC),
          "i"(TRAP_REDIRECT_EXPECTED_SCAUSE)
    );
}

static bool run_trap_redirect_test(void)
{
    uint32_t stvec_value =
        (((uint32_t)(uintptr_t)test_s_trap_redirect_handler) & ~0x3FU) | 3U;
    uint32_t stvec_readback = 0;
    uint32_t mtvec_value =
        ((uint32_t)(uintptr_t)test_m_trap_redirect_handler) & ~0x3FU;
    uint32_t mstatus_val;
    bool pass;

    trap_redirect_stvec_programmed = stvec_value;
    trap_redirect_stvec_readback = 0;
    trap_redirect_forwarded_scause = 0;
    trap_redirect_forwarded_sepc = 0;
    trap_redirect_forwarded_stval = 0;
    trap_redirect_s_scause = 0;
    trap_redirect_s_sepc = 0;
    trap_redirect_s_stval = 0;
    trap_redirect_s_stvec = 0;
    trap_redirect_result = 0;

    __asm__ volatile ("csrw stvec, %0" :: "r"(stvec_value));
    __asm__ volatile ("csrr %0, stvec" : "=r"(stvec_readback));
    trap_redirect_stvec_readback = stvec_readback;

    ESP_LOGI(TAG, "trap redirect test: programmed stvec=0x%08" PRIx32
                  " readback=0x%08" PRIx32,
             stvec_value, stvec_readback);

    __asm__ volatile ("csrw mtvec, %0" :: "r"(mtvec_value));
    __asm__ volatile ("csrr %0, mstatus" : "=r"(mstatus_val));
    mstatus_val &= ~(3U << 11);
    mstatus_val |= (1U << 11); /* MPP = S-mode */

    __asm__ volatile (
        "la t0, trap_redirect_resume_pc\n\t"
        "la t1, 1f\n\t"
        "sw t1, 0(t0)\n\t"
        "la t0, trap_redirect_resume_sp\n\t"
        "sw sp, 0(t0)\n\t"
        "csrw mepc, %0\n\t"
        "csrw mstatus, %1\n\t"
        "mret\n\t"
        "1:\n\t"
        :
        : "r"(test_s_trap_redirect_entry), "r"(mstatus_val)
        : "t0", "t1", "memory"
    );

    pass = (trap_redirect_result == 1U) &&
           (trap_redirect_stvec_readback == trap_redirect_stvec_programmed) &&
           (trap_redirect_forwarded_scause == TRAP_REDIRECT_EXPECTED_SCAUSE) &&
           (trap_redirect_s_scause == TRAP_REDIRECT_EXPECTED_SCAUSE) &&
           (trap_redirect_s_stval == 0U);

    ESP_LOGI(TAG, "trap redirect test result=%" PRIu32
                  " fwd(scause=0x%08" PRIx32 " sepc=0x%08" PRIx32
                  " stval=0x%08" PRIx32 ")"
                  " s(scause=0x%08" PRIx32 " sepc=0x%08" PRIx32
                  " stval=0x%08" PRIx32 " stvec=0x%08" PRIx32 ")",
             trap_redirect_result,
             trap_redirect_forwarded_scause, trap_redirect_forwarded_sepc,
             trap_redirect_forwarded_stval,
             trap_redirect_s_scause, trap_redirect_s_sepc,
             trap_redirect_s_stval, trap_redirect_s_stvec);

    if (!pass) {
        ESP_LOGE(TAG, "trap redirect test FAILED");
    } else {
        ESP_LOGI(TAG, "trap redirect test PASSED");
    }

    return pass;
}

static bool s_mode_probe_addr_in_hp_sram(uintptr_t addr, size_t size)
{
    return addr >= HP_SRAM_LOW &&
           addr + size >= addr &&
           addr + size <= HP_SRAM_HIGH;
}

static void clear_s_mode_probe_state(void)
{
    s_mode_probe_op = 0;
    s_mode_probe_requested_satp = 0;
    s_mode_probe_result = 0;
    s_mode_probe_stage = 0;
    s_mode_probe_return_a1 = 0;
    s_mode_probe_mcause = 0;
    s_mode_probe_mepc = 0;
    s_mode_probe_mtval = 0;
    s_mode_probe_mstatus = 0;
    s_mode_probe_mtvec = 0;
}

static bool prepare_s_mode_sv32_identity_root_at(uintptr_t root_addr,
                                                 bool writeback,
                                                 const char *name,
                                                 uint32_t *satp_value)
{
    const uintptr_t mega_base = HP_SRAM_LOW & ~((1UL << SV32_L1_MEGAPAGE_SHIFT) - 1U);
    const uint32_t root_index = (uint32_t)(mega_base >> SV32_L1_MEGAPAGE_SHIFT);
    const uint32_t leaf_pte =
        (uint32_t)(((mega_base >> 12U) << 10U) | SV32_PTE_RWXGAD);
    uint32_t *root = (uint32_t *)root_addr;

    memset(root, 0, sizeof(s_mode_sv32_root));
    root[root_index] = leaf_pte;

    if (writeback) {
        int ret = Cache_WriteBack_Addr(CACHE_MAP_L1_DCACHE, root_addr,
                                       sizeof(s_mode_sv32_root));
        if (ret != 0) {
            ESP_LOGE(TAG, "S-mode Sv32 %s root writeback failed: %d",
                     name, ret);
            return false;
        }
    }

    *satp_value = SV32_SATP_MODE | ((uint32_t)root_addr >> 12U);

    ESP_LOGI(TAG, "S-mode Sv32 %s root: root=0x%08" PRIx32
                  " satp=0x%08" PRIx32 " l1[%u]=0x%08" PRIx32
                  " writeback=%d",
             name, (uint32_t)root_addr, *satp_value, root_index, leaf_pte,
             writeback ? 1 : 0);
    return true;
}

static bool prepare_s_mode_sv32_identity_root_hpsram(uint32_t *satp_value)
{
    const uintptr_t root_addr = (uintptr_t)s_mode_sv32_root;

    if (!s_mode_probe_addr_in_hp_sram(root_addr, sizeof(s_mode_sv32_root))) {
        ESP_LOGE(TAG, "S-mode Sv32 HP SRAM root table outside HP SRAM: 0x%08" PRIx32,
                 (uint32_t)root_addr);
        return false;
    }

    return prepare_s_mode_sv32_identity_root_at(root_addr, false, "HP SRAM", satp_value);
}

static bool prepare_s_mode_sv32_identity_root_psram(bool writeback, uint32_t *satp_value)
{
    if (!esp_psram_is_initialized()) {
        ESP_LOGE(TAG, "S-mode Sv32 PSRAM root requested before PSRAM init");
        return false;
    }

    return prepare_s_mode_sv32_identity_root_at(S_MODE_PROBE_PSRAM_ROOT_ADDR,
                                                writeback,
                                                writeback ? "PSRAM+C2M" : "PSRAM",
                                                satp_value);
}

IRAM_ATTR static void __attribute__((naked, aligned(64))) s_mode_probe_entry(void)
{
    __asm__ volatile (
        "la t0, s_mode_probe_stage\n\t"
        "li t1, 1\n\t"
        "sw t1, 0(t0)\n\t"
        "la t0, s_mode_probe_op\n\t"
        "lw t0, 0(t0)\n\t"

        "li t1, %0\n\t"
        "beq t0, t1, 1f\n\t"
        "li t1, %1\n\t"
        "beq t0, t1, 2f\n\t"
        "li t1, %2\n\t"
        "beq t0, t1, 3f\n\t"
        "li t1, %3\n\t"
        "beq t0, t1, 4f\n\t"
        "li t1, %4\n\t"
        "beq t0, t1, 5f\n\t"
        "li t1, %5\n\t"
        "beq t0, t1, 6f\n\t"
        "li t1, %6\n\t"
        "beq t0, t1, 7f\n\t"
        "ebreak\n\t"

        "1:\n\t"
        "la t0, s_mode_probe_stage\n\t"
        "li t1, 2\n\t"
        "sw t1, 0(t0)\n\t"
        "li a0, %7\n\t"
        "li a1, 0\n\t"
        "ecall\n\t"

        "2:\n\t"
        "la t0, s_mode_probe_stage\n\t"
        "li t1, 2\n\t"
        "sw t1, 0(t0)\n\t"
        "fence rw, rw\n\t"
        "li t1, 3\n\t"
        "sw t1, 0(t0)\n\t"
        "li a0, %7\n\t"
        "li a1, 0\n\t"
        "ecall\n\t"

        "3:\n\t"
        "la t0, s_mode_probe_stage\n\t"
        "li t1, 2\n\t"
        "sw t1, 0(t0)\n\t"
        "csrr a1, satp\n\t"
        "li t1, 3\n\t"
        "sw t1, 0(t0)\n\t"
        "li a0, %7\n\t"
        "ecall\n\t"

        "4:\n\t"
        "la t0, s_mode_probe_stage\n\t"
        "li t1, 2\n\t"
        "sw t1, 0(t0)\n\t"
        "csrw satp, zero\n\t"
        "li t1, 3\n\t"
        "sw t1, 0(t0)\n\t"
        "csrr a1, satp\n\t"
        "li t1, 4\n\t"
        "sw t1, 0(t0)\n\t"
        "li a0, %7\n\t"
        "ecall\n\t"

        "5:\n\t"
        "la t0, s_mode_probe_requested_satp\n\t"
        "lw a2, 0(t0)\n\t"
        "la t0, s_mode_probe_stage\n\t"
        "li t1, 2\n\t"
        "sw t1, 0(t0)\n\t"
        "fence rw, rw\n\t"
        "li t1, 3\n\t"
        "sw t1, 0(t0)\n\t"
        "csrw satp, a2\n\t"
        "li t1, 4\n\t"
        "sw t1, 0(t0)\n\t"
        "fence rw, rw\n\t"
        "li t1, 5\n\t"
        "sw t1, 0(t0)\n\t"
        "csrr a1, satp\n\t"
        "li t1, 6\n\t"
        "sw t1, 0(t0)\n\t"
        "li a0, %7\n\t"
        "ecall\n\t"

        "6:\n\t"
        "la t0, s_mode_probe_requested_satp\n\t"
        "lw a2, 0(t0)\n\t"
        "la t0, s_mode_probe_stage\n\t"
        "li t1, 2\n\t"
        "sw t1, 0(t0)\n\t"
        "sfence.vma zero, zero\n\t"
        "li t1, 3\n\t"
        "sw t1, 0(t0)\n\t"
        "csrw satp, a2\n\t"
        "li t1, 4\n\t"
        "sw t1, 0(t0)\n\t"
        "sfence.vma zero, zero\n\t"
        "li t1, 5\n\t"
        "sw t1, 0(t0)\n\t"
        "csrr a1, satp\n\t"
        "li t1, 6\n\t"
        "sw t1, 0(t0)\n\t"
        "li a0, %7\n\t"
        "ecall\n\t"

        "7:\n\t"
        "la t0, s_mode_probe_stage\n\t"
        "li t1, 2\n\t"
        "sw t1, 0(t0)\n\t"
        "sfence.vma zero, zero\n\t"
        "li t1, 3\n\t"
        "sw t1, 0(t0)\n\t"
        "li a0, %7\n\t"
        "li a1, 0\n\t"
        "ecall\n\t"
        :
        : "i"(S_MODE_PROBE_OP_BASELINE_ECALL),
          "i"(S_MODE_PROBE_OP_FENCE_RW_RW),
          "i"(S_MODE_PROBE_OP_READ_SATP),
          "i"(S_MODE_PROBE_OP_WRITE_SATP_ZERO),
          "i"(S_MODE_PROBE_OP_ENABLE_MMU_FENCE_FENCE),
          "i"(S_MODE_PROBE_OP_ENABLE_MMU_SFENCE_SFENCE),
          "i"(S_MODE_PROBE_OP_SFENCE_ONLY),
          "i"(S_MODE_PROBE_RETURN_MAGIC)
    );
}

IRAM_ATTR static void __attribute__((naked, aligned(64))) s_mode_probe_trap_handler(void)
{
    __asm__ volatile (
        "csrr t0, mcause\n\t"
        "li t1, 0xFFF\n\t"
        "and t2, t0, t1\n\t"
        "li t1, 9\n\t"
        "bne t2, t1, 1f\n\t"
        "li t1, %0\n\t"
        "bne a0, t1, 1f\n\t"
        "la t3, s_mode_probe_result\n\t"
        "li t4, %1\n\t"
        "sw t4, 0(t3)\n\t"
        "la t3, s_mode_probe_return_a1\n\t"
        "sw a1, 0(t3)\n\t"
        "j 2f\n\t"

        "1:\n\t"
        "la t3, s_mode_probe_result\n\t"
        "li t4, %2\n\t"
        "sw t4, 0(t3)\n\t"
        "la t3, s_mode_probe_mcause\n\t"
        "sw t0, 0(t3)\n\t"
        "csrr t4, mepc\n\t"
        "la t3, s_mode_probe_mepc\n\t"
        "sw t4, 0(t3)\n\t"
        "csrr t4, mtval\n\t"
        "la t3, s_mode_probe_mtval\n\t"
        "sw t4, 0(t3)\n\t"
        "csrr t4, mstatus\n\t"
        "la t3, s_mode_probe_mstatus\n\t"
        "sw t4, 0(t3)\n\t"
        "csrr t4, mtvec\n\t"
        "la t3, s_mode_probe_mtvec\n\t"
        "sw t4, 0(t3)\n\t"

        "2:\n\t"
        "la t3, s_mode_probe_resume_pc\n\t"
        "lw t4, 0(t3)\n\t"
        "csrw mepc, t4\n\t"
        "la t3, s_mode_probe_resume_sp\n\t"
        "lw sp, 0(t3)\n\t"
        "li t3, 0x1800\n\t"
        "csrs mstatus, t3\n\t"
        "mret\n\t"
        :
        : "i"(S_MODE_PROBE_RETURN_MAGIC),
          "i"(S_MODE_PROBE_RESULT_PASS),
          "i"(S_MODE_PROBE_RESULT_TRAP)
    );
}

static bool run_s_mode_probe_case(const char *name, uint32_t op, uint32_t expected_a1)
{
    uint32_t mtvec_value =
        ((uint32_t)(uintptr_t)s_mode_probe_trap_handler) & ~0x3FU;
    uint32_t mstatus_val;
    uint32_t satp_now = 0;
    bool pass;

    clear_s_mode_probe_state();
    s_mode_probe_op = op;
    s_mode_probe_requested_satp = expected_a1;

    __asm__ volatile ("csrw satp, zero" ::: "memory");
    __asm__ volatile ("csrw mtvec, %0" :: "r"(mtvec_value));
    __asm__ volatile ("csrr %0, mstatus" : "=r"(mstatus_val));
    mstatus_val &= ~(3U << 11);
    mstatus_val |= (1U << 11);

    ESP_LOGI(TAG, "S-mode probe start: %s op=%" PRIu32
                  " entry=0x%08" PRIx32 " mtvec=0x%08" PRIx32
                  " stage@0x%08" PRIx32 " satp_req=0x%08" PRIx32,
             name, op, (uint32_t)(uintptr_t)s_mode_probe_entry, mtvec_value,
             (uint32_t)(uintptr_t)&s_mode_probe_stage, s_mode_probe_requested_satp);

    __asm__ volatile (
        "la t0, s_mode_probe_resume_pc\n\t"
        "la t1, 1f\n\t"
        "sw t1, 0(t0)\n\t"
        "la t0, s_mode_probe_resume_sp\n\t"
        "sw sp, 0(t0)\n\t"
        "csrw mepc, %0\n\t"
        "csrw mstatus, %1\n\t"
        "mret\n\t"
        "1:\n\t"
        :
        : "r"(s_mode_probe_entry), "r"(mstatus_val)
        : "t0", "t1", "memory"
    );

    __asm__ volatile ("csrr %0, satp" : "=r"(satp_now));
    __asm__ volatile ("csrw satp, zero\n\t"
                      "fence rw, rw"
                      ::: "memory");

    pass = (s_mode_probe_result == S_MODE_PROBE_RESULT_PASS) &&
           (s_mode_probe_return_a1 == expected_a1);

    ESP_LOGI(TAG, "S-mode probe done: %s result=%" PRIu32
                  " stage=%" PRIu32 " a1=0x%08" PRIx32 " satp_now=0x%08" PRIx32
                  " mcause=0x%08" PRIx32 " mepc=0x%08" PRIx32
                  " mtval=0x%08" PRIx32,
             name, s_mode_probe_result, s_mode_probe_stage,
             s_mode_probe_return_a1, satp_now, s_mode_probe_mcause,
             s_mode_probe_mepc, s_mode_probe_mtval);

    if (!pass) {
        ESP_LOGE(TAG, "S-mode probe failed: %s mstatus=0x%08" PRIx32
                      " mtvec=0x%08" PRIx32,
                 name, s_mode_probe_mstatus, s_mode_probe_mtvec);
    }

    return pass;
}

static bool run_s_mode_probe_suite(void)
{
    const uintptr_t entry_addr = (uintptr_t)s_mode_probe_entry;
    const uintptr_t handler_addr = (uintptr_t)s_mode_probe_trap_handler;
    const uintptr_t stage_addr = (uintptr_t)&s_mode_probe_stage;
    uint32_t sv32_satp_hpsram = 0;
    uint32_t sv32_satp_psram_wb = 0;
    uint32_t sv32_satp_psram = 0;

    if (!s_mode_probe_addr_in_hp_sram(entry_addr, 256U) ||
        !s_mode_probe_addr_in_hp_sram(handler_addr, 256U) ||
        !s_mode_probe_addr_in_hp_sram(stage_addr, sizeof(s_mode_probe_stage)) ||
        !prepare_s_mode_sv32_identity_root_hpsram(&sv32_satp_hpsram) ||
        !prepare_s_mode_sv32_identity_root_psram(true, &sv32_satp_psram_wb) ||
        !prepare_s_mode_sv32_identity_root_psram(false, &sv32_satp_psram)) {
        ESP_LOGE(TAG, "S-mode probe code/data must stay in HP SRAM:"
                      " entry=0x%08" PRIx32 " handler=0x%08" PRIx32
                      " stage=0x%08" PRIx32,
                 (uint32_t)entry_addr, (uint32_t)handler_addr,
                 (uint32_t)stage_addr);
        return false;
    }

    if (!run_s_mode_probe_case("baseline ecall", S_MODE_PROBE_OP_BASELINE_ECALL, 0U) ||
        !run_s_mode_probe_case("fence rw,rw", S_MODE_PROBE_OP_FENCE_RW_RW, 0U) ||
        !run_s_mode_probe_case("csrr satp", S_MODE_PROBE_OP_READ_SATP, 0U) ||
        !run_s_mode_probe_case("csrw satp, zero", S_MODE_PROBE_OP_WRITE_SATP_ZERO, 0U) ||
        !run_s_mode_probe_case("Sv32 enable with HP SRAM root + fence/fence",
                               S_MODE_PROBE_OP_ENABLE_MMU_FENCE_FENCE,
                               sv32_satp_hpsram) ||
        !run_s_mode_probe_case("Sv32 enable with HP SRAM root + sfence/sfence",
                               S_MODE_PROBE_OP_ENABLE_MMU_SFENCE_SFENCE,
                               sv32_satp_hpsram) ||
        !run_s_mode_probe_case("Sv32 enable with PSRAM root + C2M + fence/fence",
                               S_MODE_PROBE_OP_ENABLE_MMU_FENCE_FENCE,
                               sv32_satp_psram_wb)) {
        return false;
    }

    ESP_LOGW(TAG, "Starting destructive S-mode probe op=%" PRIu32
                  " with PSRAM root without C2M writeback"
                  " stage@0x%08" PRIx32,
             S_MODE_PROBE_OP_ENABLE_MMU_FENCE_FENCE, (uint32_t)stage_addr);

    return run_s_mode_probe_case("Sv32 enable with PSRAM root + NO writeback + fence/fence",
                                 S_MODE_PROBE_OP_ENABLE_MMU_FENCE_FENCE,
                                 sv32_satp_psram);
}

static void handoff_trap_vector(void);
static void __attribute__((noreturn, noinline, used)) handoff_trap_report(uint32_t mcause,
                                                                         uint32_t mepc,
                                                                         uint32_t mtval,
                                                                         uint32_t mstatus,
                                                                         uint32_t mtvec,
                                                                         uint32_t mscratch);

static uint32_t __attribute__((noinline)) flash_exec_probe(void)
{
    return PROBE_RESULT;
}

static void __attribute__((naked)) handoff_trap_vector(void)
{
    __asm__ volatile (
        "csrr a0, mcause\n\t"
        "csrr a1, mepc\n\t"
        "csrr a2, mtval\n\t"
        "csrr a3, mstatus\n\t"
        "csrr a4, mtvec\n\t"
        "csrr a5, mscratch\n\t"
        "tail handoff_trap_report"
    );
}

static void __attribute__((noreturn, noinline, used)) handoff_trap_report(uint32_t mcause,
                                                                         uint32_t mepc,
                                                                         uint32_t mtval,
                                                                         uint32_t mstatus,
                                                                         uint32_t mtvec,
                                                                         uint32_t mscratch)
{
    ESP_LOGE(TAG, "handoff trap: mcause=0x%08" PRIx32 " mepc=0x%08" PRIx32
                  " mtval=0x%08" PRIx32,
             mcause, mepc, mtval);
    ESP_LOGE(TAG, "handoff trap: mstatus=0x%08" PRIx32 " mtvec=0x%08" PRIx32
                  " mscratch=0x%08" PRIx32,
             mstatus, mtvec, mscratch);
    esp_restart();
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
    ESP_LOGI(TAG, "mstatus after MPRV clear: 0x%08" PRIx32,
             (uint32_t)RV_READ_CSR(mstatus));
}

static void log_opensbi_early_trap_mailbox(void)
{
    volatile uint32_t *mb = (volatile uint32_t *)OPENSBI_EARLY_TRAP_MAILBOX;

    if (mb[0] != OPENSBI_EARLY_TRAP_MAGIC) {
        return;
    }

    ESP_LOGE(TAG, "previous OpenSBI early trap: mcause=0x%08" PRIx32
                  " mepc=0x%08" PRIx32 " mtval=0x%08" PRIx32,
             mb[1], mb[2], mb[3]);
    ESP_LOGE(TAG, "previous OpenSBI early trap: mstatus=0x%08" PRIx32
                  " mtvec=0x%08" PRIx32 " mscratch=0x%08" PRIx32
                  " mhartid=0x%08" PRIx32,
             mb[4], mb[5], mb[6], mb[7]);

    mb[0] = 0;
}

static bool check_code_probe(const char *name, uintptr_t exec_addr, uintptr_t write_addr)
{
    volatile uint32_t *code = (volatile uint32_t *)write_addr;
    code[0] = PROBE_WORD0;
    code[1] = PROBE_WORD1;
    for (size_t i = 2; i < PROBE_SIZE / sizeof(uint32_t); i++) {
        code[i] = 0;
    }

    cache_ll_l1_invalidate_icache_addr(CACHE_LL_ID_ALL, exec_addr, PROBE_SIZE);
    fence_i();

    uint32_t ret = ((probe_fn_t)exec_addr)();
    if (ret != PROBE_RESULT) {
        ESP_LOGE(TAG, "%s exec probe returned 0x%08" PRIx32, name, ret);
        return false;
    }

    ESP_LOGI(TAG, "%s R/W/X probe ok (write=0x%08" PRIx32 ", exec=0x%08" PRIx32 ")",
             name, (uint32_t)write_addr, (uint32_t)exec_addr);
    return true;
}

static bool check_sram_probe(void)
{
    uintptr_t addr = (uintptr_t)sram_exec_probe_buf;
    if (addr < HP_SRAM_LOW || addr + PROBE_SIZE > HP_SRAM_HIGH) {
        ESP_LOGE(TAG, "SRAM probe buffer outside HP SRAM: 0x%08" PRIx32, (uint32_t)addr);
        return false;
    }

    return check_code_probe("HP SRAM", addr, addr);
}

static bool check_flash_probe(const void *opensbi_ptr, uint32_t fdt_addr)
{
    volatile const uint32_t *opensbi = (volatile const uint32_t *)opensbi_ptr;
    uint32_t first_word = opensbi[0];
    uint32_t fdt_magic = *(const volatile uint32_t *)(uintptr_t)fdt_addr;
    uintptr_t flash_fn = (uintptr_t)flash_exec_probe;

    ESP_LOGI(TAG, "flash read probe: first=0x%08" PRIx32 " fdt_magic=0x%08" PRIx32,
             first_word, fdt_magic);
    ESP_LOGI(TAG, "flash mapped write probe skipped: XIP flash is expected read-only");

    if (flash_fn < FLASH_XIP_LOW || flash_fn >= FLASH_XIP_HIGH) {
        ESP_LOGW(TAG, "flash exec probe function at 0x%08" PRIx32 " is outside XIP window",
                 (uint32_t)flash_fn);
    }

    uint32_t ret = ((probe_fn_t)flash_fn)();
    if (ret != PROBE_RESULT) {
        ESP_LOGE(TAG, "flash exec probe returned 0x%08" PRIx32, ret);
        return false;
    }

    ESP_LOGI(TAG, "flash R/X probe ok (exec=0x%08" PRIx32 ")", (uint32_t)flash_fn);
    return true;
}

static bool check_psram_probe(void)
{
    if (!esp_psram_is_initialized()) {
        ESP_LOGE(TAG, "PSRAM is not initialized");
        return false;
    }

    size_t size = esp_psram_get_size();
    if (size < 0x2000) {
        ESP_LOGE(TAG, "PSRAM size too small: %u", (unsigned)size);
        return false;
    }

    uintptr_t offset = (size - 0x1000U) & ~(uintptr_t)(PROBE_SIZE - 1U);
	if (offset >= PSRAM_SIZE) {
        ESP_LOGE(TAG, "PSRAM probe offset out of mapped window: 0x%08" PRIx32,
                 (uint32_t)offset);
        return false;
    }

	uintptr_t exec_addr = PSRAM_BASE + offset;
    uintptr_t write_addr = PSRAM_BASE + offset;
    return check_code_probe("PSRAM", exec_addr, write_addr);
}

static bool load_initramfs_partition(void)
{
    const esp_partition_t *initramfs_part = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, 0x40, "initramfs");
    if (!initramfs_part) {
        ESP_LOGE(TAG, "initramfs partition not found");
        return false;
    }

    if (initramfs_part->size != INITRAMFS_PARTITION_SIZE) {
        ESP_LOGE(TAG, "initramfs partition size 0x%08" PRIx32
                      " != expected 0x%08" PRIx32,
                 initramfs_part->size, (uint32_t)INITRAMFS_PARTITION_SIZE);
        return false;
    }

    if (!esp_psram_is_initialized()) {
        ESP_LOGE(TAG, "cannot load initramfs before PSRAM init");
        return false;
    }

    size_t psram_size = esp_psram_get_size();
    if (INITRAMFS_LOAD_ADDR < PSRAM_BASE ||
        INITRAMFS_LOAD_ADDR + initramfs_part->size > PSRAM_BASE + psram_size) {
        ESP_LOGE(TAG, "initramfs load range [0x%08" PRIx32 "..0x%08" PRIx32
                      ") outside PSRAM size 0x%08x",
                 (uint32_t)INITRAMFS_LOAD_ADDR,
                 (uint32_t)(INITRAMFS_LOAD_ADDR + initramfs_part->size),
                 (unsigned)psram_size);
        return false;
    }

    esp_err_t err = esp_partition_read(initramfs_part, 0,
                                       (void *)INITRAMFS_LOAD_ADDR,
                                       initramfs_part->size);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "reading initramfs partition failed: %s",
                 esp_err_to_name(err));
        return false;
    }

    uint32_t magic0 = *(const volatile uint32_t *)INITRAMFS_LOAD_ADDR;
    if (magic0 != INITRAMFS_NEWC_MAGIC0) {
        ESP_LOGE(TAG, "initramfs magic 0x%08" PRIx32
                      " != expected newc prefix 0x%08" PRIx32,
                 magic0, (uint32_t)INITRAMFS_NEWC_MAGIC0);
        return false;
    }

    ESP_LOGI(TAG, "initramfs copied: flash=0x%08" PRIx32
                  " size=0x%08" PRIx32 " psram=[0x%08" PRIx32 "..0x%08" PRIx32 ")",
             initramfs_part->address, initramfs_part->size,
             (uint32_t)INITRAMFS_LOAD_ADDR,
             (uint32_t)(INITRAMFS_LOAD_ADDR + initramfs_part->size));
    return true;
}

static bool check_opensbi_early_probe(const void *opensbi_ptr)
{
    uintptr_t mapped_base = (uintptr_t)opensbi_ptr;
    uintptr_t data_lma = mapped_base + (OPENSBI_EARLY_DATA_LMA - OPENSBI_XIP_ADDR);
    volatile const uint32_t *src = (volatile const uint32_t *)data_lma;
    volatile uint32_t *data_start = (volatile uint32_t *)OPENSBI_EARLY_DATA_START;
    volatile uint32_t *data_end = (volatile uint32_t *)(OPENSBI_EARLY_DATA_END - sizeof(uint32_t));
    volatile uint32_t *bss_start = (volatile uint32_t *)OPENSBI_EARLY_BSS_START;
    volatile uint32_t *bss_end = (volatile uint32_t *)(OPENSBI_EARLY_BSS_END - sizeof(uint32_t));
    volatile uint32_t *stack_word = (volatile uint32_t *)(OPENSBI_EARLY_STACK_TOP - sizeof(uint32_t));

    uint32_t old_data0 = data_start[0];
    uint32_t old_data1 = data_start[1];
    uint32_t old_data_end = *data_end;
    uint32_t old_bss0 = bss_start[0];
    uint32_t old_bss_end = *bss_end;
    uint32_t old_stack = *stack_word;

    data_start[0] = src[0];
    data_start[1] = src[1];
    *data_end = src[(OPENSBI_EARLY_DATA_END - OPENSBI_EARLY_DATA_START) / sizeof(uint32_t) - 1U];
    bss_start[0] = 0;
    *bss_end = 0;
    *stack_word = 0xA5A55A5AU;

    uint32_t old_amo = 0;
    __asm__ volatile ("amoswap.w %0, %2, (%1)"
                      : "=r"(old_amo)
                      : "r"(data_start), "r"(0x13572468U)
                      : "memory");
    data_start[0] = old_amo;

    data_start[0] = old_data0;
    data_start[1] = old_data1;
    *data_end = old_data_end;
    bss_start[0] = old_bss0;
    *bss_end = old_bss_end;
    *stack_word = old_stack;

    ESP_LOGI(TAG, "OpenSBI early data probe ok: lma=0x%08" PRIx32
                  " data=[0x%08" PRIx32 "..0x%08" PRIx32 ")"
                  " bss=[0x%08" PRIx32 "..0x%08" PRIx32 ")"
                  " stack_top=0x%08" PRIx32,
             (uint32_t)data_lma, (uint32_t)OPENSBI_EARLY_DATA_START,
             (uint32_t)OPENSBI_EARLY_DATA_END, (uint32_t)OPENSBI_EARLY_BSS_START,
             (uint32_t)OPENSBI_EARLY_BSS_END, (uint32_t)OPENSBI_EARLY_STACK_TOP);
    return true;
}

static void run_handoff_probes(const void *opensbi_ptr, uint32_t fdt_addr)
{
    if (!check_sram_probe() ||
        !check_flash_probe(opensbi_ptr, fdt_addr) ||
        !check_psram_probe() ||
        !check_opensbi_early_probe(opensbi_ptr)) {
        ESP_LOGE(TAG, "handoff probes failed");
        esp_restart();
    }
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
    log_opensbi_early_trap_mailbox();
    disable_apm();
    disable_watchdogs();
    clear_mstatus_mprv();
    install_global_pmp();

    /* --- Find and mmap OpenSBI partition for Flash XIP --- */
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
    ESP_LOGI(TAG, "OpenSBI mmap at %p (size %" PRIu32 "), entry offset +0x%x (CHECK IF MATCH OpenSBI BUILD)",
             opensbi_ptr, part->size, 0);

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

    if (!load_initramfs_partition()) {
        ESP_LOGE(TAG, "initramfs load failed");
        esp_restart();
    }

    // run_s_mode_test();
    if (!run_trap_redirect_test() || !run_s_mode_probe_suite()) {
        ESP_LOGE(TAG, "pre-handoff S-mode probes failed");
        esp_restart();
    }
    run_handoff_probes(opensbi_ptr, fdt);
    disable_test_s_mode_clic_interrupt();

    /* Jump to OpenSBI in M-mode.  a1 carries the FDT address which OpenSBI
     * may relocate (or ignore when a1 == 0). */
    uint32_t entry = (uint32_t)opensbi_ptr;
    RV_WRITE_CSR(mtvec, (uint32_t)(uintptr_t)handoff_trap_vector);
    ESP_LOGI(TAG, "handoff trap vector at 0x%08" PRIx32, (uint32_t)(uintptr_t)handoff_trap_vector);

    disable_stack_protector();

    ESP_LOGI(TAG, "PMP handoff: pmpcfg0=0x%08" PRIx32 " pmpaddr0=0x%08" PRIx32,
             (uint32_t)RV_READ_CSR(pmpcfg0), (uint32_t)RV_READ_CSR(pmpaddr0));
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
