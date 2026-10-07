# Compatibility-layer CPU diagnosis method

The previous profiler retained only built-in kernel addresses; radio module
and payload execution were invisible. This experiment adds an all-address
timer-PC histogram to kernel/profile.c. Diagnostic image 1bf66f2f941d592f;
accepted image e168e8c0f293a4d4 is restored afterward.

The profiler records exact saved instruction pointer, PID, user/kernel mode,
CPU, and count. Per-CPU open-addressed tables use bounded 32-slot probes and
report lost samples. Tables are allocated dynamically to keep the XIP image
within its slot. Root-only /proc/s31_pc_profile accepts 1 to reset/enable,
0 to freeze. An on_each_cpu barrier waits for existing tick writers before
reset or read. Reads while enabled fail with EBUSY. No UART output in the
timed window.

Each protocol uses off/on/on/off runs, 40 seconds of checked host-received
payload. At approximately +5 seconds, a 30-second CPU-idle probe runs with
the diagnostic sampler enabled or disabled. Counters are frozen afterward
and pulled as gzip/base64, at most 300 KiB per pull, with SHA256 matching.
Disabled runs briefly reset/enable then disable before the probe; any tiny
setup sample is ignored. No profile read runs concurrently with sampling.

CPU percentage uses wall minus high-resolution NO_HZ idle/iowait residency,
normalized to both cores. This is independent of the sample histogram.
Host throughput covers the full 40 seconds. Payload gaps/corruption and
radio HCI drops are checked.

Limitations: timer samples are not cycle-exact CPU accounting. IRQ masking
can delay samples and concentrate them at IRQ-enable instructions; NO_HZ
idle suppresses ticks. No time spent sleeping is directly sampled, but
deferred IRQs still limit attribution. Unknown addresses are kept explicit.
Generic kernel samples do not have call chains, so scheduler/locking work
cannot be assigned exclusively to the compatibility layer.

Symbols are frozen from the matching build, with ELF hashes. Payload ELF
and kernel ELF use absolute function address/size ranges. CONFIG_KALLSYMS
is disabled, so module symbols use /proc/modules load address and exact ELF
section order/alignment per kernel/module/main.c; CONFIG_MODULE_UNLOAD=y
retains .exit.text ahead of .text. Dynamic PLT addresses remain unresolved.

Configuration matches the accepted tests: HZ100, LE1M, controller10,
RX32/TX16, sleeping gate, timer40ms, BTstack Os on CPU1/RR20, local callbacks,
no packet logging, BURST24, BLE495/SPP990. Only diagnostic sampling code is
added. Persist is never written. Full six-slot flash and six explicit digest
verifications are recorded for deployment and restoration.

Deferred ticks after task switches can also be attributed to the new PID.
Separate vzalloc buffers avoid the kernel per-CPU allocator size limit.
Kernel/module hashes and payload-body byte match: symbols/verification.json.
