# BTstack scheduling priority control
Linux dist3487ed32a9da7963, controller10, RX32/TX16, worker unpinned,
BTstackCPU1, local callbacks1, coalescing0, gate_sleep1, timer40,
SPP990, advertising0, pairable1, coex0, no pklg, profiler enabled.
Only BTstack main-thread policy/nice changes after initialization.
Normal=SCHED_OTHER/nice0, nice10=SCHED_OTHER/nice-10,
rr20=SCHED_RR20, rr90=SCHED_RR90. Actual policy/priority retained.

Commands:
python3 -u bt-latency/priority_matrix.py bt-latency/logs/btstack-priority/runs normal nice10 rr20 normal
python3 -u bt-latency/priority_matrix.py bt-latency/logs/btstack-priority/clean-runs normal rr20 normal rr20
python3 -u bt-latency/logs/btstack-priority/recover.py
python3 -u bt-latency/logs/btstack-priority/high-priority.py

The native BTDM build overlapped the first matrix's rr20 and final normal case;
those remain exploratory. Clean repeat had a host pairing failure before
payload in rr20-1 (ConnectionAttemptFailed/Host is down); no rate is claimed.
BTstack still initialized. Recovery retried fresh pairing on the same running
controller, then switched policy in place, with no host build running.

|Run|Host bytes|Seconds|Host KiB/s|Boundary|
|---|---:|---:|---:|---|
|runs/nice10-1|7596270|40.000214|185.454820|isolated|
|runs/normal-0|7709130|40.000154|188.210456|isolated|
|runs/normal-3|7522020|40.000102|183.642598|host build overlapped; exploratory|
|runs/rr20-2|7824960|40.000099|191.038591|host build overlapped; exploratory|
|clean-runs/normal-0|7840800|40.000183|191.424906|isolated|
|recovery/normal-1|7765560|40.000174|189.588043|isolated|
|recovery/rr20-0|8128890|40.000141|198.458528|isolated|
|recovery/rr20-2|7945740|40.000223|193.986712|isolated|
|high-priority/rr20-1|7949700|40.000342|194.082811|isolated|
|high-priority/rr90-0|7928910|40.000204|193.575915|isolated|
All completed payload runs have zero gaps, duplicates, bad patterns, partial
frames, and HCI RX/TX drop/rejection counter increments.
Isolated RR20 yields198.46/193.99/194.08 versus normal191.42/189.59 on
the clean boot. Nice-10 did not help. RR90 gives193.58, no advantage over20.
Use explicit RR20 for subsequent isolated Classic tests; no default service
policy changed. Native~214KiB/s target remains unmet.
CPU/memory snapshots retained; their timing includes setup around the exact40s
host-fd receive window. Lower worker passes/CPU accompany RR20, but no claim
that a specific scheduler or IRQ path is the remaining root cause.
