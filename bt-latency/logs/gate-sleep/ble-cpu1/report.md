# Linux BLE reaches measured native range
Linux distc252d979f699ebb8; BT-only gate_sleep1, BTstack pinnedCPU1,
PAIRABLE0, COEX0, BURST24,495-byte values, requested15ms interval, LE1M,
legacy callback scheduling, packet log off. Same host adapter and fresh
BlueZ agent/pair/AcquireNotify flow as native comparison.
Kernel profiler active, as in the preceding Classic controls.
Exact start command and per-run host commands/raw bytes retained.

|Run|Host bytes|Notifications|Seconds|KiB/s|
|---|---:|---:|---:|---:|
|first|1296405|2619|40.000212|31.650345|
|repeat|1297395|2621|40.000237|31.674495|
Both40-second runs have zero sequence gaps, duplicates, bad payloads,
and HCI RX/TX drop increments across the surrounding board snapshots.
First run5-second windows31.421–32.001KiB/s; all windows are in summary.json.
Native reference with the same495B/15ms/1M settings was31.529511 and31.795451.
These Linux results are inside that measured native range: BLE parity is
met for this tested configuration. They are host-fd rates, not pump rates.
No claim of negotiated DLE or2M; peer is1M and native DLE was rejected.
Classic remains~126KiB/s versus native~214KiB/s, so the overall goal is open.
