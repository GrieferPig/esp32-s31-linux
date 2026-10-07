# Linux throughput parity with native ESP-IDF
Accepted image: dist/e168e8c0f293a4d4 on BasedSurface's real ESP32-S31 board.
All six matched Linux slots flashed and explicitly hash-verified. Persist
[0x1ee000,0x400000) excluded. Radio binary and BTstack Os binary hashes are
identical to the preceding HZ1000 image3487ed32a9da7963.
Only the Linux tick configuration changes1000 to100; high-resolution timers,
radio RTOS tick conversion and explicit40ms BT batching remain.
Candidate diff scratch-applied exactly; generated kernel config retained.

## Measured result
All rates below are end-to-end host-fd reception, not pump/controller counts.
Same BasedSurface Marvell peer; BLE1M, MTU517,495-byte notifications at15ms;
Classic authenticated channel1,990-byte checked frames. Each timed run40s.
Fresh host agent, scan cache and pairing before connect. No host build or
packet logging during acceptance runs.

|Protocol/run|Host bytes|Seconds|Host KiB/s|p99 host interarrival gap ms|
|---|---:|---:|---:|---:|
|Classic SPP rr20-0|8738730|40.000098|213.347380|15.053|
|Classic SPP rr20-1|8690220|40.000126|212.162908|15.060|
|BLE notify rr20-0|1297395|40.000176|31.674543|29.994|
|BLE notify rr20-1|1295415|40.000066|31.626291|30.045|
Every final stream has zero sequence gaps, duplicates, bad patterns,
partial SPP frames, and HCI RX/TX drop/rejection counter increments.
Raw host bytes match their recorded totals. Arrival gaps are receive timing,
not one-way packet latency or RTT; full timing arrays are retained.

Original native references: BLE31.529511/31.795451KiB/s,
Classic-only214.386362/213.710117KiB/s.
Native BTDM control immediately before this Linux image:
214.507148/210.011487KiB/s, also checked clean (../native-btdm/report.md).
Linux Classic mean212.755144 is99.40% of the original native Classic mean,
and within the current native control's range. BLE31.63–31.67 is within
the native31.53–31.80 range. This establishes sustained speed on par with
the measured ESP-IDF reference; it is not a claim of exceeding every run
or of an absolute theoretical maximum. BLE and Classic are separate runs.

## Reproduction and scope
Build command and flash/verify argv are retained beside this report.
python3 -u bt-latency/priority_matrix.py bt-latency/logs/kernel-hz100/runs rr20 rr20
python3 -u bt-latency/ble_final_matrix.py bt-latency/logs/kernel-hz100/ble rr20 rr20
python3 -u bt-latency/wifi_final_check.py bt-latency/logs/kernel-hz100/wifi

Common module setup: mode=bt direct_hci=1 bt_tx_buffers=10
bt_hci_rx_slots=32 bt_hci_tx_slots=16 bt_worker_cpu=-1,
bt_gate_sleep=1 and bt_tick_ms=40.
BTstack pinnedCPU1 (mask2), main thread SCHED_RR20, Os variant,
S31_BTSTACK_LOCAL_CALLBACKS=1, COALESCE_WAKEUPS=0, COEX=0,
BURST=24, SPP_BYTES=990, BLE_BYTES=495, INTERVAL=12, logging none.
Classic: ADVERTISE=0, PAIRABLE=1.
BLE: ADVERTISE=1, PAIRABLE=0; no PHY override (peer supports1M only).
These are explicit benchmark settings, not simultaneous BLE+SPP rates.

Controlled preceding HZ1000 RR20 results were198.46/193.99/194.08KiB/s;
HZ100 closes the remaining measured gap. CPU busy snapshots fall to
67.23–73.69% aggregate for Classic,59.74–60.56% forBLE. Snapshot windows
include setup around the40s receive window, and IRQ_TIME_ACCOUNTING is off;
do not interpret these as isolated instruction/function costs.
Non-SRAM memory and radio heap deltas are in acceptance.json and raw
snapshots; all four runs show zero heap_used delta. BLE first run raises
heap_peak by4224B during connection warmup; second has no peak increase.
This is a measured configuration effect, not proof of one underlying
scheduler/IRQ bottleneck. Broader interactive scheduling latency was not
measured; HZ100 is the explicit bulk-throughput configuration.

## Wi-Fi and final state
Volatile combo-mode Wi-Fi scan found55BSS, wifi_init=0, zero Wi-Fi RX/TX
and HCI drop counters. wlan0 and /dev/s31-hci coexist. Bluetooth enable
was pending until frontend open in this Wi-Fi-only check; no simultaneous
combo Bluetooth throughput claim. wlan0 was brought down afterward.
AP association/IP throughput remain outside this fixture (no authorized AP).
Saved config/persist unchanged; all test configuration used /tmp/cfg.
Final restoration script resets to the verified BLE-ready BT-only setup,
confirms advertising command completion, affinity, policy and health.
The final restoration result is retained under restored/ and restore.txt.
The speculative HCI-alignment candidate was not applied.
