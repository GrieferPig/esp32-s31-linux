# Timer cadence A/B: rejected as the Classic throughput fix
Linux dist/c988e2833b35f1d3, complete six-slot flash and six explicit digest
verifications. Persist excluded. Kernel experiment commit a823555fed8c89b39b56dd5b67a1140e53160bc6.
bt_tick_ms defaults40 and is clamped1..40. Only the BT-only branch changes;
the Wi-Fi/combo10ms branch is unchanged. No Wi-Fi association acceptance claimed.

Same running BTstack,990-byte SPP frames, PAIRABLE1,COEX0, no packet logging.
Each run resets the host adapter, registers agent, warms scan cache, pairs,
and receives for40seconds. Command:
python3 -u bt-latency/tick_matrix.py spp bt-latency/logs/tick-cadence/runs 40 10 1 40

| Timer ms | Host KiB/s | Aggregate CPU busy around RX | Data/HCI errors |
|---|---:|---:|---|
|40|107.289754|97.84%|0|
|10|101.585623|95.39%|0|
|1|108.280732|96.70%|0|
|40 repeat|105.453007|88.90%|0|

Shorter timer batches do not close the gap to native213.71-214.39KiB/s.
The final repeat's CPU window includes a4.55s connection phase; CPU snapshots
surround the receiver rather than defining its exact40s window. No target met.
Keep40ms. This parameter is experimental, not a claimed performance fix.

A separate40ms run with per-task /proc/PID/stat snapshots achieved103.857745KiB/s,
also with no content/HCI drops. Delta CPU USER_HZ ticks:
BTstack3038 (1208user+1830system), radio worker2560system, btdm2333system.
These are observed CPU owners, not yet a causal diagnosis.
The next isolated test examines redundant BTstack callback wakeup writes.
Commands, raw bytes, timestamps, memory/CPU/radio/process snapshots retained.
