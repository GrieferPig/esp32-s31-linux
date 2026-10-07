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
