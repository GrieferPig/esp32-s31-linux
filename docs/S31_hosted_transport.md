# S31 FreeRTOS/Linux ESP-Hosted transport

## Ownership and memory

- hart0 runs ESP-IDF/FreeRTOS and owns the Wi-Fi/Bluetooth hardware.
- hart1 runs OpenSBI/Linux.
- `0x2f062f80..0x2f072f80` is a 64 KiB transport reservation in internal
  HP SRAM. It is directly shared by both HP harts; ring ownership transfers
  use RISC-V fences. The S31 external-memory cache-sync engine is not used
  for this internal `0x2f...` range.
- Linux maps the unaligned 64 KiB reservation through 17 I/O fixmap
  pages. FreeRTOS accesses the same physical address directly.

## ABI

The shared ABI is defined only by `shared/s31_hosted_sram.h`.

- A 4 KiB control area contains link metadata and two SPSC ring states.
- Each direction has 16 slots of 1920 bytes.
- Each slot has an 8-byte ring header and up to 1912 bytes of ESP-Hosted
  frame data.
- The ESP-Hosted frame uses the upstream 12-byte `esp_payload_header`,
  including sequence number and additive checksum.
- The producer writes data, length, and slot sequence before publishing the
  ring producer. The consumer advances only its own consumer word.
- Unsigned producer-minus-consumer arithmetic makes normal 32-bit counter
  wrap safe. A distance greater than 16 is treated as ring corruption and
  recovered by dropping to the current producer.

## Doorbells

- FreeRTOS to Linux: `HP_SYSTEM_CPU_INT_FROM_CPU_2`, interrupt-matrix source
  67, Linux raw CLIC ID 40, register `0x20586018`.
- Linux to FreeRTOS: `HP_SYSTEM_CPU_INT_FROM_CPU_3`, register `0x2058601c`.
- Doorbells are hints and may coalesce. Both consumers recheck their rings.
  Hart0 uses a polling task instead of installing a
  CLIC handler into ESP-IDF's vector table. This task must not call
  `taskYIELD()`: Linux reconfigures the shared interrupt environment and the
  hart0 FreeRTOS tick can stop, leaving a yielded task permanently dormant.
- Linux uses the private OpenSBI Hosted extension for the S-mode user-buffer
  copy and ring commit.

## Boot ordering

FreeRTOS reserves the transport/GDMA/DWC2 SRAM range from the IDF heap,
initializes and publishes the complete transport, and starts the polling task
before releasing hart1. Linux rejects a missing magic or ABI mismatch during
probe.

The full ESP-Hosted co-processor image is larger than the original loader.
The project derives its memory linker script from IDF's S31
`memory.ld.in`, changing only the FreeRTOS flash-XIP base to `0x42000020`.
The loader maps the complete 16-MiB Flash linearly at `0x40000000`, so OpenSBI
and Linux execute at `0x40220000` and `0x40400000`. It programs that mapping
directly with the S31 MMU
HAL; `esp_partition_mmap()` is not used because its dynamically selected
address is not a fixed-link ABI.

## ESP-Hosted-FG integration

`bootloader/components/esp_hosted_slave` builds the ESP-Hosted-MCU
co-processor control plane, protobuf RPC implementation, and Wi-Fi data path
as an IDF component. `sram_slave_api.c` is its physical-interface adapter.
It converts the upstream co-processor's one-based interface numbers to the
zero-based ESP-Hosted wire ABI and publishes the standard INIT capability
event. The adapter also bridges ESP-IDF station connect/disconnect events to
the shared control block: it publishes the real STA MAC and sends a private
link event so Linux updates the `ethsta0` address and carrier state.

Linux registers:

- `ethsta0` for STA/AP Ethernet frames;
- `/dev/esps0` for the ESP-Hosted SERIAL TLV/protobuf control channel.
- `hci0` for the S31 BLE controller through Linux Bluetooth HCI core.

The initramfs includes the statically linked `/sbin/esp-hosted-ctl` control
binary. It links the protobuf-C runtime and generated
`esp_hosted_rpc.pb-c.c` from the same ESP-Hosted-FG tree as the FreeRTOS
co-processor, rather than using the incompatible legacy Linux
`esp_hosted_config.proto` ABI. `/sbin/test.out` is a compatibility symlink
for existing ESP-Hosted-FG scripts. Supported commands are:

```
esp-hosted-ctl sta_connect <ssid> <password> [--dhcp]
esp-hosted-ctl sta_disconnect
esp-hosted-ctl get_wifi_mode
esp-hosted-ctl set_wifi_mode <0..3>
esp-hosted-ctl wifi_stop
esp-hosted-ctl get_fw_version
```

