# bt-latency-fix status (updated 2026-10-06, commit: audit + before-logs)

Done in this commit:
- STEP 0 partial: submodules initialized (all pinned, no `-`/`+`), branch
  `bt-latency-fix`, IDF choice recorded (`S31_ALLOW_UNPINNED=1` with local
  08e0d30a vs pinned a602e67b), BTstack source fetched. `make doctor` still
  red (no toolchain) and `make fetch`/`make build` not run yet.
- STEP 1: payload audit in bt-latency/audit.md (38 items with file:line).
- STEP 2 analysis (no code change): bt-latency/worker-analysis.md — worker
  exists to serialize the blob gate + run deferred IRQ/timer/TX/command work;
  tasklet cannot sleep for the mutex, shared workqueue keeps an async thread
  with pool jitter, pure inline deadlocks when the caller holds the gate and
  leaves idle timers/IRQs with no runner (plus shared foreign-TCB aliasing).
  Board's Oct-4 image already runs BT with `worker_passes=0` ("no s31-radio
  thread") and GATT reads stall — deleting the runner without fixing
  wakeup/delivery reproduces the symptom.
- STEP 3 partial (BEFORE only): bt-latency/logs/before-measurements.md with
  exact commands + raw numbers. LE connect works (10.1 s); GATT reads stall
  (>55 s, 0 B/s); Classic pair fails AuthenticationFailed (9.9 s); SDP
  stalls; l2ping blocked (no root); Wi-Fi blocked (mode=bt + dummy SSID +
  recovery cmdline).

NEXT (blocking):
1. Decide the code change. Candidates: (a) keep the kthread but delete the
   *periodic tick* (event-driven hrtimer for `timer_next_due_us`, no 40 ms
   BT batching, no ACL-RX coalescing, distinct TCB per context); (b) extend
   the existing workerless/native path to BT/combo with the same wakeup
   guarantees + Wi-Fi STA re-check. Both need profiling first:
   `s31_linux_timer_report` lateness, `-1` counts at `s31_radio_vhci_try_send`
   / `s31_radio_wifi_try_send`, BTstack HCI log (`-l` file, not `none`) for
   the SMP/ATT stall, and BlueZ `btmon` (needs root — currently unavailable).
2. Build: `make fetch` (toolchain ~GB + buildroot downloads) then `make build`
   (uboot/linux/rootfs/radio-image). Long; run with generous timeouts.
3. Flash + AFTER: respect tools/tests/test_s31_hil_flash_safety.py, never wipe
   persist, use existing-image flash targets; then rerun §1–§8 same-setup for
   AFTER numbers + Wi-Fi STA connect/throughput. Pairing failure (§5) and GATT
   stall (§4) must be root-caused (SMP/ATT trace) before claiming a
   throughput win — do not tune softmac/BTstack MTU blindly.
4. Uplink risk: board image source ≠ pinned linux-esp32-s31 52dc6ea
   ("btdm native…" string absent here); identify the Oct-4 build commit
   before rebasing the fix, or AFTER numbers won't compare like-for-like.
