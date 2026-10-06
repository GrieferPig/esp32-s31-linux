# STEP 1 — radio compatibility layer audit (payload side, before driver changes)

Scope: firmware/radio/s31_rtos/s31_rtos_core.c, s31_rtos_queue.c,
s31_rtos.h, firmware/radio/s31_linux_timer.c, firmware/radio/radio_stack.c,
s31_memcpy.c. Plus directly-coupled s31_linux_locks.c, s31_rtos_event.c,
s31_softmac_tx_agg.inc / s31_softmac_tx_reports.inc where they affect the
same latency/throughput questions. Linux kernel bridge
(s31_linux_task_create/sync/critical, radio worker kthread, 10 ms tick
source) lives in linux-esp32-s31 (drivers/platform/esp32s31-radio-*.c),
which finished fetching during this step; worker-thread analysis (STEP 2)
is in bt-latency/worker-analysis.md.

## A. Tick / timer sloppiness (latency-relevant)

1. `s31_rtos.h:28` — `portTICK_PERIOD_MS 10UL`. Every timeout, delay, and
   tick count in the compat layer is quantized to 10 ms. BLE connection
   intervals start at 7.5 ms; a 10 ms quantum alone can add a full interval
   of jitter to notify/queue/event waits and to esp_timer dispatch.
2. `s31_rtos_core.c:136-149` — `xTaskGetTickCount()` returns
   `s31_linux_tick_count()` (Linux jiffies; kernel
   `esp32s31-radio-smode.c:948-958` divides ms by 10). All
   `*_wait_remaining()` helpers (`s31_rtos_core.c:305-314`,
   `s31_rtos_queue.c:209-218`, `s31_rtos_event.c:135-137`) compute remaining
   time from that 10 ms tick, so short BT waits round up to 10 ms or expire
   late.
3. `s31_linux_timer.c:2,152-199` — esp_timer dispatch runs only in
   `s31_linux_timers_tick()`, called from `s31_rtos_tick()`
   (`s31_rtos_core.c:160-169`) on the Linux radio worker's tick (10 ms
   normally, 40 ms in BT-only batching; kernel
   `esp32s31-radio-smode.c:2793-2809,2904-2910`). Epoch
   (`s31_timer_now_us()`, `s31_linux_timer.c:29-35`) correctly uses
   `s31_linux_time_ns()` at 1 us, so start times are precise but firing is
   up to ~10 ms (40 ms BT-only) late. `late_total_us/late_max_us` tracking
   exists — use `s31_linux_timer_report()` to quantify before changing tick.
4. `s31_linux_timer.c:168-175` — linear scan picks the *first* due timer in
   list order, not the earliest deadline. With several active timers (Wi-Fi
   BA reorder + coex + BT controller timers) a later deadline can fire first.
5. `s31_linux_timer.c:176` — `if (!timer || callbacks++ == 64) break;`
   caps dispatch at 64 callbacks per tick. A burst of >64 due timers slips a
   full tick. Bound should be higher or loop until idle.
6. `s31_linux_timer.c:201-220` — `s31_linux_timer_next_due_us()` exists
   (1 us when overdue, `UINT32_MAX` when idle) and the kernel worker already
   sleeps until `min(timer_wait_us, 10 ms)` in combo mode
   (`esp32s31-radio-smode.c:2929-2942`), but the payload still cannot program
   a tickless wakeup by itself; idle still ticks every 10 ms. Wasted SBI
   wakeups when idle, added latency when a timer is due in e.g. 2 ms and the
   next pass is 10 ms away with no IRQ to pull it forward.
7. `s31_rtos_core.c:288-297` `vTaskDelay()` — `ticks==0` yields, else
   `s31_linux_task_delay(ticks)` with 10 ms units (kernel
   `esp32s31-radio-rtos.c:879-890` converts via `s31_ticks_to_jiffies`,
   `ticks*HZ/100`). IDF `vTaskDelay(pdMS_TO_TICKS(2))` is 0 ticks (yields
   instead of delaying) or 1 tick = 10 ms (5x overshoot). The two in-tree
   users (`radio_stack.c:1550,706-707`: `vTaskDelay(pdMS_TO_TICKS(10) ?: 1)`)
   are exactly 1 tick so correct by accident; any 1–9 ms delay is wrong.
