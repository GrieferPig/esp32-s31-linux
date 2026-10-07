# Scheduler and BTstack syscall path costs

Scheduling and IRQ handling are stronger next targets than direct gate/wait work or BTstack syscall bodies. The payload itself also consumes substantial CPU. These measurements locate execution; they do not yet prove which change will reduce it, or assign the whole Linux versus ESP-IDF gap to one layer.

## Real board timing

Diagnostic dist 559a02aabaf6fdca, same accepted settings, basedsurface peer, LE1M. Eight checked 40-second board-to-host transfers: off/on/on/off for each protocol. Four enabled profiles passed exact per-CPU nanosecond conservation, zero clock/state errors and SHA256-verified capture. All eight transfers have zero sequence/pattern errors and HCI drops.

Percentages below use both cores' elapsed capacity: 50% equals one full core. Nested categories are exclusive; sleeping waits are excluded. Payload includes the controller and open payload shim/timer work, rather than only the closed blob.

| Annotated path | BLE capacity | SPP capacity |
|---|---:|---:|
| Controller / payload | 21.73% | 18.95% |
| Scheduler | 10.24% | 14.25% |
| Hard IRQ | 12.80% | 13.65% |
| Radio integration | 5.97% | 6.22% |
| Gate + sync + radio enqueue | 8.43% | 7.87% |
| BTstack syscall bodies | 2.74% | 7.63% |
| BTstack outside annotated scopes | 3.23% | 9.29% |
| Soft IRQ | 1.24% | 1.25% |

Within BTstack syscall bodies, select/poll is the largest category (BLE 1.57%, SPP 3.51% of capacity). Read, write and time calls together are smaller. Syscall architecture entry/exit is outside these body scopes; BTstack outside therefore includes userspace and unannotated trap/SBI boundaries, and cannot be called pure userspace time.

The direct compatibility bridge is not free: gate/sync/enqueue costs about 8% of total capacity, plus about 6% in integration. It is not established as the sole cause. Scheduler and hard IRQ categories together occupy 23.04% for BLE and 27.90% for SPP. IRQ actor names denote the interrupted task, not the IRQ source.

## Overhead controls and performance

Two repetitions per condition; values are means. CPU uses wall minus NO_HZ idle/iowait residency, distinct from the annotated path partition. Throughput is host-received, not pump-to-controller.

| Protocol / image | CPU capacity busy | Host KiB/s |
|---|---:|---:|
| BLE / accepted | 68.021% | 31.826 |
| BLE / diagnostic off | 69.007% | 31.868 |
| BLE / diagnostic on | 69.904% | 31.832 |
| SPP / accepted | 80.231% | 213.528 |
| SPP / diagnostic off | 80.225% | 211.232 |
| SPP / diagnostic on | 81.937% | 210.761 |

Enabling timing adds 0.90 CPU percentage points in BLE and 1.71 in SPP versus the same diagnostic disabled. Corresponding throughput changes are -0.11% and -0.22%. The diagnostic-disabled versus restored accepted CPU means differ by +0.99 points in BLE and -0.006 points in SPP. Those checks expose the always-present hooks plus run variation. Compare raw repetitions rather than treating these small samples as a tight overhead bound.

Task-state idle includes idle-loop execution and unannotated trap/firmware boundaries, whereas NO_HZ measures idle residency. Thus the annotated non-idle means (BLE 66.61%, SPP 79.27%) differ from the CPU probe means (69.90%, 81.94%). Do not rescale the categories to erase that difference. Instrumentation and boundary overhead remain included.

## What changed, and precise next work

The diagnostic adds nested timing scopes, task state that survives sleep/migration, scheduler-switch accounting, IRQ/softIRQ overrides, and a root-only frozen /proc/s31_cost readout. It never uses timer-PC samples to apportion time. Tests exercise sleep exclusion, nested attribution, freezing, invalid clocks/states and conservation; real runs validate both CPU windows and instrumentation overhead. Ten patch-applied scratch files match the frozen diagnostic snapshots byte for byte. Six flash-safety tests passed.

Event rates averaged over enabled windows:

| Event / second across both CPUs | BLE | SPP |
|---|---:|---:|
| context_switch | 809.3 | 962.9 |
| hardirq | 1016.4 | 807.7 |
| worker_call | 208.2 | 182.8 |
| sync_wait | 155.6 | 156.9 |
| sync_wake | 761.5 | 1566.7 |
| deferred_isr | 216.7 | 163.2 |
| direct_isr | 315.5 | 84.7 |

NEXT: split scheduler costs into task selection, switch_mm, the custom coprocessor SBI switch and finish_task_switch; split IRQ costs by interrupt source and entry/dispatch/exit. Also time esp32s31_ext_enter_kernel/exit_user separately from BTstack userspace. The source calls custom-state SBI during applicable context switches and restores extension state at the user-return boundary; these are concrete candidates, not separately measured culprits. ASIDs are deliberately disabled because S31 TLBs do not reliably distinguish user translations, so that setting is not an established safe optimization.

## Evidence and restoration

Exact commands, every run's raw host payload and UART records, frozen timings, parsers, summary.json, build manifest/hashes, source snapshots and diagnostic.patch are retained here. See method.md for scopes and reproduction. archive-manifest.json records lossless gzip SHA256 round trips; large raw files and source snapshots may be stored as .gz.

No production optimization is retained. Original kernel sources are restored exactly to HEAD; submodule clean. Full accepted six-slot set e168e8c0f293a4d4 is restored with six explicit digest matches, and dist/current points to it. Diagnostic build artifacts remain available for reproduction; out/ still contains diagnostic build outputs and needs a rebuild before source-matched use. Persist was never written.

Post-restore Wi-Fi scan found 56 BSS; AP association is untested. BLE advertising, CPU1/RR20 and controller health are confirmed. No emulator was used.
