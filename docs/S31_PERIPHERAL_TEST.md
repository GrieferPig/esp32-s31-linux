# ESP32-S31 Dynamic Peripheral Configuration and Testing

The system builds a single base device tree, `esp32s31_generic.dtb`.
Peripherals that may conflict over pins, clocks, interrupts, or other hardware
resources are enabled at runtime with device-tree overlays. `S31_PROFILE` is no
longer used, and switching peripherals does not require rebuilding Linux or
OpenSBI.

The in-kernel `/dev/s31-overlay` manager permits multiple non-conflicting
overlays to remain active at the same time. Before loading an overlay, it checks
physical GPIO assignments, GPIO-matrix input signals, exclusive resources such
as `sdmmc-host`, and shared interrupt sources. A conflict returns `EBUSY` and
identifies the conflicting owner in the kernel log.

Applying an already active overlay performs an atomic reassignment: the manager
removes the old instance, applies the replacement, and restores the old instance
if the replacement fails. When a device is unbound, pinctrl returns its GPIOs to
a high-impedance safe state and releases GPIO-matrix and dedicated GMAC/SDIO
pinmux selections.

## Overlay Commands

List the available configurations:

```sh
s31-overlay list
```

Apply and persist multiple non-conflicting configurations:

```sh
s31-overlay apply timers
s31-overlay apply gdma
s31-overlay status
```

Apply a temporary configuration. After reboot, the previously persisted set is
restored:

```sh
s31-overlay apply pwm-counter --volatile
```

Remove one configuration or all configurations:

```sh
s31-overlay remove gdma
s31-overlay remove --all
```

Overlays with GPIO-matrix routes accept GPIO assignments at load time. Inspect
the configurable routes first:

```sh
s31-overlay routes uart1
s31-overlay routes pwm-counter
```

Then load or dynamically reassign the overlay. Routes not listed on the command
line retain their DTBO defaults:

```sh
s31-overlay apply uart1 uart1.tx=35 uart1.rx=36
s31-overlay apply uart1 uart1.tx=46 uart1.rx=47
s31-overlay apply pwm-counter ledc0.out=38 pcnt0.in=39
```

GPIO26 through GPIO32 carry the active XIP flash bus. GPIO33, GPIO34, and
GPIO41 are invalid pads according to ESP-IDF, and GPIO58/GPIO59 are owned by the
Linux UART0 console. The configuration utility rejects all of these pins.

The following conflicts return `Device or resource busy`:

- two active overlays assigning the same physical GPIO;
- two GPIO-matrix inputs assigning the same input signal;
- SDMMC0 and SDMMC1 assigning the same host;
- a dynamic reassignment moving a route onto a pad owned by another overlay;
- AHB-GDMA and the analog comparator, which share HP interrupt-matrix source 22.

GPIO-matrix output signals may fan out in hardware, but each output pad still
has exactly one owner.

GMAC RGMII, SDMMC, ADC, touch, and comparator signals use dedicated IO-MUX pads
defined by ESP-IDF. They cannot be moved freely like GPIO-matrix signals. The
two valid SDMMC IO-MUX layouts are represented by the `sdmmc0` and `sdmmc1`
DTBOs. Dedicated GMAC and SDMMC selections are configured and released
automatically during device probe and removal.

## Persistent Configuration

`persist` is a 1.375 MiB MTD partition between the OpenSBI and Linux partitions.
It starts at flash offset `0x2A0000` and has size `0x160000`. At boot it is
mounted as JFFS2 and used as the overlayfs upper and work storage.

Persistent program configuration is stored in the merged root filesystem under
`/etc/s31-conf/*.conf` as simple `key=value` lines. Overlay state is stored in
`/etc/s31-conf/s31-overlay.conf`. The read-only root filesystem installs DTBOs
under `/usr/lib/s31-overlays`. The `S03s31-overlay` startup script restores the
persisted overlay set early in userspace startup.

## Available Test Profiles

| Overlay | Enabled hardware | Software test |
| --- | --- | --- |
| `timers` | SYSTIMER and RTC timer | Verify that counters increase |
| `pwm-counter` | LEDC0/1, MCPWM0-3, PCNT0/1, and Sigma-Delta | Verify registration counts and PWM apply operations |
| `gdma` | AHB-GDMA | Run one 64 KiB `dmatest` iteration with randomized offsets and length |
| `analog` | ADC, temperature sensor, touch controller, and analog comparator | Exercise IIO/hwmon samples and event interfaces |
| `gmac` | GMAC, up to 1 Gbit/s | Verify interface registration; link testing requires an external PHY |
| `sdmmc0` | SDMMC slot 0 | Build and probe validation only; no card access |
| `sdmmc1` | SDMMC slot 1 | Build and probe validation only; no card access |
| `uart1` | UART1 with dynamically assigned TX/RX matrix GPIOs | Verify `/dev/ttyS1` registration |
| `uart2` | UART2 with dynamically assigned TX/RX matrix GPIOs | Verify `/dev/ttyS2` registration |

