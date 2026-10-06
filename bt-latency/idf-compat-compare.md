# Linux compat layer vs ESP-IDF reference (latency-relevant differences)

Reference: local ESP-IDF 6.2 (`08e0d30a`, `$HOME/esp-idf`). S31 side:
`firmware/radio/*`, `linux-esp32-s31/drivers/platform/esp32s31-radio-*.c`.

## 1. Time base and timer dispatch (the big one)
- IDF: FreeRTOS tick default 100 Hz (`FREERTOS_HZ`, default 100,
  `components/freertos/Kconfig:33-39`) — same 10 ms quantum as S31's
  `portTICK_PERIOD_MS 10` (`firmware/radio/s31_rtos/s31_rtos.h:28`). The
  quantum is NOT the differentiator.
- IDF esp_timer: hardware systimer alarm + IRAM ISR
  (`timer_alarm_isr`, `components/esp_timer/src/esp_timer_impl_systimer.c:87+`)
  → ISR-method callbacks run IN the ISR at µs precision; task-method
  callbacks are dispatched by a dedicated esp_timer task. Time base is the
  always-on systimer counter.
- S31: epoch is fine (`s31_timer_now_us()` reads Linux ktime at 1 µs,
  `firmware/radio/s31_linux_timer.c:29-35`), but DISPATCH happens only when
  a serialized pass runs `s31_linux_timers_tick()`. Measured idle cadence:
  67.5 passes/s (~15 ms granularity); under load, passes run per event
  (fast). A 1 ms coexistence timer can therefore fire up to ~15 ms late
  when idle — IDF fires it from the systimer ISR within µs. The 10 ms
  tick is inherited from IDF; the missing piece is a hardware-timed
  dispatch path, which S-mode Linux cannot provide — the hrtimer-driven
  requeue (`s31_radio_hrtimer`, smode.c) is the closest equivalent and is
  what bounds idle latency now.
- Consequence for BLE: controller-internal timing (connection anchors,
  supervision) is hardware-owned on both; host-side supervision of
  timers (retransmit, BA reorder, coex phases) is what slips on S31 idle.

## 2. Task priority mapping (combo shares everything)
- IDF Bluedroid BTU task: `BTU_TASK_PRIO = BT_TASK_MAX_PRIORITIES - 5`
  (near top), pinned core, own workqueue
  (`components/bt/host/bluedroid/stack/btu/btu_init.c:50-55`). The
  controller/BTDM runs above Wi-Fi data-path tasks by fixed priority.
- S31 (`esp32s31-radio-rtos.c:697-713`): name `btdm` + integer-only payload
  → `SCHED_RR/80` in BT-only mode (real preemption, closest to IDF), but
  plain CFS `nice -10` in combo — the SAME nice as the `wifi` task.
  So in Wi-Fi+BT combo there is no strict BT-over-Wi-Fi ordering like
  IDF's; both contend as equal CFS tasks and only the coex callbacks
  arbitrate the radio. Any Classic/BLE jitter under Wi-Fi load that IDF
  would serialize by priority must be re-proven here (see combo streaming
  measurements).
- The `vTaskPrioritySet` path preserves the mapping on change
  (`s31_linux_task_set_priority`, rtos.c:897+); first-32-changes printk
  diagnostics are bounded.

## 3. VHCI data path hop count
- IDF (`components/bt/host/bluedroid/api/esp_bluedroid_hci.c:106`):
  `esp_vhci_host_send_packet(data, len)` calls straight into the linked
  controller on the calling task — zero hops, backpressure by polling
  `esp_vhci_host_check_send_available()`.
- S31 keeps the same wrapper shape (`s31_radio_vhci_try_send`,
  `radio_stack.c:1210+`: single attempt, -1 when busy) but the path is:
  BTstack userspace → `/dev/s31-hci` chardev → SRAM TX ring →
  ordered-workqueue pass (gate) → payload → controller, with RX symmetric
  plus a bounded 8-frame delivery batch. Each hop is cheap (measured
  syscall avg ~178 µs in the meta log), but a full round trip spans
  several scheduler transitions where IDF spans none. The single-attempt
  `-1` return is the throughput cliff to watch: under load, count these
  before blaming aggregation (unchanged from the audit).
- Host flow-control posture matches IDF Bluedroid (ACL-only,
  `S31_HCI_ACL_ONLY_FLOW_CONTROL`, 4 host buffers vs controller-reported
  1021×4), and credit batching (0017, batch 4) mirrors Bluedroid's
  Host-Number-Of-Completed-Packets batching.

## 4. PHY / connection-interval policy (reference for Stages B–D)
- IDF Bluedroid NEVER auto-prefers 2M: PHY changes are app-driven
  (`BTM_BleSetPreferPhy` ← `bta_dm_act.c:5827`, default PHY 1M
  (`le_connection_phys = 0x01`, hci.c:5604)). Our `gap_le_set_phy()`
  call is the exact analog — no reference deviation.
- IDF Bluedroid central defaults: interval 10–30 ms, latency 4,
  supervision 720 ms (hci.c:5590-5595). Our measured stock link (BlueZ
  central chose 45 ms / 420 ms) is looser on interval and tighter on
  supervision than IDF's own central would use — the peripheral-side
  update request (7.5–15 ms / 2000 ms) moves toward and past the IDF
  reference, justified by measurement, not by matching IDF.
- DLE: IDF/Bluedroid and BTstack both gate it on explicit enablement
  (`ENABLE_LE_DATA_LENGTH_EXTENSION`, BTstack sends Set-Data-Length only
  via `gap_le_set_data_length()` task). Neither stack auto-negotiates it
  on this path; explicit request (Stage C) matches both references.

## 5. What was deliberately NOT copied from IDF
- 100 Hz compat tick kept (matches `FREERTOS_HZ` default; changing it
  would desync every `pdMS_TO_TICKS` user in the closed libs).
- ISR-method esp_timer callbacks: impossible without the systimer IRQ in
  S-mode; all callbacks run serialized in passes (documented, accepted).
- `vTaskSuspendAll` scheduler-lock semantics: no-op on S31 (audit item);
  no IDF lib in the closure was observed to depend on it for correctness
  (queue/event paths use gate + sync primitives instead).
