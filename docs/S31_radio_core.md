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

- `0x2f030000..0x2f071800`: blob heap, managed by a Linux `gen_pool`;
- `0x2f071800..0x2f072380`: synchronous-exception stack and guard;
- TIMG1/T1 source 29: CLIC46, 100 Hz compatibility tick;
- radio sources 127, 124 and 133: CLIC47, CLIC45 and CLIC44; and
- legacy Wi-Fi MAC sources 122 and 120: CLIC43 and CLIC42.

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

## Stage-4 hardware result

The built-in `hci_esp32s31` driver registered `hci0` through Linux's standard
Bluetooth HCI core. A management-channel BLE discovery powered the controller,
ran for eight seconds and found seven unique nearby devices. Source 124/Clic45
delivered three controller interrupts during the scan. OpenOCD halted hart1
inside `s31_process_timeouts` with `priv=1` and an internal-SRAM stack pointer,
confirming that the active controller/compatibility scheduler path stayed in
Linux S-mode. The five-partition radio-only image uses `/dev/mtdblock5` for its
SquashFS root.

## Stage-5 hardware result

The built-in `esp32s31_wifi` fullmac front end registered `phy0` and `wlan0`
with cfg80211. A dependency-free userspace test sent a standard generic-netlink
`NL80211_CMD_TRIGGER_SCAN`, waited for completion, and dumped the cfg80211 BSS
cache. Two consecutive scans each returned six nearby 2.4-GHz networks with
BSSID, frequency, RSSI and SSID.

Starting Wi-Fi exposed an ESP-IDF assumption that its legacy logical interrupt
1 could program the M-mode CLIC window directly. The S-mode adapter now
translates Wi-Fi sources 122 and 120 into Linux-owned external CLIC43 and
CLIC42. Hard IRQs only mask and wake the radio worker; the shared blob ISR runs
inside the serialized gate. After two scans `/proc/interrupts` reported 69
interrupts on CLIC42, no Oops or warning was present, and the radio tick had
continued past 9,000 interrupts. OpenOCD halted hart1 during the repeated scan
with `priv=1`, then resumed it; the scan still completed normally.

The kernel is configured not to require a signed external regulatory database.
This is intentional for the firmware-free bring-up image: attempting to parse
the built-in X.509 regulatory certificates exercised an unrelated, unfinished
S31 SHA-DMA path before the radio worker started. A missing `regulatory.db` is
therefore non-fatal for this stage.