After applying overlays, run all tests that correspond to the active set:

```sh
s31-peripheral-test
```

To test a specific active profile explicitly:

```sh
s31-peripheral-test gdma
```

The base device tree always includes TIMG0, TIMG1, and RTC watchdogs. Every run
performs non-destructive registration, ping, and magic-close checks for these
watchdogs.

## Persistence Test

```sh
s31-overlay apply timers
s31-overlay apply gdma
s31-overlay apply uart1 uart1.tx=35 uart1.rx=36
s31-overlay status
reboot

# After reboot
s31-overlay status
s31-peripheral-test
s31-overlay remove --all
```

After reboot, the active set should still contain `timers`, `gdma`, and `uart1`
with its original GPIO assignments. The counters, GDMA test, and `/dev/ttyS1`
should remain operational. The final `remove --all` command persists an empty
overlay set.

## Tests That Require Additional Hardware

- **GMAC at 1 Gbit/s:** requires an RGMII PHY, correct clock and delay wiring, a
  cable, and a 1000BASE-T peer. Without an attached link, only driver probe and
  interface registration can be verified.
- **LEDC, MCPWM, and Sigma-Delta:** sysfs configuration can be verified in
  software. Frequency, duty cycle, and Sigma-Delta waveforms require an
  oscilloscope or logic analyzer connected to the overlay-selected output pins.
- **PCNT:** requires an external pulse source or a loopback wire from a PWM
  output to a PCNT input.
- **Analog comparator:** requires input voltages that cross the reference level;
  rising and falling events are read through the IIO event interface.
- **ADC accuracy:** requires a known DC voltage source and a common ground to
  check range, linearity, and calibration.
- **Touch:** GPIO14/channel 8 has completed conversions successfully. Sensitivity,
  thresholds, and physical touch behavior require a suitable electrode. Some
  unconnected channels on the current sample never assert `meas_done`; reads from
  those channels return `ETIMEDOUT`.
- **SDMMC0/1:** card access was intentionally not performed. Future testing
  requires a socket wired for the selected slot, power, pull-ups,
  card-detect/write-protect wiring, and an SD card.
- **Watchdog reset:** the non-destructive test never intentionally resets the
  board. Reset validation requires permission to stop feeding the watchdog and
  accept a full board restart.

GPTimer and RMT are outside the current porting scope and have no overlays.

## Hardware Test Results

Flashing and serial testing were performed through `/dev/ttyUSB0`. The following
behaviors were verified on hardware:

- coexistence of multiple non-conflicting DTBOs;
- rejection of physical GPIO, GPIO-matrix input, exclusive host, and interrupt
  source conflicts;
- transactional GPIO reassignment of an active DTBO, including rollback after a
  failed reassignment;
- persistence of DTBO configuration in JFFS2 and automatic restoration across a
  reboot;
- TIMG0, TIMG1, and RTC watchdog ping and magic-close operations;
- increasing SYSTIMER and RTC timer counters;
- registration of seven PWM controllers and two PCNT controllers, plus PWM
  apply operations;
- ADC one-shot conversion, temperature samples, touch channel 8 conversion, and
  the comparator event interface;
- GMAC probe and removal in DWMAC1000 mode;
- one successful 64 KiB AHB-GDMA `dmatest` iteration with zero failures, using
  the ESP-IDF-compatible 12-byte descriptor layout.

The default PWM/counter routes use GPIO20 through GPIO25 and GPIO35 through
GPIO37. An early test placed Sigma-Delta and PCNT routes on GPIO26 through
GPIO28. Hardware testing showed that changing those pads away from the flash
IO-MUX immediately interrupted XIP instruction fetches. The final implementation
uses safe pins and permanently rejects GPIO26 through GPIO32 in both validation
layers.

SDMMC card access was not performed, as required. GMAC line-rate testing,
physical PWM/Sigma-Delta waveform validation, external PCNT counting, comparator
threshold crossing, ADC accuracy, and watchdog reset testing still require the
external hardware or destructive-reset permission described above.
