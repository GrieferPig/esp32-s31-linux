# BLE advertising airtime control during Classic
Dist 3487ed32a9da7963; full matched six-slot flash and six explicit digest matches.
Persist excluded. Patch0037 scratch-applied and candidate matched built source.
Build command: PATH="$HOME/.local/bin:$PATH" ESP_TOOLS="$HOME/.espressif/tools/riscv32-esp-elf/esp-16.1.0_20260609/riscv32-esp-elf" S31_ALLOW_UNPINNED=1 make build image

S31_BTSTACK_ADVERTISE defaults on. Setting0 explicitly disables advertising,
with command credit/transport readiness guarding the direct command.
Each accepted run requires opcode0x200a COMMAND_COMPLETE status0 for its setting.
This isolates advertising airtime; the controller remains BTDM, unlike native
Classic-only. No claim that all native/controller mode differences are removed.

python3 -u bt-latency/advertising_matrix.py bt-latency/logs/advertising-control/runs 1 0 1 0

One hardware boot/controller setup, controller10, RX32/TX16, worker unpinned,
BTstackCPU1, local callbacks1, coalescing0, gate_sleep1, timer40, SPP990,
COEX0, PAIRABLE1, no pklg, profiler enabled. Fresh host agent/pair each40s.
Exact setup/run commands and raw host payloads retained.

|Run|Host bytes|Seconds|Host KiB/s|
|---|---:|---:|---:|
|adv0-1|7757640|40.000169|189.394707|
|adv0-3|7773480|40.000205|189.781256|
|adv1-0|7209180|40.000178|176.004586|
|adv1-2|7180470|40.000105|175.303984|
All four streams have zero sequence gaps, duplicates, bad patterns, partial
frames, and HCI RX/TX drop/rejection counter increments.
Advertising off yields189.39/189.78 versus176.00/175.30 on (about8% faster).
Use advertising off for further isolated Classic tests; enable it for BLE.
Classic remains below native~214KiB/s. No final parity claim.
CPU/memory snapshots span a wider window; rates use only40s host-fd reception.
