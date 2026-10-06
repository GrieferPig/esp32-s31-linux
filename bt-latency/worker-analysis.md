# STEP 2 analysis — why the dedicated radio worker exists, and what
# "removing" it would actually require (no code changed yet)

Linux worker: `s31_radio_runtime_thread` ("s31-radio" kthread),
linux-esp32-s31/drivers/platform/esp32s31-radio-smode.c:2789-2970.
Payload identity: single static `s31_foreign_tcb` named "radio-worker",
firmware/radio/s31_rtos/s31_rtos_core.c:29,550-551,171-176.

## What the worker does every pass
`s31_radio_run_linux_pass()` (smode.c:2605-2662):
- Outside blob gate (Linux context): `s31_radio_hci_deliver_rx()` (bounded 8),
  `s31_radio_wifi_deliver_scan/control_events`, `s31_radio_process_commands()`
  (kthread_create for bt-enable/wifi-connect/etc — must run on normal kernel
  stack before blocking on the gate, smode.c:2628-2632).
- Inside blob gate (`s31_radio_run_blob_pass`, smode.c:2529-2603):
  `s31_rtos_tick()` (esp_timer + empty coex stub), deferred IRQ callbacks,
  `s31_radio_hci_process_tx()` (VHCI send), `s31_radio_wifi_process_tx()`,
  inc `s31_radio_worker_passes`, leave gate, `cond_resched()` so compat tasks
  (Wi-Fi/BTDM, RR/CFS) can enter the gate next.
- Sleep on `s31_radio_waitq` until tick/IRQ/command/HCI-RX-TX/Wi-Fi-TX or
  `timer_wait_us` (50 us..10 ms combo; next 10/40 ms tick otherwise).

Tick: 10 ms (`init_tick_period`, HZ=100 jiffies time base,
smode.c:948-958,2797-2798); 40 ms BT-only batching
(`bt_tick_period`, smode.c:2799-2800,2892-2899). In combo,
`s31_rtos_tick()` runs on *every* pass, not just tick events
(smode.c:2558-2565), so IRQ/HCI activity already pulls timers forward.

## Why it exists (from code, not guesswork)
1. Gate-holder execution domain. Compat tasks block on the blob mutex
   (rtos.c:1162-1201 enter/leave/suspend/resume). When all are blocked and a
   timer expires or IRQ arrives, nobody holds the gate to run the callback.
   The worker is the designated gate acquirer for that case. Deleting it
   without a replacement leaves timer/IRQ/TX work with no runner.
2. IRQ deferral target. Hard IRQ (`s31_radio_hardirq`, smode.c:1212-1256)
   tries direct inline ISR (`s31_linux_blob_run_direct_isr`); on
   DEFER_* it masks the source and wakes the worker. Native nesting is only
   safe for compat tasks (rtos.c:522-576 comments); the worker's foreign
   context explicitly cannot nest (same comment). Without the worker,
   deferred callbacks never run.
3. Command kthread creation context (smode.c:2628-2632,1903-2006).
4. RX delivery in Linux context (skb/netif_rx must not run under the blob
   gate/SRAM stack), plus TX-ring wakeups and health reporting.

## Why naive removal is unsafe
- Tasklet: runs in softirq, cannot sleep for the blob mutex. Blob pass
  sleeps. Unsuitable.
- Shared workqueue for the blob pass: still an async thread (just not
  dedicated); preserves serialization only with a single-threaded/ordered
  queue, adds pool-contention jitter, and still needs the 10 ms/hrtimer tick
  source. Saves one kthread, does not remove latency; likely worse.
- Pure inline on the calling path: callers are (a) hard IRQ (cannot sleep),
  (b) payload context already holding the gate (`s31_radio_vhci_receive`
  wakes only on EVENT precisely to avoid re-entry, smode.c:1438-1445,
  1509-1510), (c) process context HCI/Wi-Fi send (could run inline, but then
  who runs timers when the system is idle with all tasks blocked?). A fully
  inline design needs a per-caller "if gate held, mark pending; gate release
  runs pending work before unlock" protocol plus an idle tick source — i.e. a
  worker by another name, with higher deadlock risk (see shared foreign-TCB
  aliasing, audit item 11).
- Board observation supports caution: the Oct-4 board image reports
  "btdm native task service; no s31-radio thread" with `worker_passes=0`
  (radio_health) — i.e. it already runs BT without this worker — and BLE
  GATT reads stall indefinitely (see bt-latency/logs/before-*.log) while
  LE connect (10.1 s) succeeds. Removing the runner without fixing
  wakeup/response delivery reproduces exactly this symptom.

## Recommendation (profile first, per STEP 4)
Do NOT delete the kthread as a latency fix. The measured combo path already
dispatches timers on every wakeup; the BT-only 40 ms batching + ACL-RX
coalescing (smode.c:1506-1510, hardirq 1242-1254) is the concrete added
latency to profile against the 10 ms combo cadence. If a "worker gone"
demonstration is still required, the safe equivalent is: keep ONE execution
domain for gate-held work but make it event-driven (hrtimer for
`timer_next_due_us`, immediate wake on EVENT + IRQ, no 40 ms batching, no
ACL coalescing), and give each dispatch context a distinct TCB (fix audit
item 11). That deletes the *periodic 10/40 ms tick*, not the *runner* — and
even that must be re-measured with Wi-Fi STA checks before claiming safety.

No code changed in this analysis step; implementation (if pursued) belongs in
linux-esp32-s31/drivers/platform/esp32s31-radio-smode.c +
firmware/radio/s31_rtos/s31_rtos_core.c (foreign-TCB) with Wi-Fi + BT
re-measurement. See NEXT in bt-latency/status.md.
