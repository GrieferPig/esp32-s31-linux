# Scheduler-aware path cost diagnostic

Purpose: distinguish radio scheduling/wakeup/gate/IRQ costs from BTstack syscall-body execution without assigning delayed timer samples to IRQ-unmask successor instructions.

Diagnostic dist 559a02aabaf6fdca. Accepted restore dist e168e8c0f293a4d4. Same real basedsurface adapter (LE1M), board, CPU1/RR20 BTstack, HZ100, gate sleep, 40ms tick, burst24 and checked host receiver. No packet logging. Each protocol runs disabled/enabled/enabled/disabled. Transfers last 40 seconds; collection starts five seconds after first received data and runs through a 30-second idle-counter CPU probe.

The patch records timestamps at annotated nested scope boundaries and immediately before context switches. Task scope survives sleep and migration. CPU state switches to the next task, so sleeping waits are excluded. IRQ and softIRQ nesting overrides task state; nested payload callbacks override integration until they return. All states partition each CPU's start-to-end window. Counters are frozen on both CPUs before UART readout.

Actors are radio tasks/ordered worker, BTstack (comm prefix), and other. Radio tasks use payload as their base; ordered worker uses integration. Scheduler scope is charged to the outgoing/running actor, excluding off-CPU waits. Direct and deferred IRQ callbacks are scoped as payload. Gate, synchronization, queue and syscall scopes take precedence when nested.

Limitations:
- This is occupancy timing, including instrumentation and boundary overhead; not hardware instruction cycles or causal proof of an optimization.
- Payload includes controller code and open payload shim/timer scanning; it is not an exclusive closed-blob measurement.
- Syscall categories cover the syscall body; architecture entry/exit, trap overhead and BTstack userspace fall into BTstack outside.
- Generic wake/scheduler work inside gate/queue/sync is charged to that enclosing path except the explicitly scoped __schedule body.
- IRQ actor is the interrupted task, not necessarily the IRQ source.
- The radio worker remains radio-tagged; surrounding workqueue execution can be included in integration.
- Profiling-disabled runs still execute inexpensive category/IRQ nesting maintenance in the diagnostic kernel. Accepted image measurements from earlier commits are the baseline for that fixed overhead.

Reproduction from repository root:
1. Apply diagnostic.patch to the matching kernel source, after a scratch patch -p1 pass.
2. Run build-command.txt and freeze.py, preserving the exact manifest and ELF hashes.
3. python3 -u bt-latency/logs/sched-paths/flash.py 559a02aabaf6fdca
4. python3 -u bt-latency/logs/sched-paths/run.py
5. python3 -u bt-latency/logs/sched-paths/flash.py e168e8c0f293a4d4
6. python3 -u bt-latency/logs/sched-paths/restore-check.py

The six explicit verification matches are required. Persist [0x1ee000,0x400000) is outside all slot writes. Individual board commands, host pairing/benchmark commands, frozen statistics, SHA256-checked bounded gzip/base64 captures and CPU results are recorded per run. analyze.py rejects invalid records, duplicate/missing cost records, nonzero clock/state errors, and any failure to conserve each CPU's exact elapsed nanoseconds.