`sta_connect` waits for the ESP-Hosted connected/disconnected event and
returns failure on timeout. The optional `--dhcp` runs the initramfs DHCP
client after association.

The initramfs includes statically linked BlueZ `hciconfig` and `hcitool`
utilities. They are built from the pinned BlueZ release by the root Makefile
without D-Bus, GLib, or dynamic-library runtime dependencies. BLE discovery
can be run with:

```
hciconfig -a
hcitool dev
/sbin/ble-scan 15
```

S31 only accepts Bluetooth 5.x extended scan commands, while the deprecated
BlueZ `hcitool lescan` command always uses legacy scanning. Use
`/sbin/ble-scan [seconds]` for discovery on this target. It uses the kernel
Bluetooth Management interface, allowing the kernel to select extended HCI
scanning while retaining BlueZ tools for controller inspection and raw HCI
diagnostics.

S31 is configured as an ESP-IDF controller-only, BLE-only device using the
RAM/VHCI interface. FreeRTOS initializes and enables the controller before
releasing hart1. For HCI frames, the H4 packet type is stored in the final
byte of the 12-byte Hosted header and is not duplicated in the payload.
Linux reconstructs `hci_skb_pkt_type()` on receive and performs the inverse
conversion on transmit. Linux Bluetooth core's normal setup sequence is used
to validate and discover the controller.

`/dev/esps0` preserves RPC message boundaries. Outgoing writes are fragmented
at 1500 bytes with one sequence number; incoming fragments are reassembled by
sequence and queued as complete messages. A read buffer smaller than the next
message returns `EMSGSIZE`. Each userspace `write()` starts one RPC message;
test tools must therefore issue the complete pserial TLV in one write rather
than relying on a shell `printf` that may split its output.

The S31 core traps the unaligned halfword read used by the upstream pserial
TLV parser. The local port decodes the little-endian length byte-wise and
validates every TLV against the remaining buffer length.

## Built-in transport test

Linux sends 17 deterministic 1500-byte `TEST_IF` frames one at a time.
FreeRTOS echoes each frame and Linux verifies its length, contents, and
checksum. Seventeen rounds cross the 16-slot ring boundary and therefore test
both wrap paths. INIT capability parsing is independently required before the
SERIAL device accepts writes.

Successful Linux output contains:

```
ESP-Hosted ready: chip=32 capabilities=0xe8 ext=0x000000
Bluetooth HCI initialized: commands=23 tx=96 rx=265 bytes
transport self-test passed: 17 x 1500-byte frames
SRAM transport generation N, netdev ethsta0, RPC /dev/esps0, HCI hci0, IRQ N
```

Use `/dev/ttyUSB0` with `idf.py monitor` for serial monitoring. OpenOCD is only
for JTAG inspection through its telnet port and must be stopped before
flashing.

## Current hardware-test status

Hardware tests on S31 pass:

- ESP-Hosted INIT negotiation (`chip=32`, capabilities `0xe8`);
- Linux HCI setup with 23 controller commands and verified bidirectional
  traffic (96 transmitted and 265 received HCI payload bytes);
- all 17 deterministic 1500-byte wraparound frames;
- one complete GetWifiMode protobuf RPC in each direction;
- 20 sequential RPC requests and 20 back-to-back queued requests, with all
  40 responses retaining their individual 25-byte message boundaries;
- Wi-Fi init, STA mode/config/start/connect RPCs against `Griefer`, followed
  by a DHCP lease on `ethsta0` (`10.131.205.48/24`);
- 100/100 gateway pings and 20/20 1400-byte Internet pings with zero packet
  loss, plus successful DNS resolution and Internet ping;
- an explicit disconnect/reconnect cycle, with Linux carrier changing
  `1 -> 0 -> 1`, the STA MAC changing from the random netdev address to
  `30:ed:a0:f3:d4:ac`, and 10/10 post-reconnect gateway pings;
- three consecutive hardware resets, each followed by INIT, the 17-frame
  self-test, `hci0` setup with identical HCI counters, and device
  registration;
- zero netdev RX errors and no malformed/reassembly/queue overflow reports.

The GetWifiMode response is `msg_type=Resp`, `msg_id=0x203`, with the expected
`ESP_ERR_WIFI_NOT_INIT` status before the Linux host API calls Wi-Fi init.
This proves the control request reaches the IDF Wi-Fi API rather than merely
being echoed by the SRAM layer.

OpenOCD telnet was used to locate two porting failures: the old polling task
yielded after Linux changed the shared tick environment, and the pserial TLV
parser trapped on its unaligned `uint16_t` load. Neither path uses GDB.
