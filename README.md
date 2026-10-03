# Linux 6.18 for ESP32-S31

MMU RV32 Linux running natively on an ESP32-S31 microcontroller.

Target module: ESP32-S31-WROOM-3 E1H16R16V (ESP32-S31 Coreboard/Korvo).
Hardware and emulator validation of the current merged image are pending.
Persistent-flash erase and LP firmware remain unresolved validation limitations;
working persistence and a successful normal boot have not been established.

<p align="center">
  <img src="https://raw.githubusercontent.com/GrieferPig/esp32-s31-linux-docs/main/bootlog.png"
       alt="Historical Linux boot log on an ESP32-S31 development board"
       width="850">
</p>

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
outputs and retains caches. Set host paths and jobs in `local.mk`.

The full kernel uses size optimization and unused-export trimming. Its XIP
image must fit the fixed 6 MiB kernel slot. Every build enforces the size limit;
no peripheral is silently removed. Inspect the verified `dist/current` artifacts
for the size and remaining headroom of a particular build.

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

These statuses describe implementation and build integration. Runtime validation
of the current image remains pending for every feature below.

### Legend

| Status | Meaning |
|---|---|
| 🟢 Integrated | Implemented and included in the full board build |
| 🟡 Experimental | Implementation available; limitations and runtime validation remain |
| 🟠 WIP | Driver exists, but full functionality is work in progress |
| 🔴 Unsupported | Not implemented or supported |

### System

| Feature | Status |
|---|---|
| Linux, Sv32 MMU, and flash XIP | 🟢 Integrated |
| Dual-core SMP | 🟢 Integrated |
| Read-only SquashFS root | 🟢 Integrated |
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
| GPIO and UART | 🟢 Integrated |
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

- Yes, it is heavily agent-assisted. I understand the esp32 microcontroller architecture to some extent, but I barely know how to port Linux to other RISC-V platforms; what I did is to tell the agent something like "Go implement an IPC transport that uses a shared SRAM buffer and an IPC interrupt doorbell" or "sdmmc uses designware ip; search esp-idf usage and port the existing Linux driver over." An AI agent on its own would never discover S31's bespoke hardware behavior without my guidance, for example, that the register `mcliccfg` has writable bits, despite esp-idf saying otherwise. However I admit that AI assistance is the direct reason why I am able to progress this fast, and I did learn a lot about kernel development during the process.
