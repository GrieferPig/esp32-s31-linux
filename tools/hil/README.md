# ESP32-S31 hardware-in-the-loop tests

The complete test contract, safety rules, commands, and evidence interpretation
are documented in
[`docs/en/contribute/testing-hil.md`](../../docs/en/contribute/testing-hil.md). Implementation status is recorded in
[`docs/en/resources/support-matrix.md`](../../docs/en/resources/support-matrix.md).
Keep local acceptance records separate from the reference documentation.

The HIL system has three independently verifiable layers:

- `s31-hil-agent` in the Buildroot rootfs validates S31 firmware and runs
  SDMMC, USB host, MTD/XIP, LP-core, SMP/IRQ/GDMA, and fixture cleanup. Its
  `HIL1` JSON lines label probe, electrical, data, and destructive evidence
  separately.
- `esp32p4-tester` is the Waveshare P4 firmware.  It keeps every fixture GPIO
  high-Z until a per-boot arm token is supplied and automatically disarms.
- `s31_hil.py` drives both serial consoles and stores the machine-readable
  results on the host.

Build and flash the standard kernel, rootfs, and radio bundle as a matched set:

```sh
S31_LEAN_RADIO=0 make -j8 linux rootfs radio-fs
```

The implemented P0-P2 cases, excluding USB device/gadget mode, are:

```text
gpio uart i2c spi pwm-pcnt i2s c6-wifi c6-ble
ethernet sdmmc usb-drive mtd lp-core smp-irq-dma
```

The four-wire cases use the guarded P4 responders and receiver-first host
sequencer. Run each independently and save its JSON result:

```sh
tools/hil/s31_hil.py --board both --case gpio --output logs/hil-gpio.json
tools/hil/s31_hil.py --board both --case uart --output logs/hil-uart.json
tools/hil/s31_hil.py --board both --case i2c --repeat 12 \
  --output logs/hil-i2c-repeat.json
tools/hil/s31_hil.py --board both --case spi --output logs/hil-spi.json
tools/hil/s31_hil.py --board both --case i2s --output logs/hil-i2s.json
tools/hil/s31_hil.py --board both --case pwm-pcnt \
  --output logs/hil-pwm-pcnt.json
```

I2C defaults to 100 kHz. The I2C overlays and runner also expose the validated
400 kHz and 1 MHz timing points, for example:

```sh
tools/hil/s31_hil.py --board both --case i2c --i2c-speed 1000000 \
  --repeat 10 --output logs/hil-i2c-1mhz.json
```

The standard SPI and I2S cases are bounded functional smoke tests. Keep their
longer payload characterization separate:

```sh
tools/hil/s31_hil.py --board both --case spi-stress \
  --output logs/hil-spi-stress.json
tools/hil/s31_hil.py --board both --case i2s-stress \
  --output logs/hil-i2s-stress.json
```

SPI stress defaults to 4096-byte full-duplex transfers in all four modes at a
5 MHz request rate. Earlier fixture characterization used a 14 MHz request
(13.333 MHz effective with the 80 MHz GPSPI parent). That historical result
does not change the CLI default. Override the rate explicitly for a boundary
run; for example, the earlier 20 MHz modes-1/2 configuration was:

```sh
tools/hil/s31_hil.py --board both --case spi-stress \
  --spi-stress-speed 20000000 --spi-stress-length 4096 \
  --spi-stress-modes 1,2 \
  --output logs/hil-spi-20mhz.json
```

The ESP32-P4 v1.3 slave fixture cannot validate modes 0/3 at 20 MHz: the same
fragmented transactions occur with the S31 running the ESP-IDF master baseline
and persist across both P4 GPSPI instances and internal-edge sweeps. Keep the
historical all-mode acceptance at a 14 MHz request. At exact 20 MHz, modes 1/2
are the hardware-qualified gate and require the tester's SCLK input hysteresis.
For cache and direction diagnosis, `spi_direction_diag.py` additionally checks
the P4 bit length and both endpoint CRCs on each individual transfer.
The diagnostic accepts requests through 40 MHz. On the current loose-jumper
P4-v1.3 fixture, 40 MHz is validated only from S31 MOSI to P4 (20/20 exact
4096-byte transfers across GPSPI2/3); bidirectional acceptance remains 20 MHz.

