# CPU optimization A/B results: reverted

Sixteen real host-received runs retained clean payloads and broadly retained
throughput, but did not establish a reliable useful reduction in CPU usage.
All production source changes were reverted. The accepted image remains
e168e8c0f293a4d4. This is a negative optimization result, not a speedup.

## Decisive repeat

The first BLE comparison looked promising. Its closing baseline fell too,
so a new baseline/batch/batch/baseline block was run on the same image.

- ble-repeat/base: 68.038416% CPU, 31.922358 KiB/s, approximately 42.625000 CPU-ms/KiB; n=2.
- ble-repeat/tx4: 67.905134% CPU, 31.861875 KiB/s, approximately 42.624638 CPU-ms/KiB; n=2.

The small CPU reduction accompanies a similarly small throughput reduction.
CPU per KiB is effectively unchanged. This diagnostic combines a 30-second
CPU window with the full 40-second throughput mean, so do not interpret its
extra decimal places as measurement accuracy.

100% CPU means both cores fully busy. The individual baseline variation is
larger than the measured mean difference. No formal statistical speedup is
claimed. Exact wall-clock/NO_HZ-idle samples, host payloads and timing are
retained for every run.

## All hardware runs

| Group | Case | Total CPU % | Host KiB/s | Approx CPU-ms/KiB | Accepted batch frames / writes |
|---|---|---:|---:|---:|---:|
| ble-repeat | base-0 | 69.120651 | 32.097612 | 43.069030 | 0 / 0 |
| ble-repeat | base-3 | 66.956182 | 31.747104 | 42.180970 | 0 / 0 |
| ble-repeat | tx4-1 | 67.490639 | 31.674544 | 42.615066 | 2671 / 2479 |
| ble-repeat | tx4-2 | 68.319629 | 32.049205 | 42.634211 | 2701 / 2495 |
| ble-screen | all-4 | 67.687246 | 31.735016 | 42.657767 | 2675 / 2477 |
| ble-screen | base-0 | 68.474526 | 32.037075 | 42.747052 | 0 / 0 |
| ble-screen | base-5 | 66.905918 | 31.565810 | 42.391384 | 0 / 0 |
| ble-screen | coarse-3 | 68.039538 | 31.904141 | 42.652480 | 0 / 0 |
| ble-screen | tx4-1 | 66.759698 | 31.831677 | 41.945449 | 2683 / 2472 |
| ble-screen | tx4drain-2 | 66.757675 | 31.940414 | 41.801384 | 2692 / 2505 |
| spp-screen | base-0 | 80.453072 | 214.724385 | 7.493613 | 0 / 0 |
| spp-screen | base-5 | 80.202136 | 213.951556 | 7.497224 | 0 / 0 |
| spp-screen | coarse-3 | 79.887804 | 213.830154 | 7.472080 | 0 / 0 |
| spp-screen | policy-4 | 80.008048 | 214.410956 | 7.463056 | 0 / 0 |
| spp-screen | tx4-1 | 79.588268 | 212.549966 | 7.488900 | 8913 / 6957 |
| spp-screen | tx4drain-2 | 79.825892 | 213.660170 | 7.472230 | 8960 / 7086 |

All sixteen runs: zero sequence gaps, duplicates, malformed payloads, and HCI
RX/TX drops. SPP also has zero partial frames. BLE logs confirm interval12
(15 ms); the configured PHY is 1M, value495 bytes. SPP uses990-byte frames.
No packet logging or task scanning runs within the steady CPU window.

## Outcomes by change

- TX batching: valid copied-frame batching with a single worker wake per
  accepted batch. It reduced TX writes by about22% in SPP and7-8% in BLE,
  but the end-to-end CPU benefit did not survive the controlled repeat.
- Callback draining: bounded extra callback-only rounds gave no clear
  additional saving over batching.
- Coarse clock: retained throughput here but gave no useful reliable CPU
  improvement; combining it with batching did not improve the result.
  It also reduces timer precision, so it was not retained.
- Worker policy reset: the skip counter stayed zero. The worker was not
  already in the target policy at those reset sites; there was no redundant
  call to eliminate in these runs.

The large remaining CPU consumer is radio-task execution, which includes both
the controller blob and Linux compatibility code. A kthread's execution is
accounted as system CPU time without implying that it is servicing syscalls.
Only BTstack runs in userspace and explicitly enters the kernel for
read/write/select/time operations. The existing PC profiler excludes module
and payload code, so syscall prominence among visible samples does not mean
that blobs are cheap. Separating blob work from compatibility/gate/IRQ work
remains the next useful profiling step.

## Implementation, tests, and provenance

Candidate952277019477208a was built, flashed as a full six-slot set and explicitly
verified with six matching digests. Its radio.bin SHA256 is identical to the
accepted image. Persist was excluded. Runtime switches isolated each change;
baseline and candidate settings were compared on that same image.

Both candidate patches passed scratch patch -p1 application. C tests compile
the real batch parser/flush functions with bounded ring/write mocks under
ASan/UBSan and verify malformed/truncated input, eight-frame limits, EAGAIN,
EINTR, partial accepted prefixes, ordered suffix retry, completion and POLLOUT
accounting. All passed. Six flash-safety tests passed.

A pruned batch-only follow-up285e9667e55eb727 also built and passed scratch/unit
checks, including a driver-capability handshake. It was never flashed:
the decisive repeat on952277019477208a failed to justify deployment. Its
build/source evidence is under final/ and explicitly marked unflashed.

The candidate kernel patch, BTstack patch, exact sources, commands, build logs,
flash logs and test harnesses are retained. Archived cpu_opt_matrix.py and
cpu_batch_matrix.py resolve shared helpers from bt-latency; run them only
against their matching experimental image. Historical commands retain the
original harness locations. test_tx_batch.py now tests the frozen source in
final/ and can run after the production source reversion.

The unrelated BT_DEBUG_MISSION.md is preserved. No production kernel or
BTstack performance patch remains. Gzip archives preserve raw bytes, with
SHA256/length records. Build and scratch trees are not committed.

## Verified final state

Restored accepted dist e168e8c0f293a4d4 as all six slots and explicitly matched
all six flash digests. Combo-mode Wi-Fi scan found 48 BSS entries;
association was not tested because no authorized AP is available. Restored
BLE advertising, CPU1 affinity and RR20 scheduling with zero HCI drops.
No UART owner remains. Persist was not flashed. Kernel worktree is clean;
only this experiment's evidence is committed.
