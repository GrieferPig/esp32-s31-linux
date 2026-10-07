# Native ESP-IDF reference measurements

All rates below are actual BasedSurface host-received application bytes, not API-queued bytes.
Same ESP32-S31 (base MAC 30:ED:A0:F3:D4:AC), same Marvell HCI4.2 peer F0:6E:0B:D8:CE:45.
Native Bluetooth MAC ends AD; Linux BTstack ends AE. Native BLE-only / Classic-only versus Linux BTDM is a configuration difference.
ESP-IDF upstream 08e0d30a74ad0bfd5a34933142b80f45619ee410, CPU320MHz, sleep disabled, MTU517 for BLE, no packet capture during timed runs.

| Run | Bytes received | Seconds | KiB/s | Valid |
|---|---:|---:|---:|---|
| ble-default | 1195992 | 40.000114 | 29.198940 | yes |
| ble-iv12-len244 | 1219024 | 40.000084 | 29.761266 | yes |
| ble-iv12-len495 | 1291455 | 40.000193 | 31.529511 | yes |
| ble-iv12-len495-repeat | 1302345 | 40.000102 | 31.795451 | yes |
| ble-iv36-len495 | 1223145 | 40.000230 | 29.861767 | yes |
| ble-iv6-len244 | 173972 | 40.000218 | 4.247340 | yes |
| ble-iv6-len495 | 180675 | 40.000117 | 4.410998 | yes |
| spp-fixed-len1012 | 6676164 | 40.000147 | 162.991685 | yes |
| spp-fixed-len1980 | 8777340 | 40.000105 | 214.289966 | yes |
| spp-fixed-len3960 | 8706060 | 40.000199 | 212.549235 | yes |
| spp-fixed-len990 | 8781300 | 40.000158 | 214.386362 | yes |
| spp-fixed-len990-repeat | 8753580 | 40.000062 | 213.710117 | yes |
| spp-len1012 | 6684106 | 40.000009 | 163.186144 | NO: dropped partial writes |

## Targets
Best measured native settings: BLE 15 ms interval / 495-byte values, 31.529511 and 31.795451 KiB/s; SPP 990-byte writes, 214.386362 and 213.710117 KiB/s.
Use approximately 31.8 KiB/s BLE and 214.4 KiB/s Classic as the new Linux targets. These are the best settings in this measured sweep, not proof of an absolute hardware maximum.
All valid runs lasted 40 seconds, with zero sequence gaps, duplicates, or bad complete payloads. The 3960-byte run ended with 1980 partial-frame bytes; an offline check matched their expected next sequence and pattern too. summary.json includes every 5-second window.
BLE 7.5ms was only 4.25-4.41 KiB/s, 15ms was 29.76-31.80, and 45ms was29.20-29.86 in this sweep.
Native DLE251 requests failed with status17 and peer-not-supported at connection and again at subscription. LE1M is retained. Linux2022 command-complete success must not be reported as negotiated DLE.

## Benchmark correction
The initial SPP run is INVALID despite its 163.19 KiB/s byte rate. At sequence430 the native API accepted only990 of1012 bytes; the original benchmark advanced to the next frame and lost22 bytes. The corrected application tracks completion length, retries the remainder, and permits one pending write while respecting congestion. The valid1012-byte run observed9 partial completions and zero content errors. Expected L2CAP recursion-guard logging was disabled via esp_log_level_set for that tag only.

## Reproduce
Build each native project after sourcing /home/grieferpig/esp-idf/export.sh: idf.py --preview set-target esp32s31 build; incremental idf.py --preview build.
Use /home/grieferpig/.espressif/python_env/idf6.2_py3.13_env/bin/python bt-latency/native-idf/flash_native.py ble --execute (or spp). Guarded complete four-image writes and four hash verifications are recorded in *flash*.txt.raw.gz. Linux persist[0x1ee000,0x400000) is excluded from every partition and erased sector.
Each run command.json records its exact host argv and environment; fresh pairing/agent/scan sequence is implemented in fresh_fd_run.py or fresh_spp_run.py. UART is raw115200 with no modem-line toggles; native_monitor.py reads without sending Linux shell commands.
Exact sdkconfig snapshots, per-flash image manifests, build logs, original receiver bytes, arrival timestamps, and UART logs are retained here. Source changes are in bt-latency/native-idf; the installed SDK was not edited.
Raw files are gzip-compressed losslessly. sources-sha256.json fingerprints the final native sources/config defaults; raw-sha256.json fingerprints uncompressed evidence.

## Linux restoration
Restored dist94fb9aabbbf452d9 using tools/device/flash.py --slot all. All six writes passed internal hashes, followed by six explicit verify-flash digest matches. Persist was excluded. Linux booted and accepted root console login. Kernel6.18.0 and the pre-existing loopback-only network state were observed; Wi-Fi association was not tested.
