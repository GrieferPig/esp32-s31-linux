# Callback wake coalescing: not adopted for throughput
Patch0034 leaves legacy behavior as default. S31_BTSTACK_COALESCE_WAKEUPS=1
coalesces empty-to-nonempty callback pipe wakeups under the existing mutex.
Scratch patch -p1 application matched the expected source exactly.
Actual patched source passed40 host correctness runs: recursive512 and
recursive512 plus2000 cross-thread callbacks, no losses/duplicates.
Pure512 callback chain pipe writes:512 legacy versus1 coalesced.
This host result does NOT establish board performance.

Linux dist7c5a6c6799d9b61b full six-slot flash and explicit six digest matches.
Persist excluded. Timer40ms, SPP990B, PAIRABLE1, COEX0, no packet logging.
python3 -u bt-latency/callback_matrix.py spp bt-latency/logs/callback-wakeups/runs 0 1 0 1

|Mode|Host KiB/s|CPU busy around receive|Data/HCI errors|
|---|---:|---:|---|
|Legacy|102.915056|88.01%|0|
|Coalesced|90.443228|85.66%|0|
|Legacy repeat|96.147508|90.53%|0|
|Coalesced repeat|87.808857|85.96%|0|

Each transfer40s with checked sequence/pattern and raw receive bytes.
CPU windows include connect/discovery and snapshot overhead; compare with care.
Per-task data shows less BTstack CPU per frame, but no throughput benefit.
Keep legacy mode. Native target213.71-214.39KiB/s remains unmet.
The next control measures native SPP at100Hz FreeRTOS versus the original
1000Hz baseline, before attributing the difference to the Linux bridge.