8. `s31_linux_timer.c:70-87` `s31_timer_start()` — no lock around
   `timer->active/alarm_us/period_us`; `s31_linux_timers_tick()` reads the
   same fields from the worker. Concurrent start/stop vs tick races. Same
   for list insert in `__wrap_esp_timer_create` (`37-55`) and remove in
   `__wrap_esp_timer_delete` (`57-68`): singly-linked list mutated without
   critical section.
9. `s31_linux_timer.c:129-140` `__wrap_esp_timer_restart()` — when
   `period_us` is set it overwrites `period_us = timeout_us`. Verify against
   IDF 6.2 `esp_timer_restart` docs (restart-with-new-timeout vs
   same-period) before keeping.
10. `s31_linux_timer.c:238-252` report truncates 64-bit `*_us` to
    `unsigned long` (32-bit RV32) via `%lu`. Diagnostics-only wrap >71 min.

## B. The shared `radio-worker` identity (correctness, blocks STEP 2)

11. `s31_rtos_core.c:29,171-176,547-556` — single static `s31_foreign_tcb`
    named `"radio-worker"`, returned for *every* non-compat entry (worker,
    ISR-deferred, Linux callbacks). Concurrent Linux contexts share one
    `notify_value/state`, `tls[]`, `critical_depth/flags`, mutex-ownership
    identity. Consequences: (a) mutex taken on one entry looks owned on
    another (`s31_rtos_queue.c:434,491` compare `q->owner == t`); (b)
    notify/TLS clobbered across entries; (c) `critical_depth` shared so
    nested critical sections from different entries corrupt each other.
    Any direct-dispatch replacement must give each context a distinct TCB
    or eliminate TCB-identity checks.
12. `s31_rtos_core.c:71-94` `vPortEnter/ExitCritical()` — since
    `s31_rtos_current()` never returns NULL, the `!t` orphan path
    (`s31_orphan_critical_depth/flags`) is dead, but sharing (item 11) makes
    the live path wrong under concurrency. `vPortExitCritical()` silently
    ignores `critical_depth==0` underflow instead of asserting.
13. `s31_rtos.h:40,47-48` — `core_id`, `critical_depth/flags` in TCB but
    foreign TCB has no affinity; `xPortGetCoreID()`
    (`s31_rtos_core.c:488-491`) returns Linux CPU which can differ from
    `t->core_id`, so `xTaskGetCurrentTaskHandleForCore()` (`493-500`)
    mis-reports for worker entries.

## C. Lock contention / needless wakeups and copies (throughput-relevant)

14. `s31_rtos_queue.c:288-318,373-398,431-474`, `s31_rtos_event.c:115-157`,
    `s31_rtos_core.c:327-355,370-404` — every blocking wait does
    `sync_sequence` + `sync_lock` + predicate + `sync_unlock` + `sync_wait`
    + re-lock loop. Each primitive is an SBI round-trip (kernel
    `esp32s31-radio-rtos.c:925-1098`: raw spinlock + waitqueue + hrtimer).
    Fast non-blocking tries still take lock+unlock. No payload-side fast
    path. On A2DP (hundreds of queue ops/s/dir) this is pure overhead.
15. Wakeups unconditional on success: `s31_rtos_queue.c:297,346,380,414,452,
    500,552`, `s31_rtos_event.c:70` call `s31_linux_sync_wake()` (kernel:
    `atomic_inc sequence; wake_up_all`) even with no waiters. Needs
    wake-only-on-transition to avoid an SBI + thundering-herd per packet
    (kernel comment at `1087-1098` already notes a prior global-waitqueue
    100 Hz pathology).
