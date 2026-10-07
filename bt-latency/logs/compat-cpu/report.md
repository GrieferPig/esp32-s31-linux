# Compatibility-layer CPU check

**The compatibility layer is not established as the main cause.** Direct
shim, controller and generic kernel execution are all visible. The largest
apparent kernel hotspots are biased by delayed timer interrupts, preventing
a trustworthy exclusive CPU split.

The old kernel-only profile did not establish that the blob was cheap.
This full-address profiler records substantial Classic ACL router/programming
and BLE controller execution. Location of execution also does not by itself
explain why Linux consumes more CPU than native ESP-IDF.

## Eight real host-received runs

Diagnostic dist 1bf66f2f941d592f; unchanged accepted settings and LE1M.
Each protocol used off/on/on/off, 40-second checked transfers with a
30-second CPU window. All eight have zero payload/sequence errors and HCI
drops. Four enabled profiles have zero lost samples and SHA256-verified pulls.
CPU is normalized to both cores; 50% means one fully busy core.

| Protocol | Off CPU | On CPU | Off host KiB/s | On host KiB/s |
|---|---:|---:|---:|---:|
| BLE | 67.554% | 67.348% | 31.916 | 31.729 |
| SPP | 79.762% | 80.196% | 213.347 | 213.843 |

Throughput is retained and observed profiler overhead is modest relative
to repeat variation. Two repeats per condition are not a tight statistical
overhead bound. No performance optimization is retained.

## Proven attribution limitation

Disassembly shows finish_task_switch enabling interrupts at 0xc081b736;
the next instruction, 0xc081b73a, receives most of its samples.
_raw_spin_unlock_irqrestore restores interrupts at 0xc0824ff6; its next
instruction, 0xc0824ffa, similarly dominates. These are a conditional branch
and return-address load, not independently expensive algorithms. Pending
timer ticks land immediately after the masked interval ends.

| Protocol/run | All recorded samples | These two IRQ-enable successors |
|---|---:|---:|
| ble 1 | 3401 | 1197 |
| ble 2 | 3835 | 1337 |
| spp 1 | 4414 | 1621 |
| spp 2 | 4422 | 1611 |

This is a lower bound on boundary bias, not a correction factor. Other
bridge critical-exit/sync-unlock sites also restore IRQs. A deferred tick
after a context switch can be recorded against the new task, and NO_HZ
suppresses idle ticks. Do not convert these counts to CPU-time shares.

Raw samples whose recorded current task was btdm or the radio worker:

| Protocol/run | Direct RTOS shim | Other payload | Other module | Generic kernel | Unknown |
|---|---:|---:|---:|---:|---:|
| ble 1 | 191 | 920 | 191 | 1372 | 20 |
| ble 2 | 227 | 852 | 222 | 1338 | 20 |
| spp 1 | 148 | 1024 | 102 | 1157 | 10 |
| spp 2 | 179 | 1040 | 111 | 1169 | 14 |

Shim membership uses exact object symbols for RTOS core/queue/event,
Linux locks/timers and kernel radio-rtos. Other payload includes controller
and other glue, not exclusively closed blob. Generic kernel lacks call
chains and cannot be assigned wholly to the shim.

## Reproducibility and precise remaining blocker

See method.md, summary.json, profiler.patch, frozen symbols/disassembly,
build manifest, flash commands, individual commands and raw captures.
The patch passed a scratch patch -p1 apply and six flash-safety tests.
Kernel/module hashes match the flashed manifest; objcopy of the payload
ELF exactly matches the body of flashed radio.bin.

The first allocation attempt exceeded the per-CPU limit and is excluded
under allocation-failed. Missing kallsyms required module-address recovery
from /proc/modules plus exact ELF section layout. One SPP connection
aborted before data; failed-connect-1 is excluded and the same condition
was retried. No persist or no-AP repair was attempted.

NEXT: scheduler-aware on-CPU accounting at nested bridge/IRQ boundaries,
excluding off-CPU waits and subtracting controller callbacks, with an on/off
overhead check. Queue wait wall time and timer samples at unlock sites
cannot resolve the remaining causal question.

Production source is restored exactly to HEAD. Full six-slot restoration
of accepted dist e168e8c0f293a4d4 and post-restore health are in cleanup.json.
Persist is never flashed. Wi-Fi validation is a scan, not AP association.

Restoration verified: six digest matches, Wi-Fi scan 57 BSS, BLE advertising
and CPU1/RR20 confirmed. Kernel submodule clean; diagnostic not retained.
Raw captures and patches may be stored as .gz; archive-manifest.json records
original SHA256 and verified lossless gzip round trips.
