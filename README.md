# Linux 6.12 for ESP32-S31

Linux port with Sv32 virtual memory, Supervisor mode, XIP, and a Buildroot
userspace, running on an ESP32-S31 microcontroller.

Module tested: ESP32-S31-WROOM-3 E1H16R16V (ESP32-S31 Core Board/Korvo).

<p align="center">
  <img src="docs/bootlog.png"
       alt="Linux 6.12 booted on an ESP32-S31 development board"
       width="850">
</p>

> **Experimental hardware bring-up project.**
> Definitely not something you want for production.

## Quick Start

Install `esptool`, Espressif's tool for flashing ESP32s:

```bash
$ pip install esptool
```

Then download the binaries in [Release](https://github.com/GrieferPig/esp32-s31-linux/releases), connect your board through USB-UART, and flash the board per the provided command below (change `/dev/ttyUSB0` to your actual serial device):

```bash
$ esptool -p /dev/ttyUSB0 -b 2000000 erase-flash
$ esptool -p /dev/ttyUSB0 -b 2000000 write-flash \
    --flash-mode dio --flash-freq 80m --flash-size 16MB \
    0x2000 bootloader.bin \
    0x8000 partition-table.bin \
    0x17000 ota_data_initial.bin \
    0x20000 hello_world.bin \
    0x220000 fw_payload.bin \
    0x2A0000 xipImage \
    0xA20000 rootfs.sqfs
```

## Porting progress

### General

| Feature | Status |
|---|---|
| Buildroot rootfs | 🟢 Stable |
| Reboot and poweroff | 🟢 Stable |
| Wireless (ESP-Hosted) | 🟡 Untested |
| Dual hart SMP | ⚫ Not Planned (Used by FreeRTOS; see FAQ) |

### Peripheral Drivers
| Feature | Status |
|---|---|
| AXI GDMA | 🟡 Untested |
| AHB GDMA | 🟡 Untested |
| Cache driver | 🟡 Untested |
| TRNG | 🟡 Untested |
| eFuse | 🟡 Untested |
| Watchdog | 🟡 Untested |
| PWM, counter, analog peripherals | 🟡 Untested |
| CLIC/CLINT interrupt driver | 🟡 Untested |
| Flash MTD driver | 🟡 Untested |
| Timers | 🟠 WIP |
| Clock tree | 🟠 WIP |
| Security accelerators | 🟠 WIP |
| LP subsystem & IPC | 🔴 Not Implemented |
| PMP/APM | 🔴 Not Implemented (properly) |


### Connectivity Drivers
| Feature | Status |
|---|---|
| UART0 console | 🟢 Stable |
| UART1/2 | 🟡 Untested |
| GMAC Ethernet | 🟡 Untested |
| SDMMC | 🟡 Untested |
| GPIO | 🟡 Untested |
| pinctrl/GPIO Matrix | 🟡 Untested |
| USB | 🟠 WIP |
| I2C | 🔴 Not Implemented |
| I2S | 🔴 Not Implemented |
| SPI | 🔴 Not Implemented |
| RMT | 🔴 Not Implemented |
| USB Serial/JTAG | ⚫ Not Planned (Used by FreeRTOS; see FAQ) |


> 🟢 **Stable** — Fully tested and working | 🟡 **Untested** — Seems working; not throughly tested | 🟠 **WIP** - Functions not fully implemented

## Build/Flash Instructions

Refer to the [Build Instructions](docs/build.md).

## S31 Quirks

(For more hardware references, see `docs/` folder)

This port was done before S31 TRM is available, therefore these guessworks were made:

### CLIC v. PLIC v. CLINT

S31 uses CLIC and CLINT similar to P4. Linux expects PLIC. Therefore a custom CLIC driver is needed. I referenced [this CLIC patch](https://github.com/litex-hub/linux-on-litex-vexriscv/pull/438) from [disdi](https://github.com/disdi) to get the CLIC working.

Also, standard RISC-V interrupt CSRs are not usable, presumably because, from P4's TRM, CLINT interrupts are routed to CLIC and `mtvec.MODE` is hardwired to `0x3` (CLIC mode). Patches needed to make OpenSBI interrupts work.

### S mode

S31's supervisor mode is not standard and has absolutely no usage in ESP-IDF so a lot of these CSR uses were mostly guessed from either P4's TRM or CSR probing (see `docs/`). For example, the use of `sclicbase(?)` and the lack of `sie`.

S31 implemented [SCLIC (Supervisor CLIC?)](https://esp32.com/viewtopic.php?t=48188) which is confusing since there is no known standardization; According to all laws of esp-idf, `mcliccfg.NMBITS` is not writable. ***IT IS WRITABLE!*** And setting it to `0b01` enables writes to the `clicintattr[i].MODE` field and thus enabling the use of S-mode interrupts.

### OpenSBI and Linux XIP

To save the *precious* 16MB PSRAM memory, OpenSBI was modified to use XIP in flash and internal SRAM (hence the `3915901 KB` firmware size in OpenSBI banner, since flash and SRAM mappings are not continuous).

In mainline linux, XIP support on RISC-V was removed, so 6.12 was used instead which has proper XIP support. 

## FAQ

### Why not SMP?

For several reasons:

- Espressif's radio firmware blobs are closed source, and must run within ESP-IDF's FreeRTOS framework. It's near impossible to reverse-engineer them (not to mention legal risks.)
- S31's two cores are kinda heterogeneous already: SIMD path only on hart 1. SMP makes scheduling things on the right core harder.
- PSRAM is already slow enough (compared to SRAM); two cores would share the same, tiny 32KiB D-cache.
- Cache maintenance, IPC, Interrupt routing, etc.
- I like having an RTOS for other tasks. If you want absolute performance, a low-end MPU (like `Allwinner T113-S3`) would be a far better choice

For this port, think S31 as a reincarnated `Boufallo BL808`.[^1]

### TODO

[^1]: I actually liked the BL808 and attempted to use it for a project, but the absurd lack of drivers is REAL BAD and made me appreciate Espressif's software support more

