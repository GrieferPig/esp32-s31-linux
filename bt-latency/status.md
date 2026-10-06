# bt-latency-fix status (final 2026-10-06)

## Completed and committed
- STEP 0: submodules pinned, `make doctor` pass, `make fetch` exit 0,
  `make build` + `make image` exit 0. Invocation:
  `PATH="$HOME/.local/bin:$PATH"
  ESP_TOOLS="$HOME/.espressif/tools/riscv32-esp-elf/esp-16.1.0_20260609/riscv32-esp-elf"
  S31_ALLOW_UNPINNED=1 make build image`.
  Host-only fixes in bt-latency/host-build-fixes.md; one tracked compat fix
  (`-mespv-spec=` probe in gen_s31_pie_cases.sh, GCC 15 path kept).
- STEP 1: 38-item payload audit (bt-latency/audit.md), files verified
  unchanged before editing.
- STEP 2: dedicated `s31-radio` kthread DELETED, replaced by ordered
  workqueue + deadline hrtimer
  (`linux-esp32-s31@632d31313a7ef`, `@3d0953ecdc1df` first-pass creation +
  non-kthread guard, `@ce3917f` workerless-teardown guard). No payload
  functional change. Implementation: step2-implementation.md; plan:
  worker-removal-plan.md; analysis: worker-analysis.md.
- STEP 3 BEFORE: bt-latency/logs/before-measurements.md (+diagnostics
  addendum). STEP 3 AFTER: bt-latency/logs/after-measurements.md.
- STEP 4: profile-first suspect matrix (step4-profile.md). No blind tuning
  applied; the measured stall (LE-ATT on the Oct-4 image) is gone on the
  new image with event-driven delivery.
- Flash: full matched sets hash-verified (dist 4a080ca03e4ab94f then
  4bbdfb33bbbf56cf), persist never touched (still corrupt from before;
  RAM-only configs used). Initial 2M-baud flash failure recovered via
  verify-flash + 921600-baud rewrite; procedure recorded in logs.

## How the AFTER run went (honest log)
- First flashed image hung in module init: init called payload task APIs
  from insmod process context (upstream never did). Fixed by creating the
  radio-init task on the first serialized pass + PF_KTHREAD guard.
- Wi-Fi→combo transition oopsed (teardown on uninitialized timer/work in
  workerless mode). Fixed with existence guard + unpublish-first ordering;
  transitions now clean with zero oops/BUG/panic.
- Host BlueZ sessions are flaky (stale agents, cache expiry); one-shot
  invocations + fresh scans used throughout; UART needs single continuous
  sessions (reopen flakiness observed, worked around, not fully explained).

## Verdict: NEXT (numbers recorded; two blockers remain)
Worker gone, BLE fixed with numbers, Wi-Fi coexistence validated as far as
the environment permits. NOT done because:
1. Classic pairing fails identically before/after
   (`AuthenticationFailed`; board SM NoInputNoOutput/auth-req-0 vs host
   agent). This blocks Classic bonded/bulk throughput numbers and any
   A2DP-streaming test. Needs SMP packet trace (BTstack `-l` + root
   `btmon`, unavailable unprivileged) + SM/agent fix — a separate task.
2. No authorized Wi-Fi AP and no host root: STA association + iperf
   throughput and `l2ping` RTT are unobtainable here. Wi-Fi evidence is
   interface-up + working scans + zero drops + clean transitions.
3. Persist partition is corrupt (pre-existing JFFS2 damage; kernel refuses
   to mount, system runs recovery read-only root). All AFTER tests used
   RAM-only configs with zero MTD writes. Restoring persist (reformat)
   is explicitly out of scope (never wipe persist) — needs the lab owner.
