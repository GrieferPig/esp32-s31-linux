/*
 * ESP32-S31 hart1 Linux handoff trampoline.
 *
 * This is entered directly from the core1 boot-address register, bypassing
 * ESP-IDF's secondary-core startup path. It must not use a C stack or globals
 * through gp before establishing a known machine state.
 */

    .section .iram1, "ax"
    .align  6
    .global core1_linux_trampoline
    .type   core1_linux_trampoline, @function
    .option push
    .option norelax

core1_linux_trampoline:
    csrw    mie, zero
    csrw    mtvec, zero
    csrw    0x307, zero             /* mtvt */
    li      t0, (1 << 17)           /* MSTATUS_MPRV */
    csrc    mstatus, t0

    /* Clear this hart's address-virtualised M-mode CLIC slots. */
    li      t0, 0x10801000
    li      t1, 128
1:
    sb      zero, 0(t0)             /* clicintip */
    sb      zero, 1(t0)             /* clicintie */
    sb      zero, 2(t0)             /* clicintattr */
    sb      zero, 3(t0)             /* clicintctl */
    addi    t0, t0, 4
    addi    t1, t1, -1
    bnez    t1, 1b

    /*
     * Full RV32 PMP physical address space, RWX NAPOT.
     * Cached flash/PSRAM accesses are checked after the S31 proprietary MMU
     * translation. Do not set PMP_L: S31's documented S-mode setup uses
     * pmpaddr0=-1/pmpcfg0=0x1f, and locking the entry also subjects M-mode
     * cache/MMU accesses to PMP checks.
     */
    li      t0, -1
    csrw    pmpaddr0, t0
    li      t0, 0x1f
    csrw    pmpcfg0, t0

    /*
     * OpenSBI owns the top 1 MiB PSRAM through its uncached direct alias and
     * will establish its final stack. Cached PSRAM is unsafe during hart1
     * bring-up before OpenSBI/Linux have established their memory state.
     */
    li      sp, 0xc1000000
    li      a0, 1
    la      t0, g_core1_fdt
    lw      a1, 0(t0)
    li      a2, 0

    la      t0, g_core1_trampoline_entered
    li      t1, 1
    sw      t1, 0(t0)
    fence   rw, rw
    fence.i

    li      t0, 0x40030000
    jr      t0

    .size core1_linux_trampoline, . - core1_linux_trampoline
    .option pop
