# Native ESP-IDF throughput reference
Derived from the official BLE throughput server and Classic SPP acceptor at
ESP-IDF 08e0d30a74ad0bfd5a34933142b80f45619ee410. See ../logs/native-idf-target.md
for the user's revised target and ../logs/native-idf/ for raw evidence.

## Safety and build
CPU 320 MHz, no per-packet logging, power save disabled. The custom partition
table places NVS at 0x11000 and app at 0x20000; every native erase and partition
ends at or below Linux persist start 0x1ee000. Never use chip erase.
Run flash_native.py with the IDF Python environment; --execute writes and
verifies all four native images, including fresh benchmark NVS. This NVS is
separate from Linux persist. Restore all six Linux slots after experiments.

Build each project after sourcing /home/grieferpig/esp-idf/export.sh:
idf.py --preview set-target esp32s31 build
Incremental: idf.py --preview build

## BLE
Native esp_ble_gatts_send_indicate, sendable-packet/congestion handling, MTU517,
sequence/pattern checked values. Native address ends AD; Linux ends AE.
Write [0xa1, interval_lo, interval_hi, length_lo, length_hi] to ff12 before
AcquireNotify to select interval (1.25 ms units) and value length.
S31_BLE_INTERVAL / S31_BLE_VALUE_LEN control the host harness.
Request DLE251 at connect and again on subscribe. This peer rejects DLE as
unsupported; API request success alone must not be called negotiated DLE.
LE1M with this same BasedSurface Marvell peer is the reference.

## SPP
Native esp_spp_write, write-completion/congestion callbacks with one pending write and partial-write
retries (the completion length can be shorter than the request), authenticated
channel1. Client sends 'S31' plus little-endian uint16 frame length to start;
20..4096 bytes supported. Set S31_SPP_START_CONTROL=1 and S31_SPP_FRAME_SIZE.
Payload begins uint32 sequence followed by byte[k]=(k XOR sequence)&255.
The host validates every complete frame and retains trailing partial bytes.

Both benchmarks use fresh agent/pairing and 40-second host receive windows.
Separate BLE-only and Classic-only controller modes differ from Linux BTDM;
report this difference rather than implying perfectly identical stack configs.
