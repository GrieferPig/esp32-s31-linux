# Radio worker CPU placement: retain default unpinned
Dist575b3fa76991cb3c; six-slot flash and six explicit digest matches.
Persist excluded. Build uses established PATH/ESP_TOOLS/S31_ALLOW_UNPINNED
make build image environment. Three-file kernel patch scratch-applied exactly.

Diagnostic bt_worker_cpu (-1 default,0,1) creates the radio-owned ordered
high-priority queue with optional workqueue attributes before enqueueing work.
The built-in wrapper preserves ordered=true and MIN_NICE for pinned queues.
Default-1 uses the original allocator; Wi-Fi/combo always use-1.
No taskset or renice is applied to shared worker threads.

All completed transfers: controller10, HCI RX32/TX16, local callbacks1,
coalescing0, gate_sleep1, timer40, SPP990, COEX0, PAIRABLE1, no pklg,
profiler enabled; BTstack pinnedCPU1 after initialization.

|Run|Host bytes|Seconds|Host KiB/s|RX rejection delta|
|---|---:|---:|---:|---:|
|runs/worker-1-0|7203240|40.000189|175.859519|0|
|worker1-recovered|3501630|40.000178|85.488632|0|
|restored-start/worker-1-0|7282440|40.000144|177.793306|0|
All completed transfers have zero gaps/duplicates/payload errors/partial frames
and zero HCI RX/TX drop increments. Worker masks were read from/proc/status:
default0-1 and pinned1. CPU1 falls to85.49 vs175.86/177.79KiB/s default.
Do not adopt worker pinning.

Setup failures, with exact commands/raw evidence:
- runs/worker0-1 and cpu1-control/worker1-0 lost console response after software
  reboot/login, before radio configuration. No throughput result.
- retry/worker0-0 initialized CPU0 then lost response during BTstack startup.
- pinned-start/worker0-0 kept the console responsive with BTstack pinnedCPU1
  from launch, but HCI init did not complete after a restart.
- hard-reset/worker1-0 likewise stalled init with early BTstack pinning;
  restart-unpinned-command.txt brought it up, then it was pinnedCPU1 and
  measured as worker1-recovered.
- restored-start/worker0-1 used a read-only ROM hardware reset and restored
  unpinned BTstack initialization; startup again timed out without a marker.
  CPU0 has no accepted throughput measurement.

HUPCL was independently disabled in capture helpers (commit5887ffa); failures
also occurred with-hupcl, so it does not explain all startup failures.
The final harness uses hardware read-mac resets (no flash writes), unpinned
BTstack initialization, and up to3 bounded attempts before failure.
It records/validates the active worker CPU mask after each completed stream.
We route around these startup problems; no scheduler semantics were altered.

Reproducible commands:
python3 -u bt-latency/worker_affinity_matrix.py bt-latency/logs/worker-affinity/runs -1 0 1 -1 0
python3 -u bt-latency/worker_affinity_matrix.py bt-latency/logs/worker-affinity/retry 0 1 -1 0
python3 -u bt-latency/worker_affinity_matrix.py bt-latency/logs/worker-affinity/pinned-start 0 1 -1 0
python3 -u bt-latency/worker_affinity_matrix.py bt-latency/logs/worker-affinity/cpu1-control 1 -1 1
python3 -u bt-latency/worker_affinity_matrix.py bt-latency/logs/worker-affinity/hard-reset 1 -1 1
python3 -u bt-latency/worker_affinity_matrix.py bt-latency/logs/worker-affinity/restored-start -1 0
Historical setup variants and manual recovery are recorded in each run's
command files; final harness follows the final hardware-reset sequence.

Classic still needs~214KiB/s. Next control compares BTstack-Os and-O2 binaries
on one matched image, with default unpinned worker and the clean deep queues.