16. `s31_rtos_queue.c:40-78` tracing — `s31_queue_trace_receive()` calls
    `s31_linux_trace_wifi_event(event)` on *every* Wi-Fi main-queue receive
    (`67-68`), even when `s31_queue_trace_enabled` is false. Per-packet
    bridge call on Wi-Fi RX hot path (kernel
    `esp32s31-radio-rtos.c:225-252` then optionally polls native queue).
    Gate behind flag. `s31_queue_is_wifi_main()` (`25-29`) keys on magic
    `cap==200 && item==8`; any BT queue with same shape would be mis-traced.
17. `s31_rtos_queue.c:255-257,267-269` — payloads `memcpy`'d twice per hop
    (sender->queue->receiver). Item sizes here are small (Wi-Fi dispatcher
    8 bytes), so SBI/lock cost dominates; zero-copy needs IDF ABI break.
    Document as accepted, fix locks first.
18. `s31_rtos_event.c:42-44,71-75,108-110,124-126,153-155`,
    `s31_rtos_core.c:468-470,509-512`, `radio_stack.c:977-980,1330-1340` —
    first-N prints via `s31_linux_printf()` (SBI + printk) fire during
    bringup when timing is most sensitive. Bounded but perturbs the connects
    being measured; do not add logging when profiling BT.
19. `s31_linux_locks.c:40-61` — `s31_lock_get()` holds `vPortEnterCritical()`
    across `xQueueCreateMutex()` -> `s31_linux_sync_create()` (SBI alloc).
    Allocate outside, insert under lock with double-check.
20. `s31_linux_locks.c:64-72` — `s31_lock_find()` walks `s31_locks[]`
    without the critical section, racing `s31_lock_get()` insert.
21. `s31_linux_locks.c:11,50-53,77-80` — 32 lock slots; on exhaustion
    `s31_lock_get()` returns NULL and `void _lock_acquire()` proceeds
    *without* the lock. Silent data-race. Must assert or grow table.
22. `s31_linux_locks.c:137-150` — lazy `_lock_init` on first acquire races
    if two tasks init the same `_lock_t` concurrently.

## D. Queue / notify / event semantic bugs

23. `s31_rtos_queue.c:242-254` head discipline is correct but header comment
    (`s31_rtos.h:84`: "next free slot index") is wrong; head is the *read*
    index (`take` reads at head, `send` appends at `(head+count)%cap`). Fix
    comment so STEP 2 refactors don't "fix" working index math.
24. `s31_rtos_queue.c:375` — `xQueueReceive()` on a mutex falls through to
    wait loop instead of failing fast. Harmless if IDF never does it;
    latency trap if it does.
25. `s31_rtos_queue.c:428` — `xQueueSemaphoreTake()` fails if `!t`. Currently
    unreachable (current never NULL), but after STEP 2 gives each context a
    real TCB, keep the ISR diversion (`s31_linux_locks.c:81-82,94-95`).
26. `s31_rtos_core.c:447-460` — `vTaskSuspendAll()/xTaskResumeAll()` no-ops;
    `vTaskSuspend()` only self-suspends, silently ignoring other-task
    suspend. Grep IDF a602e67b linked Wi-Fi/BT libs for
    `vTaskSuspendAll` users before STEP 2.
27. `s31_rtos_core.c:519-524` `vTaskDelete()` — other-task delete calls
    `s31_linux_task_stop()` (kernel `esp32s31-radio-rtos.c:805-833`) but
    never frees `stack_base/notify/suspend/TCB`; relies on kernel
    `s31_linux_task_cleanup()` -> `s31_rtos_task_release()` (kernel
    `612-641`). Confirm that path runs (it does on normal return and
    self-delete longjmp) or this leaks 8 KiB + TCB per deleted task across
    enable/disable cycles.
28. `s31_rtos_core.c:235-244` — every stack rounded to >=8192 bytes internal
    SRAM. Correct per the cited kernel-frame overflow, but Wi-Fi+BT+coex
    tasks cost tens of KiB HP SRAM. Check
    `s31_radio_heap_report("after-bt-enable")` free before shrinking.
