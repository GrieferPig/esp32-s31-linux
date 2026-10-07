# Local callback dispatch: not adopted for throughput
Patch0035 optionally processes queued main-thread callbacks before select,
without a same-thread pipe write. It preserves asynchronous completion and
foreign-thread pipe wakes. Default remains disabled.
Scratch patch application exactly matched the built source.
80 host correctness runs tested recursive512 and mixed512+2000 callbacks,
with local/coalesced modes in all combinations; no lost/duplicate callbacks.
Pure recursive chain wake writes512legacy vs1local (the initial pre-loop wake).
This is host correctness evidence, not proof of board performance.

Dist e339bc60558acca2: full six-slot flash and six explicit digest matches.
Persist excluded. Build command uses the established PATH/ESP_TOOLS/
S31_ALLOW_UNPINNED=1 make build image environment; raw build/flash retained.
First test launch exceeded BusyBox's1024-byte command limit and was truncated
inside a quote. No benchmark ran. Ctrl-C cleared the incomplete command.
The harness now uses a shorter shell function, and board_command rejects
oversized commands before sending any bytes. Failed launch evidence retained.

Same image, CPU1, sleeping gate1, timer40ms, legacy coalescing0, SPP990,
COEX0, PAIRABLE1, packet log off, profiling enabled. Mode0=legacy,1=local.
python3 -u bt-latency/local_callback_matrix.py spp bt-latency/logs/local-callbacks/runs-retry 0 1 0 1

|Run|Host bytes|Seconds|KiB/s|
|---|---:|---:|---:|
|spp-mode0-0|5212350|40.000344|127.253544|
|spp-mode0-2|5029200|40.000095|122.782913|
|spp-mode1-1|4777740|40.000239|116.643347|
|spp-mode1-3|4793580|40.000385|117.029636|
All40-second transfers passed: zero sequence/pattern errors, duplicates,
or HCI drop increments. Local dispatch reduced BTstack CPU but reduced rate
to116.64–117.03KiB/s vs122.78–127.25 legacy. Keep it disabled for throughput.
This rules out a simple assumption that fewer callback syscalls alone would
recover native~214KiB/s. Next control increases controller TX buffer depth.
BLE parity remains banked on distc252d979f699ebb8,31.65/31.67KiB/s.
