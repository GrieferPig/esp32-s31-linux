# Linux 6.18 for ESP32-S31

MMU RV32 Linux running natively on an ESP32-S31 microcontroller.

Module tested: ESP32-S31-WROOM-3 E1H16R16V (ESP32-S31 Core Board/Korvo).

<p align="center">
  <img src="https://raw.githubusercontent.com/GrieferPig/esp32-s31-linux-docs/main/bootlog.png"
       alt="Linux booted on an ESP32-S31 development board"
       width="850">
</p>

> **WARNING: Experimental**
> Definitely not something you want for production.

## Quick Start

Install `esptool`, Espressif's tool for flashing ESP32s:

```bash
$ pip install esptool
```

Download all six slot images from the same [release](https://github.com/GrieferPig/esp32-s31-linux/releases)
and verify them against its `SHA256SUMS`. Replace `BOARD_PORT` with the board's
download port. On first installation only, run `esptool -p BOARD_PORT erase-flash`.
An erased persist partition is a valid empty JFFS2 filesystem.

For both first installation and subsequent updates, write the six slots:

```sh
esptool -p BOARD_PORT -b 460800 write-flash \
    --flash-mode dio --flash-freq 80m --flash-size 16MB \
    0x002000 spl_app.bin \
    0x100000 u-boot.itb \
    0x300000 esp32s31_generic.dtb \
    0x310000 radio.sqfs \
    0x500000 xipImage \
    0xBD0000 rootfs.sqfs
```

For updates, omit `erase-flash`. The six-slot write preserves persist and HIL
scratch. The separately supplied `s31_full_flash.bin` is convenient for first
installation at offset zero; writing it overwrites persist even without a
separate persist payload. Older releases containing only that combined image
cannot use the six-slot procedure without extracting their slots first.

The authoritative addresses and artifact names are kept in
[`configs/esp32s31-layout.cfg`](configs/esp32s31-layout.cfg). A source checkout
can build and update a board with `make all` and
`make flash-all`; `flash-all` deliberately preserves the persist partition.

## Porting progress

### General

| Feature | Status |
|---|---|
| Buildroot rootfs | 🟢 Stable |
| Reboot | 🟢 Stable |
| Poweroff | 🟡 Experimental — orderly shutdown into untimed PMU deep sleep; board power measurement pending |
| Linux native wireless | 🟡 Experimental |
| - WiFi | 🟡 Experimental |
| - Bluetooth controller / Classic A2DP | 🟡 Experimental |
| Dual core SMP | 🟡 Experimental |
| CPU frequency / idle | 🟡 Experimental — shared OPPs and guarded SMP WFI |
| Suspend / resume | 🟡 Experimental — Wi-Fi STA timer-wake/reconnect passes; AP needs userspace restart, Bluetooth recovery remains unverified |

### Peripheral Drivers

> The table is a support-level summary. Interfaces, behavior and limitations
> are defined in the [technical reference manual](https://grieferpig.github.io/esp32-s31-linux-docs/).

| Feature | Status |
|---|---|
| AXI GDMA | 🟡 Experimental |
| AHB GDMA | 🟡 Experimental |
| Cache driver | 🟡 Experimental |
| TRNG | 🟡 Experimental |
| eFuse | 🟡 Experimental |
| Watchdog | 🟡 Experimental |
| PWM, counter, analog peripherals | 🟡 Experimental |
| CLIC/CLINT interrupt driver | 🟡 Experimental |
| Flash MTD driver | 🟡 Experimental |
| Timers | 🟡 Experimental |
| Clock tree | 🟡 Experimental |
| Security accelerators | 🟡 Experimental — AES, SHA, RSA and ECDH drivers |
| LP subsystem & IPC | 🟡 Experimental — remoteproc and mailbox ABI v1 |
| PMP/APM | 🟠 WIP |


### Connectivity Drivers
| Feature | Status |
|---|---|
| UART0 console | 🟢 Stable |
| UART1/2/3 | 🟡 Experimental |
| GMAC Ethernet | 🟡 Experimental |
| SDMMC | 🟡 Experimental |
| GPIO | 🟡 Experimental |
| pinctrl/GPIO Matrix | 🟡 Experimental |
| USB | 🟡 Experimental |
| I2C | 🟡 Experimental |
| I2S | 🟡 Experimental |
| GPSPI | 🟡 Experimental |
| TWAI / CAN-FD | 🟡 Experimental |
| RMT | 🔴 Not Implemented |
| USB Serial/JTAG | 🟡 Experimental |


> 🟢 **Stable** — Default supported path | 🟡 **Experimental** — Interface is
> available with documented limitations | 🟠 **WIP** — Partial interface |
> 🔴 **Not Implemented** — No supported interface

Wi-Fi AP/AP+STA, receive-only monitor and firmware PEAP/EAP-TLS provisioning,
long I2C messages, DMA-backed SPI target transfers and configurable I2S/TDM
DAIs are implemented with partial board acceptance. Open AP+STA passes both
data paths in historical fixture tests. I2S has historical S16_LE stereo
evidence at 8/16/48 kHz. Those observations do not certify a changed image;
Bluetooth suspend recovery and enterprise authentication require identified
acceptance records. See the
[advanced Wi-Fi guide](https://grieferpig.github.io/esp32-s31-linux-docs/en/api-guides/wifi-advanced.html),
[peripheral reference](https://grieferpig.github.io/esp32-s31-linux-docs/en/api-reference/peripherals/) and
[support matrix](https://grieferpig.github.io/esp32-s31-linux-docs/en/resources/support-matrix.html) for limits. Radio core
ABI v1 requires a matching payload ABI v1; rebuild and deploy them together.

## Build Instructions

Refer to the [Build Instructions](https://grieferpig.github.io/esp32-s31-linux-docs/en/get-started/build-from-source.html).

The full document index is the
[ESP32-S31 Linux Technical Reference Manual](https://grieferpig.github.io/esp32-s31-linux-docs/).

## Standard Linux userspace

The root filesystem uses the upstream Linux control planes for native S31
drivers: `wpa_supplicant`/`wpa_cli` for Wi-Fi, a BTstack A2DP sink plus BLE
GATT peripheral over `/dev/s31-hci` for Bluetooth, and libgpiod 2.x for GPIO
character devices. Its direct H4 transport is self-contained; BlueZ and its
D-Bus/GLib audio-control dependency chain are not selected. See
[Standard userspace interfaces](https://grieferpig.github.io/esp32-s31-linux-docs/en/api-reference/userspace/).

Run `esp32-config` for the `dialog`-based configuration interface. Persistent
policy lives in `/etc/esp32-conf`, while radio and GPIO operations continue
to use the standard Linux tools above. See the
[esp32-config reference](https://grieferpig.github.io/esp32-s31-linux-docs/en/resources/esp32-config.html).

## Architecture notes

The HP harts use native CLIC interrupt delivery. Linux runs in S-mode with
native S-mode IPI and SYSTIMER paths, while OpenSBI retains M-mode boot, HSM and
reset services. Linux 6.18 executes its XIP text from flash and keeps writable
state in RAM. See [System architecture](https://grieferpig.github.io/esp32-s31-linux-docs/en/api-reference/system/overview.html),
[Interrupts and SMP](https://grieferpig.github.io/esp32-s31-linux-docs/en/api-reference/system/interrupts-smp.html), and
[Boot and storage](https://grieferpig.github.io/esp32-s31-linux-docs/en/api-reference/system/boot-chain.html) for the current contracts.

## FAQ

### ~Why not SMP?~

Edit: *SMP support is added.* Espressif's radio blobs exposes a set of OSI (OS interfaces). Radio support is accomplished by emulating a compatible OSI using Linux kthreads. 

### Vibe-coded?

I noticed folks on [Hacker News](https://news.ycombinator.com/item?id=49087499) questioning the use of AI-generated code. For transparency:

- Yes, it is heavily agent-assisted. It do work on real S31 dev boards (there's console output above and binary releases to prove that.) I understand the esp32 microcontroller architecture to some extent, but I barely know how to port Linux to other RISC-V platforms; what I did is to tell the agent something like "Go implement an IPC transport that uses a shared SRAM buffer and an IPC interrupt doorbell" or "sdmmc uses designware ip; search esp-idf usage and port the existing Linux driver over." An AI agent on its own would never discover S31's bespoke hardware behavior without my guidance, for example, that the register `mcliccfg` has writable bits, despite esp-idf saying otherwise. However I admit that AI assistance is the direct reason why I am able to progress this fast, and I did learn a lot about kernel development during the process.
