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
    la      t0, core1_early_trap
    csrw    mtvec, t0

    /*
     * Preserve the first failed handoff. A core-only reset re-enters this
     * trampoline without clearing HP SRAM, so park instead of overwriting
     * the OpenSBI/Linux diagnostic mailbox and PMP state.
     */
    la      t0, g_core1_trampoline_entered
    lw      t1, 0(t0)
    bnez    t1, core1_reentry_park
    li      t1, 1
    sw      t1, 0(t0)
    fence   rw, rw

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
     * PMA is hart-local. Mirror ESP-IDF's ESP32-S31 region-protection setup:
     * entry 7 covers the full 64 MiB cached PSRAM aperture with RWX NAPOT.
     * Without this entry hart1 reports a PMP/PMA store fault on 0x50000000,
     * even though the shared PSRAM MMU, cache bus, MSPI PMS and APM permit it.
     *
     * pmaaddr7 = (0x50000000 | ((0x04000000 / 2) - 1)) >> 2
     * pmacfg7  = PMA_L | PMA_NAPOT | PMA_EN | PMA_R | PMA_W | PMA_X
     */
    csrw    0xbc7, zero             /* pmacfg7 */
    csrw    0xbd7, zero             /* pmaaddr7 */
    li      t0, 0x147fffff
    csrw    0xbd7, t0
    li      t0, 0xe000001d
    csrw    0xbc7, t0

    /* Full RV32 PMP physical space for Linux S-mode, RWX NAPOT. */
    li      t0, -1
    csrw    pmpaddr0, t0
    li      t0, 0x1f
    csrw    pmpcfg0, t0

    /*
     * The loader has completed the shared D-cache ownership handoff and the
     * hart1 PSRAM I-bus setup. OpenSBI owns the top 1 MiB cached PSRAM.
     */
    li      sp, 0x51000000
    li      a0, 1
    la      t0, g_core1_fdt
    lw      a1, 0(t0)
    li      a2, 0

    fence   rw, rw
    fence.i

    li      t0, 0x40030000
    jr      t0

    .size core1_linux_trampoline, . - core1_linux_trampoline

core1_reentry_park:
    wfi
    j       core1_reentry_park

    .align  2
core1_early_trap:
    csrr    t0, mcause
    la      t1, g_core1_trap_mcause
    sw      t0, 0(t1)
    csrr    t0, mtval
    la      t1, g_core1_trap_mtval
    sw      t0, 0(t1)
    csrr    t0, mepc
    la      t1, g_core1_trap_mepc
    sw      t0, 0(t1)
    fence   rw, rw
1:
    wfi
    j       1b

    .option pop
