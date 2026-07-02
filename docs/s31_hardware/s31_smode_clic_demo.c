/**
 * @file s31_smode_clic_demo.c
 * @brief Minimal working demo for ESP32-S31 S-mode CLIC interrupt handling.
 *
 * This demo demonstrates how to configure APM, PMP, and CLIC to route and handle
 * an interrupt purely in S-mode, reading the proper S-mode CLIC base (sclicbase 0x150).
 */

#include <stdint.h>
#include <stdbool.h>

// --- APM Registers ---
#define DR_REG_CPU_APM_BASE          0x2C010000
#define CPU_APM_REGION_FILTER_EN_REG (DR_REG_CPU_APM_BASE + 0x0)
#define CPU_APM_REGION0_ADDR_START_REG (DR_REG_CPU_APM_BASE + 0x4)
#define CPU_APM_REGION0_ADDR_END_REG   (DR_REG_CPU_APM_BASE + 0x8)
#define CPU_APM_REGION0_ATTR_REG       (DR_REG_CPU_APM_BASE + 0xc)
#define CPU_APM_FUNC_CTRL_REG          (DR_REG_CPU_APM_BASE + 0xc4)

#define REG_WRITE(addr, val) (*(volatile uint32_t *)(addr) = (val))

// --- Demo Variables ---
volatile uint32_t demo_sclicbase_val = 0;
volatile bool demo_int_handled = false;

// ---------------------------------------------------------
// S-mode Interrupt Handler
// ---------------------------------------------------------
static void __attribute__((interrupt("supervisor"), aligned(64))) demo_s_clic_handler(void)
{
    demo_int_handled = true;

    // 1. Read the S-mode specific CLIC base address from CSR 0x150
    uint32_t sclicbase;
    __asm__ volatile ("csrr %0, 0x150" : "=r"(sclicbase));
    demo_sclicbase_val = sclicbase; // Typically 0x10A00000 on S31

    // 2. Clear the interrupt pending bit for ID 1
    // Offset for IP of ID 1 is 0x1000 + 4 * 1 = 0x1004
    volatile uint8_t *clicint1_ip = (volatile uint8_t *)(sclicbase + 0x1004);
    *clicint1_ip = 0;
}

// ---------------------------------------------------------
// S-mode Payload (executes after interrupt returns)
// ---------------------------------------------------------
static void __attribute__((aligned(64))) demo_s_mode_payload(void)
{
    // The interrupt was pending, so demo_s_clic_handler has already run!
    
    // Ecall to return to M-mode
    __asm__ volatile ("ecall");
}

// ---------------------------------------------------------
// M-mode Trap Handler (Catches the ecall to end the demo)
// ---------------------------------------------------------
volatile uint32_t demo_resume_pc;
static void __attribute__((naked, aligned(64))) demo_m_trap_handler(void)
{
    __asm__ volatile (
        "csrw pmpcfg0, zero\n\t"       // Revoke S-mode PMP access
        "la t0, demo_resume_pc\n\t"
        "lw t0, 0(t0)\n\t"
        "csrw mepc, t0\n\t"            // Set return address to after the demo
        "li t0, 0x1800\n\t"
        "csrs mstatus, t0\n\t"         // MPP = M-mode
        "mret\n\t"                     // Return to run_smode_demo execution
    );
}

// ---------------------------------------------------------
// Setup and Trigger from M-mode
// ---------------------------------------------------------
void run_smode_demo(void)
{
    // 1. Configure CPU APM to allow S-mode (REE_MODE 1) to access peripherals
    REG_WRITE(CPU_APM_REGION0_ADDR_START_REG, 0);
    REG_WRITE(CPU_APM_REGION0_ADDR_END_REG, 0xFFFFFFFF);
    REG_WRITE(CPU_APM_REGION0_ATTR_REG, 0x7777); // Grant all modes R/W/X
    REG_WRITE(CPU_APM_REGION_FILTER_EN_REG, 1);
    REG_WRITE(CPU_APM_FUNC_CTRL_REG, 0x0F);

    // 2. Set up M-mode trap vector (to catch the ecall from S-mode)
    __asm__ volatile ("csrw mtvec, %0" :: "r"(((uint32_t)(uintptr_t)demo_m_trap_handler & ~0x3F)));

    // 3. Enable multi-privilege interrupts (NMBITS = 1) in mcliccfg
    volatile uint32_t *mcliccfg = (volatile uint32_t *)0x10800000;
    *mcliccfg = (*mcliccfg & ~(3U << 5)) | (1U << 5); 

    // 4. Configure Interrupt ID 1 (Must write bytes separately for edge-trigger to stick)
    volatile uint8_t *clicint1_attr = (volatile uint8_t *)(0x10801006);
    volatile uint8_t *clicint1_ie   = (volatile uint8_t *)(0x10801005);
    volatile uint8_t *clicint1_ip   = (volatile uint8_t *)(0x10801004);
    volatile uint8_t *clicint1_ctrl = (volatile uint8_t *)(0x10801007);
    
    *clicint1_attr = 0x42; // Mode=1 (S-mode), Edge-triggered
    *clicint1_ctrl = 0xFF; // Max priority
    *clicint1_ie = 1;      // Enable
    *clicint1_ip = 1;      // Trigger (Pending = 1)

    // 5. Configure S-mode Trap Vector (CLIC mode = bits 0-1 set to 3)
    __asm__ volatile ("csrw stvec, %0" :: "r"(((uint32_t)(uintptr_t)demo_s_clic_handler & ~0x3F) | 3));

    // 6. Setup mstatus for transitioning to S-mode
    uint32_t mstatus_val;
    __asm__ volatile ("csrr %0, mstatus" : "=r"(mstatus_val));
    mstatus_val |= (1 << 1);  // SIE = 1
    mstatus_val |= (1 << 5);  // SPIE = 1
    mstatus_val &= ~(3 << 11);
    mstatus_val |= (1 << 11); // MPP = S-mode
    
    // 7. Grant PMP memory access to S-mode & Clear thresholds
    __asm__ volatile (
        "li t0, -1\n\t"
        "csrw pmpaddr0, t0\n\t"
        "li t0, 0x1F\n\t"
        "csrw pmpcfg0, t0\n\t"
        "csrw 0x347, zero\n\t" // mintthresh = 0
        "csrw 0x147, zero\n\t" // sintthresh = 0
    );

    // 8. Launch S-mode
    __asm__ volatile (
        "la t0, demo_resume_pc\n\t"
        "la t1, 1f\n\t"
        "sw t1, 0(t0)\n\t"
        "csrw mepc, %0\n\t"
        "csrw mstatus, %1\n\t"
        "mret\n\t"
        "1:\n\t" // Execution resumes here after M-mode catches the ecall
        :: "r"(demo_s_mode_payload), "r"(mstatus_val)
        : "t0", "t1", "memory"
    );

    // At this point, demo_int_handled == true and demo_sclicbase_val == 0x10A00000
}