29. `s31_rtos_queue.c:326-352` ISR send preserves prior `*woken=TRUE`
    (`350-351`). Keep in any rewrite; easy to regress.

## E. `s31_memcpy.c` (correct but incomplete for MMIO)

30. `s31_memcpy.c:14-33` — word fast path only when *both* pointers aligned
    and `n>=4`. Correct for the CCMP key-slot latch bug, but unaligned
    key/ID paths still use byte stores and recorrupt. Assert 4-byte
    alignment at `hal_crypto_set_key_entry` wrapper
    (`radio_stack.c:761-785`) or add RMW word path for MMIO tails.
31. `s31_memcpy.c:35-42` — `memset()` still byte-oriented; same latch bug if
    ever used on MMIO (softmac key-remove at `s31_softmac.inc:43-56` uses
    word stores directly — good). Make `memset` word-aware when aligned or
    forbid MMIO use with comment.
32. No `memmove()` override — overlapping moves use picolibc byte copy. Fine
    for DRAM; confirm no MMIO path uses `memmove`.

## F. `radio_stack.c` / softmac includes (what is *not* the worker)

33. `radio_stack.c:239-241` — `s31_radio_coex_worker_tick()` is an empty
    stub. Coexistence, if any, is in closed `libcoexist` callbacks
    (`376-414`), not on the 10 ms tick. Do not "fix" BT latency by adding
    work to this stub without profiling; the win is removing the tick, not
    feeding it.
34. `radio_stack.c:1210-1220,558-561` VHCI — `s31_radio_vhci_try_send()`
    checks `esp_vhci_host_check_send_available()` once, returns -1 if busy.
    No payload queue. Transient busy under A2DP+Wi-Fi becomes host drop/
    retry unless the kernel TX ring (`esp32s31-radio-smode.c:1623-1670`
    `s31_radio_hci_process_tx`, `2079-2114` `esp32s31_radio_hci_send`)
    absorbs it. Count -1 returns under load to distinguish "worker slow"
    from "controller busy". Same single-attempt pattern in Wi-Fi
    `s31_radio_wifi_try_send()` (`1318-1342`).
35. `radio_stack.c:918-925` `s31_wifi_tx_done()` ignores
    `data/length/status` except counting. Fire-and-forget today.
36. `s31_softmac_tx_agg.inc`, `s31_softmac_tx_reports.inc` — Wi-Fi-only
    (TID0/STA/HT20/HE-SU20). No BLE/Classic MTU, connection-interval, or
    ACL-aggregation knobs here. BT equivalents: closed BT controller +
    BTstack config (`s31_btstack_config.h`: ACL payload 1691+4, host ACL 4,
    SCO 255x10, completed-batch 4) and controller defaults (scan duty
    `0010`, EDR types `0014`, flow-control `0015`, credit batching `0017`).
    Tune there, not softmac.
37. `radio_stack.c:1616-1773` bringup — `s31_radio_stack_task()`,
    `s31_radio_bt_enable_task()`, Wi-Fi tasks are all
    `xTaskCreatePinnedToCore` tasks through the same bridge as the worker.
    STEP 2 must keep creation working; only the *serialization* (single
    worker + tick) goes away, not task creation.

## G. What to measure before fixing (hooks already present)

- `s31_linux_timer_report()` + `late_avg/late_max` per timer: 10 ms
  dispatch lateness on BT controller timers.
- `s31_queue_trace_*` + `s31_linux_trace_wifi_event()`: queue SBI rate;
  gate receive-side call first or it perturbs measurement.
- Count `-1` from `s31_radio_vhci_try_send()` / `s31_radio_wifi_try_send()`
  under load: "worker slow" vs "controller busy".
- `s31_radio_heap_report("after-bt-enable")`: SRAM headroom.

Method: full read of the six listed files plus lock/event/softmac
companions; kernel worker source (linux-esp32-s31) fetched mid-step and
used for Section F cross-references and bt-latency/worker-analysis.md.
No code changed in this step.
