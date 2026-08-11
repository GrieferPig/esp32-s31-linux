# ESP32-S31 Linux radio core

The Wi-Fi, Bluetooth, coexistence and PHY blobs execute in Linux S-mode. They
are linked into `vmlinux`; OpenSBI neither calls them nor handles their timer or
radio interrupts.

## Execution contract

There is one blob execution owner: the `s31-radio` kthread. Linux hard IRQs
only acknowledge/mask the CLIC source and enqueue work. The worker serializes:

- TIMG1/T1 compatibility-RTOS ticks;
- deferred ESP-IDF interrupt callbacks;
- compatibility scheduler passes;
- Bluetooth's post-IRQ-route enable task; and
- typed requests from future HCI and cfg80211 front ends.

During each blob pass, local S-mode interrupts are disabled, Linux owns the
single-precision floating-point registers through `kernel_fpu_begin()`, and
`thread_info.kernel_sp` points into the reserved internal-SRAM exception area.
The worker restores all three before it performs Linux IRQ-domain operations
or sleeps.

No generic `call(function_pointer, argument)` interface is exported. The public
header, `include/linux/esp32s31-radio.h`, contains only typed operations. Its
first operation is `esp32s31_radio_get_health()`, which is itself delivered
through the serialized command queue and reports init results, tick/pass
counters and SRAM heap usage. Bluetooth HCI and cfg80211 requests extend that
typed interface in their respective stages.

## Memory and interrupt ownership

- `0x2f030000..0x2f071800`: blob heap, managed by a Linux `gen_pool`;
- `0x2f071800..0x2f072380`: synchronous-exception stack and guard;
- TIMG1/T1 source 29: CLIC46, 100 Hz compatibility tick;
- radio sources 127, 124 and 133: CLIC47, CLIC45 and CLIC44.

All heap-capability allocations used by the Wi-Fi/BT payload are wrapped onto
the internal-SRAM pool. The loader reserves the entire radio interval before
releasing hart1.

## Stage-3 hardware result

The full radio payload and XIP kernel booted on the board. Wi-Fi init, BT init
and BT enable returned zero; all three radio routes were installed. The queued
health self-test reported READY with 88,256 bytes used, a 92,464-byte peak and
268,288 bytes total. At 65 seconds uptime the TIMG1 IRQ count was 6,281 and the
worker remained live. OpenOCD sampled hart1 twice in S-mode and confirmed the
three interrupt-matrix registers still contained CLIC45, CLIC47 and CLIC44.
