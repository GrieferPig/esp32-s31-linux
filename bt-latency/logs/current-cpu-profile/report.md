# Current Linux CPU profile

The dominant accounted CPU consumers are the radio controller task (btdm)
and the s31-radio worker. Together they account for 81.5-81.9% of recorded
task CPU during BLE and 62.1-62.4% during SPP. BTstack is the second major
target for SPP, where most of its accounted time is system time.

This identifies the expensive subsystem, not yet a specific inner function
or a proven throughput limiter. It does not establish whether controller
algorithms, busy waits, stack/gate transitions, or IRQ dispatch dominate
inside the radio execution path.

## Current hardware captures

Dist e168e8c0f293a4d4, HZ100, same accepted optimized configuration as the
CPU comparison: CPU1/RR20 BTstack, local callbacks, no packet logging,
controller10, RX32/TX16, sleeping gate, timer40ms. Two 40-second checked
host-received runs per protocol. No firmware/configuration optimization
was made. Kernel PC sampling was already enabled in the prior CPU runs.

| Protocol/run | Host KiB/s | btdm CPU-s | Radio worker CPU-s | BTstack user CPU-s | BTstack system CPU-s | Radio share of task CPU |
|---|---:|---:|---:|---:|---:|---:|
| ble rr20-0 | 31.953 | 22.70 | 16.84 | 3.44 | 5.06 | 81.48% |
| ble rr20-1 | 31.662 | 22.73 | 16.75 | 2.87 | 5.34 | 81.86% |
| spp rr20-0 | 213.999 | 19.43 | 16.73 | 6.89 | 14.62 | 62.40% |
| spp rr20-1 | 212.114 | 19.02 | 17.56 | 9.04 | 13.00 | 62.09% |

All four runs have zero gaps, duplicates, corrupt payloads and HCI drops.
SPP has zero partial frames. Throughput is consistent with the prior
31.81 KiB/s BLE and 212.50 KiB/s SPP means, with no material slowdown observed
in these repeats. This is not a formal statistical profiler-overhead bound.

Task CPU deltas span the before/after snapshots around the transfer, including
connection and snapshot overhead; they are not exactly 40-second windows.
CPU-s means recorded ticks divided by USER_HZ=100. These task counters
undercount wall time on this kernel; their relative shares are attribution
evidence, not replacements for the prior wall-minus-NO_HZ-idle CPU readings.
Do not use board-summary.json's legacy aggregate busy field as true total CPU.

## What the visible kernel profile says

The strongest visible paths include do_trap_ecall_u, do_select/core_sys_select,
timekeeping and idle/IRQ transitions. SPP's first capture has 234 syscall-entry
samples and 106 unambiguous do_select/core_sys_select samples out of 1087
built-in-text samples. This supports further investigation of BTstack's
syscall/event-loop cost. It does not mean syscall-entry instructions consume
21.5% of the whole CPU: delayed timer interrupts can be attributed at IRQ
re-enable, and these samples cover only a subset of execution.

The kernel profile has 64-byte PC bins. Ambiguous bins explicitly retain all
candidate symbols. It excludes userspace, loadable-module text and the
low-address radio payload. The exact vmlinux hash matches the flashed dist
manifest (symbols-verified.json). Frozen profiles were pulled as bounded
gzip/base64 chunks and matched the board's SHA256.

The old TIMG1 native-PC sampler is inactive in this current source/image
(esp32s31-radio-smode.c:965). PERF_EVENTS, FTRACE and IRQ_TIME_ACCOUNTING are
disabled. Therefore the existing profiler cannot split the main radio cost
between controller code and its Linux compatibility layer.

## Source-guided next diagnostic

Instrument the radio worker's IRQ handlers, HCI TX drain, gate/stack transitions
and the btdm task's active versus queue-wait intervals, or add bounded PC
sampling that covers the module and radio payload. Measure on the same
40-second transfers and retain a throughput control. Treat queue_receive
as a wait boundary rather than assuming that it is itself a CPU-burning cause.

For SPP, also count/timestamp select, read/write and clock_gettime calls in
the BTstack run loop. The current loop performs time queries before select
and before timer dispatch. Current evidence supports investigating batching
and per-event syscall cost, but does not prove that changing them is safe
or beneficial. No speculative performance patch is included here.

## Idle-radio control

After SPP disconnected, hcitool con showed no connection. Same running
radio/BTstack settings, 30-second wall-minus-NO_HZ-idle sample:
**1.758% of total two-core capacity**.
The surrounding task sample recorded 0.57 CPU-s in the radio worker and
0.03 CPU-s in btdm. Thus the large radio-side cost appears when the link
is active; it is not a permanently busy unloaded loop. This single control
does not separate connected-idle overhead from payload-processing overhead.

The temporary probe was uploaded with a matching SHA256. Its exact commands,
raw stat snapshots, task deltas, profile and host connection listing are in
idle-spp/. The final runtime restoration resets the board, removing the
temporary probe and profile buffers in /tmp.

## Final state

BLE-ready operation restored with advertising enabled, CPU1/RR20 and zero HCI
drops. No UART owner remains. No reflash, persist write or performance-code
change was made. The only new harness is current_profile_matrix.py, which
reuses the accepted setup and adds task snapshots and hash-verified profile
retrieval. Original unrelated BT_DEBUG_MISSION.md is preserved.
