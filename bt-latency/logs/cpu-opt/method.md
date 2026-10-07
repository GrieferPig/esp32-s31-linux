# CPU optimization experiment
Candidate 952277019477208a contains opt-in experiments; legacy behavior is the
same-image control. Keep dist e168e8c0f293a4d4 available for full-slot rollback.
Do not write persist. Six-slot flash plus six explicit hashes required.

Use the accepted 320 MHz two-core setup, 40-second checked host-fd transfers,
30-second wall-minus-NO_HZ-idle CPU windows starting five seconds into payload.
100% CPU capacity means both cores busy. No packet log or per-task scanner
inside the CPU window. Receiver bytes/pattern/sequence and final HCI drops
must remain clean. Compare bracketing baselines, then repeat useful settings.
A lower sampled tick count alone does not establish a CPU reduction.

Cases:
- base: TX batching0, local drain shortcut0, precise clock, normal worker reset.
- tx4 / tx8: up to4 /8 copied H4 records per kernel write; one worker wake per
  accepted batch. Remaining packets survive EAGAIN and partial acceptance.
- tx4drain: tx4 plus at most4 callback-only rounds before polling RX/timers.
- coarse: CLOCK_MONOTONIC_COARSE throughout the POSIX timer base, other options off.
- policy: skip resetting an already-SCHED_NORMAL BT-only worker, otherwise
  preserve the original policy/priority/gate behavior.
- all: combines tx4, drain, coarse and policy; use only if individual results warrant it.

The batch ABI is marker0xff followed by LE16 length and H4 frame, max8 frames.
The kernel validates the entire envelope before enqueueing any frame and
returns only a whole-record accepted prefix. Each individual enqueue retains
its existing IRQ-off interval; there is no enlarged batch-wide critical section.
The user transport copies packet data before reporting reusable buffer space,
holds the final completion when full, and retries an ordered unsent suffix.
Batch counters verify that the path is exercised.

RV32 builds exclude vgettimeofday from vDSO and this clocksource declares
VDSO_CLOCKMODE_NONE. No high-resolution vDSO fast clock is available here.
Coarse time is a measurable alternative with lower precision, not a claim
that vDSO is enabled. It must be rejected if it hurts throughput.

Keep only validated useful production changes. Preserve rejected experiments
as evidence with their exact source/patch and disabled/reverted final status.
