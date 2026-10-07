# Timer cadence with deep HCI queues
Same dist2944dc3068ffd7c6 and continuing BTstack PID297 from the final local
callback run: local1, controller10, RX32/TX16, CPU1, gate_sleep1, SPP990,
coalescing0, COEX0, PAIRABLE1, packet logging off, profiler enabled.
Only bt_tick_ms changes, fresh agent/pair and checked40s receiver per run.

python3 -u bt-latency/tick_matrix.py spp bt-latency/logs/deep-tick-cadence/runs 40 10 1 40

|Run|Host bytes|Seconds|Host KiB/s|CPU busy %|Worker passes|
|---|---:|---:|---:|---:|---:|
|spp-tick1-2|7388370|40.000205|180.379202|88.716|7544|
|spp-tick10-1|7327980|40.000178|178.904965|85.577|5797|
|spp-tick40-0|7280460|40.000208|177.744681|86.910|5465|
|spp-tick40-3|7347780|40.000143|179.388518|84.418|5098|
All streams have zero gaps/duplicates/payload errors/partial frames and zero
HCI RX/TX rejection/drop increments. CPU/pass deltas include connection and
snapshot overhead; throughput is only the40s receive window.
1ms adds worker passes without a material rate improvement; keep40ms.
Classic remains~180 vs native~214KiB/s. Next isolate radio workqueue CPU
placement using workqueue attributes, not taskset on a shared worker.
