# Local callback dispatch with deeper queues
Same dist2944dc3068ffd7c6, controller10, Linux RX32/TX16, CPU1,
gate_sleep1, timer40ms, SPP990, coalescing0, COEX0, PAIRABLE1,
packet logging off and profiler enabled. Module parameter snapshot retained.
Only S31_BTSTACK_LOCAL_CALLBACKS changes; fresh agent/pair each checked40s run.

python3 -u bt-latency/local_callback_matrix.py spp bt-latency/logs/deep-local-callbacks/runs 0 1 0 1

|Run|Host bytes|Seconds|Host KiB/s|Aggregate CPU busy %|BTstack CPU seconds|
|---|---:|---:|---:|---:|---:|
|spp-mode0-0|7185420|40.000192|175.424452|91.040|39.20|
|spp-mode0-2|7444800|40.000216|181.756829|91.440|40.43|
|spp-mode1-1|7431930|40.000139|181.442972|83.847|28.96|
|spp-mode1-3|7464600|40.000175|182.240415|84.501|28.98|
All streams have zero sequence/pattern errors, duplicates, partial frames,
and HCI RX/TX rejection/drop increments. CPU uses USER_HZ100 process deltas
over the surrounding snapshot window, including connect/snapshot overhead.
Local1 gives181.44/182.24 vs175.42/181.76KiB/s; small speed difference overlaps
run variation, but CPU reduction is consistent (BTstack~29vs39–40s).
Unlike the smaller pipeline experiment, local dispatch no longer regresses
throughput. Enable it explicitly for the next deep-pipeline cadence control;
global default remains off. Classic parity~214KiB/s remains unmet.
