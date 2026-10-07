# Classic controller TX buffer depth
Dist14a660a379ec0386, all six Linux slots flashed and digest-verified.
Persist excluded. Build used the established PATH/ESP_TOOLS/S31_ALLOW_UNPINNED
make build image command; raw build/flash and argv retained.
Kernel and BTstack patches applied cleanly to scratch copies and matched source.

New BT-only init parameter bt_tx_buffers:0 keeps SDK default;3..10 overrides
static Classic TX buffers, dynamic buffers0. Wi-Fi/combo does not override.
Global feature flags and shutdown argument remain unchanged.
Patch0036 reports HCI Read Buffer Size status, ACL size and count.
Each tested boot confirmed status0,1021-byte buffers and the requested4 or10.

Command:
python3 -u bt-latency/buffer_depth_matrix.py bt-latency/logs/tx-buffer-depth/runs 4 10 4 10

Clean reboot before each run. Same image, host adapter, fresh agent/pair,
CPU1, gate_sleep1, timer40ms, SPP990, local callbacks0, coalescing0,
COEX0, PAIRABLE1, no packet log, profiler enabled.

|Run|Host bytes|Seconds|Host KiB/s|HCI RX rejection increment|
|---|---:|---:|---:|---:|
|buffers10-1|7319070|40.000198|178.687349|130|
|buffers10-3|7205220|40.000154|175.908014|156|
|buffers4-0|5222250|40.000109|127.495992|0|
|buffers4-2|5102460|40.000180|124.571217|0|
All host40-second streams had zero gaps, duplicates, bad payloads or partial
frames; HCI TX drop increment0. Ten buffers improve delivery to175.91–178.69
versus124.57–127.50KiB/s. They remain below native~214KiB/s.

Ten-buffer runs are not final acceptance: hci_rx_dropped rose130/156.
This counter increments when the Linux8-slot ring (7 usable) rejects a VHCI
callback with-ENOSPC. We have not proved whether the closed controller retries
every rejected event; clean host payloads do not establish lossless HCI.
Next test varies Linux RX/TX ring depths, preserving existing defaults and
Wi-Fi/combo sizes. BLE parity remains banked on the earlier tested image.
