# Linux 6.18 for ESP32-S31

MMU RV32 Linux running natively on an ESP32-S31 microcontroller.

Module tested: ESP32-S31-WROOM-3 E1H16R16V (ESP32-S31 Coreboard/Korvo).

<p align="center">
  <img src="https://raw.githubusercontent.com/GrieferPig/esp32-s31-linux-docs/main/bootlog.png"
       alt="Linux booted on an ESP32-S31 development board"
       width="850">
</p>

> **WARNING: Experimental**
> Definitely not something you want for production.

## Quick Start

Flash the precompiled image in [Releases](https://github.com/GrieferPig/esp32-s31-linux/releases). For more info, visit the [get started guide](https://grieferpig.github.io/esp32-s31-linux-docs/en/get-started/).

## Documentation

[Over here](https://grieferpig.github.io/esp32-s31-linux-docs/en/get-started/)

### Building only U-Boot

With the normal host build tools, the RISC-V toolchain and the U-Boot/OpenSBI
submodules prepared, `make uboot` does not require an ESP-IDF installation or
an activated ESP-IDF environment. Install standalone
[esptool](https://docs.espressif.com/projects/esptool/en/latest/esp32s31/installation.html)
with ESP32-S31 support on `PATH`, or use
`make uboot ESPTOOL=/absolute/path/to/esptool`.
This does not remove ESP-IDF requirements from full S31 image builds, which
also build the radio and LP firmware.

The ESP32-P4 rev1.3 bootloader target uses the same standalone wrapper:

```sh
make uboot TARGET=esp32p4 P4_CROSS_COMPILE=/path/to/bin/riscv32-esp-elf-
```

It writes the ROM-loadable `spl-p4-80.bin` and `uboot-p4.bin` to
`out-p4/images/`. To package an existing P4 Linux Image (including its
initramfs) and matching DTB, use
`make p4-fit TARGET=esp32p4 P4_LINUX_IMAGE=... P4_LINUX_DTB=...`.
The FIT accepts a Linux Image of up to 7 MiB in flash and an in-memory
extent of up to 14 MiB including BSS. Use `make p4-linux TARGET=esp32p4`
to build the P4 kernel, Waveshare DTB and XZ-compressed Buildroot
initramfs. The build targets generate files in `out-p4/images/`.
The native P4 SPL selects 512 KiB of shared L2 with 128-byte lines. The SPL
and U-Boot proper fit in low SRAM; the runtime heap, stack and working DTB
use trained PSRAM. On ESP32-P4 rev1.3 the 80 MHz PSRAM configuration boots
the verified FIT automatically after a two-second countdown. Press a key
during the countdown for the U-Boot prompt.
`P4_PSRAM_MHZ=200` selects the experimental high-speed PSRAM build.
The ROM flash mode and clock default to DIO/40 MHz and are independent of the
PSRAM profile (`P4_FLASH_MODE` and `P4_FLASH_FREQ`).

## Porting progress

For a detailed overview of the porting progress, refer to [the support matrix](https://grieferpig.github.io/esp32-s31-linux-docs/en/resources/support-matrix.html).

### Legend

| Status | Meaning |
|---|---|
| 🟢 Stable | Fully supported and tested |
| 🟡 Experimental | Supported and mostly working; may have limitations or require further testing |
| 🟠 WIP | Driver exists, but full functionality is work in progress |
| 🔴 Unsupported | Not implemented or supported |

### System

| Feature | Status |
|---|---|
| Linux, Sv32 MMU, and flash XIP | 🟢 Stable |
| Dual-core SMP | 🟢 Stable |
| Persistent root filesystem | 🟢 Stable |
| LP firmware and mailbox | 🟡 Experimental |
| Power management | 🟠 WIP |

### Radio

| Feature | Status |
|---|---|
| Wi-Fi station | 🟢 Stable |
| Bluetooth (via BTstack) | 🟢 Stable |

### Peripherals

| Feature | Status |
|---|---|
| GPIO and UART | 🟢 Stable |
| I2C0/I2C1 | 🟡 Experimental |
| GPSPI2/GPSPI3 host | 🟡 Experimental |
| GPSPI target | 🟡 Experimental |
| I2S/TDM | 🟡 Experimental |
| SD/MMC | 🟡 Experimental |
| Ethernet | 🟡 Experimental |
| USB gadget | 🟡 Experimental |
| AHB/AXI GDMA | 🟡 Experimental |
| Timers, PWM, and pulse counter | 🟡 Experimental |
| Analog and sensor blocks | 🟡 Experimental |
| Watchdog, NVMEM, RNG, and crypto | 🟡 Experimental |
| TWAI/CAN | 🟠 WIP |
| USB host | 🟠 WIP |
| RMT | 🔴 Unsupported |

## FAQs

### Vibe-coded?

I noticed folks on [Hacker News](https://news.ycombinator.com/item?id=49087499) questioning the use of AI-generated code. For transparency:

- Yes, it is heavily agent-assisted. It do work on real S31 dev boards (there's console output above and binary releases to prove that.) I understand the esp32 microcontroller architecture to some extent, but I barely know how to port Linux to other RISC-V platforms; what I did is to tell the agent something like "Go implement an IPC transport that uses a shared SRAM buffer and an IPC interrupt doorbell" or "sdmmc uses designware ip; search esp-idf usage and port the existing Linux driver over." An AI agent on its own would never discover S31's bespoke hardware behavior without my guidance, for example, that the register `mcliccfg` has writable bits, despite esp-idf saying otherwise. However I admit that AI assistance is the direct reason why I am able to progress this fast, and I did learn a lot about kernel development during the process.
