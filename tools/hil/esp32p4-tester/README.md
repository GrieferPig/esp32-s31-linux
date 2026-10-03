# ESP32-P4 HIL tester

This ESP-IDF application turns the Waveshare ESP32-P4-WIFI6-DEV-KIT into a
safe tester peer for ESP32-S31 Linux.  The CH343 UART0 console carries a
line-oriented `HIL1` JSON stream.  Tester GPIOs always boot as input/no-pull
and output commands require the per-boot token printed by `hil hello`; the
arm automatically expires after ten seconds.

The firmware performs these standalone checks without wiring an S31 board:

- P4 chip, dual HP cores, flash, heap, timer and safe GPIO state;
- P4-to-C6 ESP-Hosted-MCU SDIO readiness and coprocessor firmware version;
- a real C6 Wi-Fi scan without logging SSIDs;
- NimBLE HCI reset/synchronization through ESP-Hosted VHCI.

Output disarm is an implemented safety mechanism, but the standalone self-test
does not arm outputs and wait for expiry. Verify that behavior separately when
qualifying a fixture.

The standalone self-test leaves every lane high-Z and reports electrical
loopback as `SKIP`; connected-board patterns require host-coordinated lane RPC.

## Fixture wiring

Connect the following fixed lane map with a common ground. Verify compatible
signal voltages and board/module pin availability before connecting wires;
do not connect a powered output to another output. The same lanes are reused
by different bus cases, so run only one wired case at a time.

| Lane | ESP32-S31 pin | ESP32-P4 pin |
|---|---|---|
| L0 | GPIO42 | GPIO23 |
| L1 | GPIO43 | GPIO22 |
| L2 | GPIO44 | GPIO21 |
| L3 | GPIO45 | GPIO20 |

LP GPIO-wake tests need a separate connection. The runner defaults to
S31 GPIO0 and P4 GPIO2; change the runner's declared pins to match the actual
wiring. Do not route LP wake through the high-numbered four-lane fixture.

For the four-lane SPI fixture, the tester uses GPSPI3 in slave-DMA mode and
enables the SCLK pad's input hysteresis. This is required to reject narrow
ringing-induced edges at the qualified 20 MHz modes-1/2 operating point. The
mode-1/2 transmit-edge override also works around the ESP-IDF 6.2 P4-v1.3
slave setting that otherwise fragments transactions. Modes 0/3 at 20 MHz are
a known P4 slave-fixture limitation and are not evidence against the S31
master; the all-mode fixture gate remains the 14 MHz request point.

Build with ESP-IDF 6.2 and the committed component versions.  The defaults
explicitly select pre-v3 ESP32-P4 support for the v1.3 chip fitted to this
board; do not force-flash an image configured for P4 revision 3.x:

```sh
source /path/to/esp-idf/export.sh
idf.py -B build-hil set-target esp32p4
idf.py -B build-hil build
idf.py -B build-hil -p /dev/serial/by-id/<P4-CH343> flash
```

Useful commands at 115200 baud:

```text
hil hello
hil selftest
hil wifi-scan
hil wifi-ap-start S31-HIL-P4 s31hiltest 6
hil wifi-ap-report
hil wifi-ap-stop
hil gpio-read 20
hil gpio-input 20 none
hil lane-read 0
hil lane-input 0 none
hil arm <token-from-hello>
hil lane-write 0 1
hil gpio-write 20 1
hil gpio-wake 2 0 2000 500
hil disarm
```

`gpio-wake <gpio> <inactive-level> <delay-ms> <hold-ms>` provides a bounded
external wake stimulus for the S31 powered-suspend test. It first drives the
declared inactive level, switches to the opposite level after the delay, then
returns the pin to input/no-pull/high-Z after the hold time. The command accepts
only the tester's safe GPIO pool, requires the arm token, limits the delay to
250--8000 ms and the hold to 10--1000 ms, and remains subject to the ten-second
automatic disarm.

`wifi-ap-start` creates the HIL SoftAP through the onboard C6 and starts the
UDP echo service on `192.168.4.1:3334`. Use `-` as the password only for an
open-AP fault-isolation run. `wifi-ap-report` exposes active clients,
connect/disconnect events, packet and byte counts, input-pattern failures, and
echo failures. The normal WPA2 gate requires exactly 64 packets and 94208
bytes with both error counters at zero before `wifi-ap-stop` tears the fixture
down.

## Extended acceptance commands

`i2c-start [length] [scl-lane]` selects a 1–4096 byte register response
(default 16) and an optional SCL lane (default L0; L1 is reserved for SDA).
`i2c-report` includes the last received write length and CRC32, allowing an
independent comparison with `s31-hil-io i2c-long DEVICE LENGTH`.

`spi-master-start MODE LENGTH SPEED` drives the S31 SPI target after a
1500 ms receiver setup delay. It accepts modes 0–3, 1–4096 bytes and
10 kHz–5 MHz. The asynchronous `peer.spi-master-data` result checks every
received byte. Start the Linux target ioctl before this command. The Linux
`s31-hil-io spi DEVICE MODE SPEED LENGTH [BITS]` command accepts 8, 16 or
32-bit words and verifies the corresponding native-memory byte order.

Both bus commands require the current arm token. `peer-stop` releases their
resources and returns the fixture pins to high impedance.

The `wifi-sta-echo-start SSID PASSWORD` fixture command connects the
C6 to an external AP and starts
the UDP echo endpoint at `192.168.77.1:3334`; `-` selects an open network.
It can exercise an AP frontend when one is available, but the current S31
SoftMAC image exposes station mode only. An AP test setup must provide the
peer address `192.168.77.2/24`. Check
`wifi-sta-echo-report` for association, exact packet counts and zero errors,
then call `wifi-sta-echo-stop`.
