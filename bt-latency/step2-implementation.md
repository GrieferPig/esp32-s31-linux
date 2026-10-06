# STEP 2 implementation — dedicated `s31-radio` kthread removed (landed)

Submodule commits:
- `linux-esp32-s31@632d31313a7ef` ("s31-radio: replace dedicated s31-radio
  kthread with ordered workqueue"), one file:
  `drivers/platform/esp32s31-radio-smode.c` (+258/-202).
- `linux-esp32-s31@3d0953ecdc1df` ("s31-radio: create radio-init task on
  first pass; guard current-task for non-kthreads"): module init (insmod
  process context) only queues the first pass and returns; the radio-init
  task is created by that pass on a pool kworker, exactly like the retired
  thread did. `s31_linux_current_task()` (rtos.c) now returns NULL for
  non-kthreads instead of calling `kthread_data()` on a user process
  (warns + garbage deref). Terminal first-pass failure parks the runner
  (`init_dead`), matching the old thread exiting. Both changes were
  required after the first flashed image hung in module init: the new
  init path was the first to call payload task APIs from process context,
  a case upstream never exercised.

## What changed
- Deleted: `s31_radio_runtime_thread` ("s31-radio" kthread),
  `s31_radio_waitq`, `s31_tick_pending`, fixed 10/40 ms sleep branches.
- Added: `s31_radio_wq = alloc_ordered_workqueue("s31-radio", WQ_HIGHPRI)`
  (shared pool, ordered = one pass at a time: same serialization, no
  driver-owned thread), `s31_radio_work` delayed_work running
  `s31_radio_workfn` (same per-iteration body: IRQ sync, state update,
  tick accounting, `s31_radio_run_linux_pass`, cond_resched), one-shot
  `s31_radio_hrtimer` for deadline requeue, `s31_radio_runner` identity
  pointer (carries the old `current == worker` tests), dedicated
  `s31_radio_bt_disable_waitq` (woken in `s31_radio_report_bt_disable`).
- Every event wake (hard IRQ incl. BT-batching condition, commands, HCI
  RX/TX, Wi-Fi TX incl. TCP-ACK reschedule check) queues a pass directly;
  idle requeues arm the hrtimer at the esp_timer deadline (combo+BT,
  50..10000 us, as before) or the next RTOS tick. Tick cadence logic
  (10 ms, 40 ms BT-only batches) and `s31_rtos_tick` call sites unchanged.
- Preserved: blob-gate protocol, direct-ISR/deferral paths, command
  kthread-creation context, RX-outside-gate delivery, RT FIFO boost,
  health counters (`worker_passes` still incremented per pass),
  shutdown semantics (timer cancel + cancel_delayed_work_sync + destroy,
  NULL-queue guards). WQ_HIGHPRI replaces nice(-10); shared pool workers
  are never reniced. `init_mm` borrowed per pass (pool workers vary);
  mask-ROM check moved to first pass (needs the low mapping).
- Payload side (`s31_rtos_core.c`, `radio_stack.c`, `s31_linux_timer.c`):
  NO functional change — tick dispatch and foreign-TCB identity preserved;
  the removed thread lived in the kernel driver, not the payload.

## Why this replacement (choice justification)
Tasklet cannot sleep for the blob mutex. Pure inline deadlocks when the
caller holds the gate and leaves idle timers/IRQs with no runner. Shared
ordered workqueue is the mission-listed workqueue option: deletes the
dedicated thread, keeps single-execution serialization, keeps all wake
latencies event-driven. See bt-latency/worker-analysis.md for the rejected
alternatives and bt-latency/worker-removal-plan.md for the site map.

## Verification so far
- `make linux` clean: no new warnings (only pre-existing
  -Wmissing-prototypes elsewhere); `esp32s31-radio.ko` links.
- `.ko` strings: new "runner blob stack" present; old "worker entered" /
  "scheduler worker stopped" gone. `out/images/{radio.bin,rootfs.sqfs}`
  rebuilt with the new module (provenance updated).
- Boot + BT + Wi-Fi re-measurement pending flash (next step).
