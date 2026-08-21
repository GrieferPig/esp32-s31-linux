# ESP32-S31 Linux radio core

The Wi-Fi, Bluetooth, coexistence and PHY blobs execute in Linux S-mode. They
are linked into `vmlinux`; OpenSBI neither calls them nor handles their timer or
radio interrupts.

## Execution contract

All blob execution is serialized by one global blob gate. Linux hard IRQs only
acknowledge/mask the CLIC source and wake `s31-radio`; the worker runs deferred
ISR callbacks. Compatibility tasks such as `wifi`, `sys_evt`, and `btdm` also
enter the same gate before executing closed payload code. The worker serializes:

- jiffies-derived compatibility-RTOS ticks;
- deferred ESP-IDF interrupt callbacks;
- compatibility scheduler passes;
- Bluetooth's post-IRQ-route enable task; and
- typed requests from the HCI and future cfg80211 front ends.

During each blob pass, local S-mode interrupts are disabled, Linux owns the
single-precision floating-point registers through `kernel_fpu_begin()`, and
`thread_info.kernel_sp` points into the reserved internal-SRAM exception area.
The worker restores all three before it performs Linux IRQ-domain operations
or sleeps.

No generic `call(function_pointer, argument)` interface is exported. The public
header, `include/linux/esp32s31-radio.h`, contains only typed operations. The
health operation, `esp32s31_radio_get_health()`, is delivered
through the serialized command queue and reports init results, tick/pass
counters and SRAM heap usage. Bluetooth uses bounded H4 RX/TX rings. The blob
copies controller packets into the RX ring while the gate is held; only after
Linux IRQ and FPU state has been restored does the core call the HCI driver.
TX packets take the reverse path and are consumed by VHCI inside the next
serialized pass. Raw `esp_vhci_*` symbols remain local to the payload.

## Memory and interrupt ownership

- `0x2f030000..0x2f038840`: linked blob data and BSS;
- `0x2f038840..0x2f071800`, `0x2f018000..0x2f030000`, and
  `0x2f078c00..0x2f07cfb0`: internal-SRAM blob heap chunks, managed by a Linux
  `gen_pool` (349,040 bytes total in the current image);
- `0x2f071800..0x2f072380`: synchronous-exception stack and guard;
- `0x2f072380..0x2f078c00`: radio DMA/status reservation;
- radio sources 127, 124 and 133: CLIC47, CLIC45 and CLIC44; and
- legacy Wi-Fi MAC sources 122 and 120: CLIC43 and CLIC42.

All heap-capability allocations used by the Wi-Fi/BT payload are wrapped onto
the internal-SRAM pool. The loader reserves the entire radio interval before
releasing hart1.

## Current hardware result (2026-08-21)

The built-in cfg80211 and HCI front ends register `wlan0` and `hci0`. Wi-Fi
connects to a WPA2 AP, obtains DHCP, and transfers data while BLE discovery is
active. Radio interrupts remain native S-mode CLIC interrupts; OpenSBI neither
handles nor proxies them.

The closed BLE controller asserted at `ble_lll_mmgmt.c:648` when both harts ran
pinned CoreMark alongside Wi-Fi and discovery. The later Flash-XIP trap faults
were nested consequences of the IDF assert/panic path, not evidence that radio
activity disabled flash/cache. The actual mismatch was scheduling: CFS nice
levels do not preserve the FreeRTOS latency contract, while promoting only
`btdm` starves the equal-priority Wi-Fi MAC task.

Both IDF priority-23 tasks (`wifi` and `btdm`) now run as Linux `SCHED_RR/80`,
matching FreeRTOS equal-priority time slicing. The deferred ISR worker uses
`SCHED_FIFO/90` only during a pass that begins with pending IRQ callbacks and
demotes immediately afterward. A 90-second test with one CoreMark on each hart,
continuous ping and HTTP wget, and BLE discovery completed with 89/89 pings,
209 HTTP transfers, four valid CoreMark runs per hart, and no assertion, panic,
reset, or shell stall.

The scanned `Mesh Mi Switch` briefly reports connected but aborts locally before
GATT service discovery. This does not reproduce the concurrency failure; use a
known continuously-connectable peripheral (or the switch provisioning window)
to validate remote GATT enumeration.

The kernel intentionally does not require a signed external regulatory
database. A missing `regulatory.db` is non-fatal for this firmware-free image.
