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
| 🟡 Experimental | Supported; may have limitations or require further testing |
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
