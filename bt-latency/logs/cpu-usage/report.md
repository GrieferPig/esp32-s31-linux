# ESP-IDF versus Linux CPU usage

Same ESP32-S31 at 320 MHz, two cores, same BasedSurface Bluetooth peer.
Two 40-second real host-received transfers per protocol and implementation;
CPU measured in a 30-second steady window about 5 seconds into each stream.
**100% total CPU capacity means both cores busy; 50% means one full core.**

| Protocol | Implementation | Mean CPU capacity | Mean host throughput |
|---|---|---:|---:|
| BLE | ESP-IDF | 13.83% | 31.542 KiB/s |
| BLE | Linux | 68.82% | 31.807 KiB/s |
| SPP | ESP-IDF | 20.54% | 214.918 KiB/s |
| SPP | Linux | 79.41% | 212.501 KiB/s |

All eight runs have zero sequence gaps, duplicates, and corrupt payloads.
SPP also has zero partial frames. These are end-to-end host-byte rates, not
pump-to-controller counts. BLE used LE 1M, 495-byte values and 15 ms interval;
SPP used 990-byte checked frames.

## Raw results

| Protocol | Implementation | Run | Host KiB/s | Core 0 busy % | Core 1 busy % | Two-core capacity % |
|---|---|---|---:|---:|---:|---:|
| BLE | ESP-IDF | run0 | 31.493237 | 27.650749 | 0.000043 | 13.825396 |
| BLE | ESP-IDF | run1 | 31.589911 | 27.673267 | 0.000030 | 13.836648 |
| BLE | Linux | rr20-0 | 31.771267 | 84.636044 | 53.141601 | 68.905486 |
| BLE | Linux | rr20-1 | 31.843597 | 85.202980 | 52.276279 | 68.739630 |
| SPP | ESP-IDF | run0 | 215.256279 | 41.098483 | 0.000060 | 20.549272 |
| SPP | ESP-IDF | run1 | 214.579995 | 41.052115 | 0.000033 | 20.526074 |
| SPP | Linux | rr20-0 | 212.476661 | 82.040469 | 76.642614 | 79.358202 |
| SPP | Linux | rr20-1 | 212.525468 | 80.239823 | 78.673671 | 79.456747 |

## Interpretation and limits

ESP-IDF leaves core 1 almost entirely idle. Its SPP work is primarily BTU_TASK,
btdm, hciT and BTC_TASK on core 0; BLE is primarily btdm, the throughput server,
BTU_TASK and hciT. Linux has substantial non-idle residency on both cores.
The observed CPU-capacity gap is repeatable, but this experiment does not
attribute Linux's excess work to individual functions or establish its cause.

The native figures use FreeRTOS task runtime accounting. Interrupts executed
while the idle task is current can be charged to idle, so native busy estimates
can undercount ISR work. Linux uses wall time minus high-resolution NO_HZ idle
and iowait residency. These are OS accounting metrics, not exactly equivalent
hardware-cycle measurements or power estimates. Native uses protocol-specific
controller modes; Linux uses BTDM. This compares the complete implementations
and configurations, not the cost of the OS alone.

Instrumented native BLE is 31.493/31.590 KiB/s versus the earlier uninstrumented
31.530/31.795 KiB/s; instrumented SPP is 215.256/214.580 KiB/s versus
214.386/213.710 KiB/s. No material throughput penalty was observed in these
two-repeat comparisons; this is not a formal instrumentation-overhead bound.

## Measurement changes

- Add an optional first-host-packet marker to the existing BLE/SPP receivers.
  Default benchmark behavior is unchanged unless S31_STREAM_STARTED_FILE is set.
- Add a small temporary Linux probe that reads /proc/stat twice around a
  monotonic-clock 30-second sleep; UART output occurs after the sample.
- Add isolated ESP-IDF runtime-stat builds and a low-priority sampler with two
  snapshots. A core-1 IPC callback flushes the idle runtime at each boundary;
  reporting is delayed until after the 40-second transfer.
- Add runners, parsers and tests for two-core normalization and counter wrap.
  No Linux kernel, transport, radio configuration or SDK source was changed.

An exploratory Linux shell sampler is retained under linux-spp/ but excluded
from this comparison. Its tick-based denominator missed accounted time.
Accepted linux-*-idle results use measured wall time and NO_HZ idle residency.
Summed tick coverage is retained as a diagnostic (about 82-85%), not used as
the CPU denominator. Snapshot brackets were under 7 ms; their timing and
tick-quantization bounds are recorded in each cpu-summary.json.

## Reproduction and evidence

See method.md for accounting details and source-revisions.json for exact
parent, kernel and SDK commits. Native project source/config snapshots,
instrumentation.patch, successful scratch-patch logs, exact build commands,
flash commands/manifests, flashed image archives and raw UART captures are
retained. The original ESP-IDF checkout remained clean.

Linux runs use cpu_linux_matrix.py with S31_CPU_PROTOCOL=ble or spp and two rr20
cases. It uploads and hash-verifies cpu_idle_probe, configures the accepted
Linux runtime, and calls cpu_measure_run.py. Native runs use native-run.py
ble/spp and call the same measurement wrapper. Each run has command.json,
raw payload, host timing, pairing logs and CPU evidence.

The parser unit tests passed (parser-tests.txt); native summed-task coverage
is essentially 100% of two-core wall time in all four samples. All instrumentation
patches passed scratch patch -p1 application before the native builds.

Gzip files preserve original bytes; raw-sha256.json records hashes and lengths.
Build and scratch directories are excluded from version control. Excluded
exploratory observations remain identified above.

## Restored board state

Linux dist e168e8c0f293a4d4 was restored as a complete six-slot set, then all
six slot digests explicitly matched. BLE-ready BTstack has advertising enabled,
CPU1 affinity, RR20 scheduling, and zero HCI drops. Persist was not written.
No CPU sampler or host UART owner remains. The Linux image is unchanged from
the prior Wi-Fi scan validation; this task did not repeat Wi-Fi association.
See restore-linux/ and cleanup.json.
