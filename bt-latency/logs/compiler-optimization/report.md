# Same-image BTstack compiler optimization
Distdd46b7df4a5eef9e; complete six-slot flash and six digest matches.
Persist excluded. Established PATH/ESP_TOOLS/S31_ALLOW_UNPINNED make build image.
Packaging diff scratch-applied exactly. Both variants build from the same
patched sources and flags, differing only-Os versus-O2; no LTO.
Default/usr/sbin/s31-btstack-a2dp remains-Os. Diagnostic alternate is
/usr/sbin/s31-btstack-a2dp-o2. Board hashes match binary-manifest.json.
Stripped binaries222588B(Os) and259452B(O2).

Completed matrix uses one boot/controller setup: controller10, RX32/TX16,
worker unpinned, BTstackCPU1, local callbacks1, coalescing0, gate_sleep1,
timer40, SPP990, COEX0, PAIRABLE1, no pklg, profiler enabled.
Fresh host agent/pair each40s stream. Short launch commands first start
BTstack onCPU1, then allow unpinned restart attempts if needed; each of the
four completed runs initialized on its first attempt. Exact commands retained.

python3 -u bt-latency/optimization_matrix.py bt-latency/logs/compiler-optimization/short-runs Os O2 Os O2

|Run|Host bytes|Seconds|Host KiB/s|HCI RX rejection delta|
|---|---:|---:|---:|---:|
|O2-1|7240860|40.000173|176.778043|0|
|O2-3|7250760|40.000183|177.019696|0|
|Os-0|6983460|40.000189|170.493823|0|
|Os-2|7279470|40.000167|177.720694|0|
All completed transfers have zero gaps/duplicates/bad payloads/partial frames
and zero HCI RX/TX rejection/drop increments. O2 gives176.78/177.02, versus
170.49/177.72Os. Results overlap; no material speed win established.
Keep Os default. CPU and memory snapshots retained with their wider timing
boundary; packet rates use only the40s host-fd receive window.

Failed setups preserved:
runs/setup stalled mid-command before loading radio.
quiet-runs completed setup/hash verification but timed out in the long compound
BTstack-start command. No throughput result from either failed setup.
Paced writes and quiet shell preparation were separately validated/committed.
Final harness splits launch, delay, log read, and affinity into short commands.
No claim that the underlying startup/console failure cause is fully identified.

Linux still advertises BLE at100ms during Classic, unlike native Classic-only.
This remains a mode difference to control after retrying CPU0 placement with
the reliable short-command startup. Native target remains~214KiB/s.
