# Linux 6.18 for ESP32-S31

MMU RV32 Linux running natively on an ESP32-S31 microcontroller.

Target module: ESP32-S31-WROOM-3 E1H16R16V (ESP32-S31 Coreboard/Korvo).
Hardware validation of the current image is pending. The emulator reaches
read-only recovery because persistent-flash erase fails; this does not
establish working persistence or a successful normal boot.

> **WARNING: Experimental**
> Definitely not something you want for production.

## Quick Start

Flash the precompiled image in [Releases](https://github.com/GrieferPig/esp32-s31-linux/releases). For more info, visit the [get started guide](https://grieferpig.github.io/esp32-s31-linux-docs/en/get-started/).

## Build locally

GNU Make is the public build interface. Start with `make help` and `make doctor`,
then run `make fetch` to prepare pinned dependencies and `make image` to build.
Every build uses the full board configuration, including all peripheral drivers.
`DEBUG=1` adds diagnostic information without changing the feature set.

Native component outputs live in `out/{linux,u-boot,opensbi,buildroot,radio,lp}`;
generated configuration, staging, reports, and final `images/` also live in `out/`.
Downloads and toolchains live in `cache/`; `make clean` removes the current build
outputs and retains caches. Existing historical output directories are never
used as inputs or migrated automatically. Set host paths and jobs in `local.mk`.

The full kernel uses size optimization and unused-export trimming. Its current
6,282,188-byte XIP image fits the 6 MiB kernel slot with only 9,268 bytes spare.
Every build enforces the size limit; no peripheral is silently removed.

`make image` publishes a verified matched set atomically under `dist/`, with
`dist/current` pointing to the complete immutable result. The manifest
binds the kernel, radio module, payload, import contract, and radio image hashes.
A packaging command never silently builds missing inputs.

Device operations are separate: `make flash-existing-all PORT=/dev/ttyUSB0`
uses only the verified immutable `dist/current` image set and preserves persist on an unchanged
layout. Partial updates fail closed because the installed companions are unknown.
The combined installation image overwrites persist. Build and host validation do
not establish a successful hardware boot.

## Documentation

[Over here](https://grieferpig.github.io/esp32-s31-linux-docs/en/get-started/)

## Porting progress

For a detailed overview of the porting progress, refer to [the support matrix](https://grieferpig.github.io/esp32-s31-linux-docs/en/resources/support-matrix.html).

### Legend

| Status | Meaning |
|---|---|
| 🟢 Stable | Fully supported and tested |
| 🟡 Experimental | Supported; may have limitations or require further testing |
| 🟠 WIP | Driver exists, but full functionality is work in progress |
| 🔴 Unsupported | Not implemented or supported |

### System

| Feature | Status |
|---|---|
| Linux, Sv32 MMU, and flash XIP | 🟢 Stable |
| Dual-core SMP | 🟢 Stable |
| Read-only SquashFS root | 🟢 Stable |
| Writable persistent overlay | 🟡 Experimental; erase/write validation pending |
| LP firmware and mailbox | 🟡 Experimental |
| Power management | 🟠 WIP |

### Radio

| Feature | Status |
|---|---|
| Wi-Fi station | 🟡 Experimental |
| Bluetooth (via BTstack) | 🟡 Experimental |

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
