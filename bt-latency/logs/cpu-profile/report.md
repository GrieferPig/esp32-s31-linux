# Standard Linux CPU sampling
Enabled CONFIG_PROFILING in the common kernel fragment. It is inactive until
explicit echo6 to /sys/kernel/profiling, with64-byte PC bins.
Built with PATH="$HOME/.local/bin:$PATH"
ESP_TOOLS="$HOME/.espressif/tools/riscv32-esp-elf/esp-16.1.0_20260609/riscv32-esp-elf"
S31_ALLOW_UNPINNED=1 make build image.
Dist0b467a4026ba9916: full six-slot flash, six explicit digest matches;
persist excluded. Six flash safety tests passed. Exact flash commands retained.
First console attempt preceded the login prompt; interrupted only that host
reader, then logged in normally and ran the saved bringup command.
Pre-existing persist mount failure remains outside scope.

BTstack legacy callbacks, timer40ms, SPP990, BURST24, COEX0, PAIRABLE1,
BLE495/interval12, no packet logging. First start stalled; restart succeeded.
python3 -u bt-latency/profile_spp_run.py bt-latency/logs/cpu-profile

|Mode|Host bytes|Seconds|KiB/s|Data errors / HCI drops|
|---|---:|---:|---:|---|
|Sampler off|4483710|40.000062|109.465406|0 /0|
|Sampler on|4483710|40.000346|109.464629|0 /0|

Profiler did not measurably affect this pair. CPU windows include connect
and snapshots: off88.03%, on93.59% aggregate busy; different connect overhead
precludes treating that difference as sampler cost.
BTstack accounted for31.3–31.7CPU seconds, radio worker25.2–26.0,
btdm23.0–23.1 across the snapshot windows.

Frozen profile272204bytes compressed1542bytes, pulled in one<=300KiB chunk;
host SHA256 matched board56d48e031eede793f33fa6379af1332c808df65d22079ab1739cfb2d885c17a9.
23682 samples in built-in kernel text. Excludes userspace, loadable modules,
and low-address native radio code.64-byte bins spanning symbols explicitly
list candidates; they must not be read as calls to the preceding symbol.
4794 unambiguous samples are in do_trap_ecall_u, including4238 in the bin
containing csrsi sstatus,2. Delayed timer delivery after IRQ-off work biases
this location; it is NOT proof that those few entry instructions cost20%.
1943 samples fall unambiguously in mutex_spin_on_owner; another511-bin spans
ww_mutex_trylock and mutex_spin_on_owner. CPU placement is the next control.
No throughput fix is claimed. Native~214KiB/s goal remains unmet.
