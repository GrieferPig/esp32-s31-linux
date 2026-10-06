# STEP 2 implementation plan — ordered-workqueue replacement for the
# dedicated `s31-radio` kthread (NOT APPLIED YET; drafted while `make build`
# runs — tree must not be patched mid-build)

## Choice and justification
Replacement: **ordered workqueue + event-driven requeue** (mission-allowed
"workqueue" option). Not tasklet (softirq cannot sleep for the blob mutex;
blob pass sleeps). Not pure inline (hard-IRQ callers cannot sleep; payload
callers already hold the gate — re-entry deadlocks; idle timers/IRQs would
have no runner when all compat tasks are blocked). Ordered (`alloc_ordered_
workqueue`) preserves the single-execution serialization the kthread loop
provided; shared pool deletes the *dedicated* thread. Requeue delay comes
from `s31_linux_timer_next_due_us()` (clamped 50 us..10 ms combo, else next
tick), collapsing the fixed 10/40 ms poll into deadline-driven wakes while
keeping timing equivalent. Immediate `queue_delayed_work(...,0)` (via
`mod_delayed_work` to collapse bursts) on IRQ/command/HCI-RX-TX/Wi-Fi-TX
preserves the current wake latency. Latency win, if any, comes from STEP 4
knob changes (40 ms BT batching, ACL coalescing, credit flush), not the
runner swap — re-measure both.

## Exact sites (linux-esp32-s31/drivers/platform/esp32s31-radio-smode.c)
- Worker decl: line 815 `static struct task_struct *s31_radio_worker;`
  → `static struct workqueue_struct *s31_radio_wq;` +
  `static struct delayed_work s31_radio_work;` (+ keep
  `s31_radio_worker_passes` counter, line 879, incremented per execution so
  `radio_health` keeps reporting).
- Sleep/wakeup: line 670 `s31_radio_waitq` + all `wake_up(&s31_radio_waitq)`
  (line 1429 `s31_radio_wake`, plus command/HCI/Wi-Fi submit paths) →
  `mod_delayed_work(s31_radio_wq, &s31_radio_work, 0)`.
- Hard-IRQ wake (line 1251-1254 `wake_up_process(worker)`, incl. BT-batching
  `!wake on handled-ACL` condition) → same `mod_delayed_work` (keep the
  batching condition as a separate STEP 4 knob; do not silently change it
  in the runner swap).
- Gate-wait hook guard (line 1347 `current != s31_radio_worker`) →
  `current is a workqueue runner of s31_radio_wq` (check
  `current->worker` membership or a `bool s31_radio_work_running` flag set
  around the work function; same for lines 2022, 2417 `current != worker`
  reschedule/need_resched decisions).
- Loop body (lines 2789-2970 `s31_radio_runtime_thread`): extract one
  iteration (`sync_irq_registrations` → `update_state` → `run_linux_pass` →
  `cond_resched`) into `s31_radio_workfn`; requeue at end with
  `queue_delayed_work(wq, work, usecs_to_jiffies(clamp(timer_wait_us,…)))`
  or 0 when IRQ/command/HCI/Wi-Fi pending (mirror the two wait branches at
  2934-2954). Delete `next_tick`/`tick_period` jiffy accumulation
  (2904-2910, `s31_tick_pending` line 891/2608/2908): instead call
  `s31_rtos_tick()` when `timer_next_due_us() <= elapsed-since-last-tick`
  or combo-mode every pass (preserve 2558-2565 semantics exactly).
- Create/destroy: lines 3158-3174 (`kthread_create`/`kthread_bind`/`wake_up`)
  → `alloc_ordered_workqueue` + `INIT_DELAYED_WORK` + initial queue;
  lines 3266-3272 (`xchg worker`, `wake_up_process`, `kthread_stop`) →
  `cancel_delayed_work_sync` + `destroy_workqueue`. Keep `s31_radio_workerless`
  early-return (3158) and native-task paths untouched. Shutdown ordering:
  cancel work BEFORE `s31_linux_tasks_stop_all` (work may create kthreads
  via `s31_radio_process_commands`); keep `s31_radio_fail_commands` on
  failure paths.
- `s31_radio_bt_disable` wait (line 2355 `wait_event_timeout(s31_radio_waitq`)
  → `wait_event_timeout` on a new `s31_radio_bt_disable_waitq` signaled by
  the work function (workqueue has no directly waitable thread).
- `esp32s31_radio_get_health` guard (line 2022 `current == worker →
  -EDEADLK`) → same guard against workqueue-runner context.

## Payload side (firmware/radio, NO functional change)
- Keep `s31_foreign_tcb` ("radio-worker") as the stable identity for
  gate-held workqueue execution (audit item 11: sharing is already unsafe
  under concurrency; ordered queue keeps single-execution so no new aliasing
  is introduced). Renaming it would churn logs/health with zero benefit.
- Keep `s31_rtos_tick`/`s31_linux_timers_tick`/10 ms `portTICK_PERIOD_MS`
  semantics byte-identical in this patch; tick-period changes are STEP 4
  with separate re-measurement.

## Wi-Fi re-check procedure (must pass after the swap, same setup both times)
1. Boot combo image (Wi-Fi+BT, not BT-only recovery): `mode=combi`? (exact
   param: `/sys/module/esp32s31_radio/parameters/mode`; record value).
2. `esp32-config wifi` STA connect to lab AP (record SSID/channel/RSSI from
   `s31_radio_wifi_scan_complete` path), DHCP, `ping -c 50` gateway RTT +
   `iperf3 -c <host> -t 20` TCP throughput, `radio_health`
   `wifi_rx/tx_dropped` before/after.
3. Repeat BT §3–§4 (LE connect + GATT reads) concurrently with Wi-Fi iperf
   to check coexistence regression (coex phase callbacks, scan-duty 0010).
4. Accept only if Wi-Fi numbers match pre-patch within noise AND BT ATT
   reads complete (the actual bug) — runner swap alone is not a win claim.

## Status
Plan only. Application + rebuild + flash + re-measure pending `make build`
completion (running). Flash must respect
tools/tests/test_s31_hil_flash_safety.py and never wipe persist.
