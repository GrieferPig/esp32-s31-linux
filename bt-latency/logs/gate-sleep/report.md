# BT-only sleeping gate experiment
The mutex remains the ownership domain used by ISR guards. Opt-in
bt_gate_sleep=1 changes contended acquisition to wait_event with mutex_trylock.
Every release wakes that queue; legacy mutex waiters keep their normal wakeup.
The feature check excludes Wi-Fi/combo; default remains0. No per-connection
BTstack task scheduling or native IRQ ownership checks changed.

Scratch patch -p1 application matched all three kernel files exactly.
Build: PATH="$HOME/.local/bin:$PATH"
ESP_TOOLS="$HOME/.espressif/tools/riscv32-esp-elf/esp-16.1.0_20260609/riscv32-esp-elf"
S31_ALLOW_UNPINNED=1 make build image.
Distc252d979f699ebb8 full six-slot write and six digest checks passed.
Persist excluded. Boot/login, volatile BT bringup, and exact commands retained.

Same running BTstack, affinityCPU1, timer40ms, legacy callbacks, SPP990,
COEX0, PAIRABLE1, no packet log. Standard kernel sampling enabled.
python3 -u bt-latency/gate_matrix.py bt-latency/logs/gate-sleep/runs

|Run|Host bytes|Seconds|KiB/s|Unambiguous mutex-spin samples|
|---|---:|---:|---:|---:|
|gate0-0|5006430|40.000180|122.226744|662|
|gate0-2|4988610|40.000349|121.791174|632|
|gate1-1|5143050|40.000087|125.562470|0|
|gate1-3|5187600|40.000246|126.649610|0|
All four40-second transfers: zero gaps, duplicate frames, bad patterns,
and HCI drops. Every profile was gzip/base64 pulled in a<=300KiB chunk and
matched its board SHA256. Sampling has the limitations described in the
cpu-profile report; modules/user/native text are excluded.
Sleeping contention removed sampled gate spinning and improved throughput
from121.79–122.23 to125.56–126.65KiB/s. This modest improvement does not meet
the native~214KiB/s target. Keep the default off; subsequent BT-only tests
may explicitly select1. Wi-Fi association was not tested.

After radio runs, the local syscall probe was uploaded only to /tmp,
hash-checked, pinnedCPU1, run, and removed. It is diagnostic, not radio
throughput evidence.2000iterations each:
getpid10.561us; monotonic clock44.954us; coarse clock32.948us;
one-byte pipe write+read155.075us;990-byte pattern fill23.928us.
These isolated idle costs do not prove costs under streaming load.
