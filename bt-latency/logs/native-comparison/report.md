# Linux matched native comparison
Native reference: ../native-idf/report.md (BLE31.8KiB/s, SPP214.4KiB/s).

## Before patch0033, restored dist94fb9aabbbf452d9
Packet logging disabled, burst24, PHY1M, DLE requested, COEX0.
Fresh host adapter power cycle, registered NoInputNoOutput agent, scan, pair,
connect, 40-second host receive window. BLE PAIRABLE0; Classic PAIRABLE1.
- BLE504-byte values, existing24..40 interval request (normally45ms):
  1,175,832 bytes /40s,2333 notifications,28.71KiB/s, zero sequence/pattern errors.
- SPP1012-byte frames:
  4,401,188 bytes /40.000174730s,4349frames,107.450410KiB/s,
  zero sequence/pattern errors.
These fresh runs replace earlier88.70KiB/s as the immediate Classic comparison;
the variation itself means a single rate must not be treated as invariant.

## Comparison controls (patch0033)
Defaults stay unchanged. S31_BTSTACK_SPP_BYTES=990 and
S31_BTSTACK_BLE_BYTES=495 select matching native payload lengths.
S31_BTSTACK_INTERVAL=12 requests exactly15ms using existing connection-update API;
actual accepted updates are printed even with packet logging disabled.
No controller, kernel scheduling, transport or Wi-Fi changes.
New patch applied with patch -p1 in a scratch directory and the complete patched
source matched byte-for-byte. Build succeeded:
PATH="$HOME/.local/bin:$PATH" ESP_TOOLS="$HOME/.espressif/tools/riscv32-esp-elf/esp-16.1.0_20260609/riscv32-esp-elf" S31_ALLOW_UNPINNED=1 make build image
Output dist/c5959e35f54b83d8. Kernel, radio and DTB hashes match the old set;
rootfs and rebuilt bootloader image hashes differ. All six slots are flashed,
excluding persist; never use generated s31_full_flash.bin.
Performance acceptance of the new knobs is pending real board tests.

## Matched real-board results
dist/c5959e35f54b83d8 flashed as a full six-slot set; all six explicit
verify-flash digests matched. No persist writes.
- SPP990: 4,200,570 bytes /40.000125553s =102.552657KiB/s,
  4243frames, zero sequence/pattern errors. Radio counters afterward showed
  zero HCI RX/TX drops.
- BLE495, accepted interval12 (15ms), MTU517, 1M:
  1,241,460 bytes /40s =30.31KiB/s,2508notifications,
  zero sequence/pattern errors. Global HCI RX drops were3 afterward versus0
  before the BLE restart; the exact phase of those drops is not localized.
These do not meet the native31.8/214.4KiB/s targets. Payload matching alone
does not fix Classic. Actual BLE interval acceptance is in matched-final.txt.

The new console helper initially transmitted a literal backslash-r rather than
CR, so its first command was not executed. That helper was stopped, the pending
line cleared with Ctrl-U, and the corrected helper sent raw CR and observed
a unique printf completion marker. comparison-bringup-fixed.txt proves recovery.
The corrected helper also propagates command exit status. Optional board stats
are collected before/after host reception, outside its40-second timed window.
