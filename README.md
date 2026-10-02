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

## FAQs

### Vibe-coded?

I noticed folks on [Hacker News](https://news.ycombinator.com/item?id=49087499) questioning the use of AI-generated code. For transparency:

- Yes, it is heavily agent-assisted. It do work on real S31 dev boards (there's console output above and binary releases to prove that.) I understand the esp32 microcontroller architecture to some extent, but I barely know how to port Linux to other RISC-V platforms; what I did is to tell the agent something like "Go implement an IPC transport that uses a shared SRAM buffer and an IPC interrupt doorbell" or "sdmmc uses designware ip; search esp-idf usage and port the existing Linux driver over." An AI agent on its own would never discover S31's bespoke hardware behavior without my guidance, for example, that the register `mcliccfg` has writable bits, despite esp-idf saying otherwise. However I admit that AI assistance is the direct reason why I am able to progress this fast, and I did learn a lot about kernel development during the process.