I2S stress sends 32 KiB fixtures and requires an exact 16 KiB playback
window without resynchronization, plus at least 16 KiB of contiguous matching
capture. Guard data allows the slave to join the running frame clock; startup
samples are outside this check. Select `--i2s-rate 8000`, `16000`, `32000` or
`48000` (default 8000). Both controllers have passed at 8/16/48 kHz, S16_LE,
stereo. These bounded checks do not establish extended endurance or other
DAI formats. See `docs/en/resources/support-matrix.md` for the accepted scope.

With the direct Ethernet cable connected, `--board both` starts the P4 IP101
peer and tests carrier, ICMP, UDP payloads, MTU, link loss, and recovery:

```sh
tools/hil/s31_hil.py --board both --case ethernet \
  --output logs/hil-ethernet.json
```

The S31 SDMMC slot has its own read-only case. It dynamically selects the
1-bit `sdmmc0` overlay parameter for the CLK/CMD/DAT0 fixture, reads 1 MiB
from an inserted card, then removes the temporary overlay:

```sh
tools/hil/s31_hil.py --board s31 --case sdmmc
```

After a USB drive is inserted into the S31 DWC2 host port, first run a
read-only check.  Write testing is opt-in and creates then removes one 64 KiB
probe file:

```sh
tools/hil/s31_hil.py --board s31 --case usb-drive
tools/hil/s31_hil.py --board s31 --case usb-drive --allow-usb-write
```

The P4/C6 Wi-Fi case creates WPA2 SoftAP `S31-HIL-P4` on the fixture itself.
It uses only an ephemeral S31 profile, temporarily pauses BTstack, then checks
association, DHCP, ICMP, and 64 exact 1472-byte UDP uplink/echo-downlink
packets. The P4 counters must report 94208 bytes and zero pattern or echo
errors. `--wifi-ap-open` is available only to isolate authentication faults:

```sh
tools/hil/s31_hil.py --board both --case c6-wifi --peer-connected \
  --output logs/hil-c6-wifi.json
tools/hil/s31_hil.py --board both --case c6-wifi --wifi-ap-open \
  --peer-connected --output logs/hil-c6-wifi-open.json
tools/hil/s31_hil.py --board both --case c6-ble --peer-connected \
  --output logs/hil-c6-ble.json
tools/hil/s31_hil.py --board s31 --case mtd --output logs/hil-mtd.json
tools/hil/s31_hil.py --board s31 --case lp-core --output logs/hil-lp-core.json
tools/hil/s31_hil.py --board s31 --case smp-irq-dma \
  --output logs/hil-smp-irq-dma.json
```

The Wi-Fi finalizer removes `/tmp` profiles and the volatile radio overlay,
restores the pre-test BT service, and verifies that persistent Wi-Fi remains
disabled with no profile. `c6-wifi-recover` is retained only for backup files
created by the older reboot-persistent workflow.

After a formal run, retain a sanitized acceptance summary with its build
identity, result, fixture and cleanup evidence in your local test records. Update
`docs/en/resources/support-matrix.md` only if implementation status or a known
limit changed. Keep failures and skips in the acceptance record. USB device/
gadget mode is outside the standard suite; `usb-drive` covers USB host mode.

For Wi-Fi-only suspend/reconnect acceptance, add `--wifi-suspend-cycles N`
to the volatile `--case c6-wifi` run (1–20 cycles). Each cycle checks the
boot identity, a 128 KiB RAM checksum, both online harts, radio readiness,
userspace reassociation, ICMP and 256 exact 1472-byte UDP echoes. This checks
reconnection after timer wake, not a retained association or WoWLAN. It does
not establish AP, Bluetooth or combo-mode recovery.


## Host transport and result checks

Serial transports live in `serial_transport.py`; the orchestrator owns test
sequencing and board cleanup. Peer ports share one cleanup scope, including
failures while opening the second port. Windows RPCs have bounded replies;
the bridge reports its Windows PID before opening the COM port so timeout
cleanup can terminate the actual worker as well as the WSL relay.

`HIL1` records require a recognized status (`PASS`, `FAIL`, or `SKIP`) and
nonempty board, test, and level fields. S31 summaries identify the requested
case. Malformed records and mismatched summaries fail the run; saved counts
retain skips separately and exclude summary records.

Run the host regressions with:

```sh
S31_TEST_SANITIZERS=1 python3 -m unittest discover -s tools/tests -v
```

On WSL with Windows `python` available, also set `S31_TEST_WINDOWS_SERIAL=1`
to exercise real Windows subprocess timeout cleanup. That optional test uses
a deliberately silent helper and does not open a hardware COM port. Actual
serial and board acceptance remain separate HIL runs.
