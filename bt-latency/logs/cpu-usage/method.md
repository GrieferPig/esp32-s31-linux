# CPU comparison method
Same physical ESP32-S31 and BasedSurface Bluetooth peer, CPU320MHz, two cores.
Linux image e168e8c0f293a4d4, HZ100, BTstackCPU1/RR20, controller10,
RX32/TX16, sleeping gate1, local callbacks1, no packet logging.
Separate BLE495B/15ms/1M and SPP990B transfers, two40s repeats each.
Native uses the same API benchmark sources/payloads as the reference:
BLE-only controller forBLE, Classic-only forSPP, FreeRTOS1000Hz.
This compares the complete implementations/configurations; it is not a
controlled measurement of the OS alone. Native SDK source remains untouched.

CPU is sampled in a30-second steady window starting about5s into each stream.
100% total capacity means both cores fully busy; 50% equals one full core.
Full40s throughput accompanies the CPU window and all payloads are validated.

Linux:
A5540-byte temporary musl executable brackets two/proc/stat reads with
CLOCK_MONOTONIC and sleeps between them. No per-process scanning or UART
output during the window. Uploaded binary hash verified; timestamps bound
the snapshot uncertainty. Use (wall minus NO_HZ idle+iowait)/wall per core.
The actual kernel's fs/proc/stat.c get_idle_time uses get_cpu_idle_time_us,
whose kernel/time/tick-sched.c implementation accounts elapsed time using
ktime, not sampled user/system ticks. CONFIG_NO_HZ_IDLE/HIGH_RES_TIMERS=y.
USER_HZ is100, independent of the kernel tick configuration.
A first exploratory shell/proc-task sampler showed a material tick-accounting
deficit. Its linux-spp/ files remain, excluded from accepted comparison.
The final linux-*-idle cases preserve that deficit as a diagnostic and
normalize against measured wall time instead of incomplete tick totals.

ESP-IDF:
Enable FreeRTOS runtime stats using esp_timer/U32 in isolated build copies.
A low-priority sampler pinnedCPU0 snapshots uxTaskGetSystemState at5s and35s.
An IPC callback schedules core1 at each boundary to flush its currently
running idle-task runtime. CPU0's sampler wake flushes that core naturally.
Task names/IDs/counters are copied at snapshot time. No periodic observer
runs in-window. UART output is delayed until42s, after40s transfer ends.
Require bothIDLE tasks and98–102% summed-task runtime coverage of two-core
wall time; preserve all raw runtime counts. Counter wrap handled explicitly.
Compare instrumented throughput against the prior uninstrumented references.

Accounting limitation:
FreeRTOS task runtime does not separately account interrupt time; interrupts
executing while its idle task is current may be charged to idle. Linux idle
residency is also an OS accounting metric, not a hardware cycle/power trace.
Native task-based busy figures can therefore undercount ISR work. Do not
interpret the cross-platform percentages as exact cycle-level equivalence,
per-function costs, or power consumption.
