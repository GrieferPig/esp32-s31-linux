# Diagnostics addendum (BEFORE setup, no code change, no flash, no persist wipe)

All runs used board BTstack manually started (`-l none`, pid 857 then 914;
`-l /tmp/btstack.pklg` pid 888 only transiently for capture). TLV at
`/var/lib/btstack/` preserved (never deleted/reset). `/tmp/*.pklg|*.log` are
volatile. UART used at 115200 (working shell); 2000000 baud from the mission
text was not used (console cmdline is 115200n8).

## New observations since before-measurements.md
- LE re-connect after BTstack restart: success again (no service-list spam;
  cached GATT DB). GATT `read` on 0xff11 stalls on all 5 attempts (4x `none`
  + 1x packet-log session). Board `/tmp/btstack2.log` shows AVRCP/AVDTP
  signaling but no ATT/SMP lines for the reads.
- Classic A2DP control plane works without pairing: host `bluetoothctl`
  shows `Endpoint …/sep1` + `Transport …/fd0`, board log shows
  `AVRCP Controller: PLAYBACK_STATUS_CHANGED… TRACK_CHANGED…`, host `info`
  resolves Classic UUIDs (Audio Sink 0x110b, AVRCP Target 0x110c, AVRCP
  0x110e, PnP 0x1200) alongside BLE UUIDs. SDP over L2CAP-Classic therefore
  completes; LE-ATT reads do not. Pattern: HCI + L2CAP-Classic signaling OK,
  LE data-path stall.
- `/tmp/btstack.pklg` (5914 B, PacketLogger-ish + log strings, e.g.
  `le_device_db_tlv.c:164: btstack_tlv not initialized`, Classic 0x0c1a…,
  BlueKitchen SDP records) captured startup + SDP + A2DP/AVRCP; hexdump grep
  for ATT `0a 07 00` shows no read req/resp — requests appear not to reach
  BTstack (or log does not include LE-ACL after restart; needs full btsnoop
  parse with root btmon, unavailable unprivileged).
- `/tmp/local-opt-bt-meta.log` (82503 B, earlier boot) has BT diagnostics:
  `BT_SYSCALL read calls=32 bytes=675 total_ns=5717125 max_ns=421563`
  (avg ~178 us, not the bottleneck), `BT_HCI_ERROR opcode=0x2074
  status=0x11 count=1` (expected extended-scan reject; kernel quirk
  `HCI_QUIRK_BROKEN_EXT_SCAN` in hci_esp32s31.c:410 handles fallback),
  `BT_COUNTS classic/ble_tx/rx=0` (signaling-only session).
- `radio_health`: `worker_passes=0`, dmesg `btdm native task service; no
  s31-radio thread`. This Oct-4 image (#38) runs BT without the kthread this
  tree creates at smode.c:3163 (string mismatch: tree says `SoftMAC native…`
  at smode.c:3159). Board source ≠ pinned linux-esp32-s31 52dc6ea — identify
  the Oct-4 commit before AFTER comparisons.
- Host limits (unprivileged): `l2ping`/`btmon`/debugfs `conn_*` all
  `Operation not permitted`; `sudo -n` needs interactive auth. Only
  `bluetoothctl` (bluetoothd) + `hcitool scan/inq` (connectionless) work.
  No PipeWire/pactl on host, so no A2DP media pump for bulk throughput yet.
- `make fetch` (toolchain + buildroot) started in background after the
  audit commit; toolchain dir exists, job still running at this checkpoint.

## What this means for STEP 4 (profile first)
MTU negotiated 517 and supervision holds (connection stays up minutes), so
neither explains a >55 s ATT stall. Prime suspects now, in order:
(1) LE-ACL wakeup/delivery on the BT-native path (`worker_passes=0` image) —
verify with `s31_linux_timer_report`, VHCI `-1` counts, and a full ATT
SMP trace with packet log + root btmon; (2) BTstack ATT server handling of
BlueZ's read (static DB via `att_db_util`, `att_server_init(...,NULL,NULL)`
in 0013 patch); (3) LE connection parameters/data-length (needs debugfs or
controller log). Connection-interval/MTU/aggregation tuning (softmac incs are
Wi-Fi-only; BT knobs are BTstack config + controller defaults in 0010/0014/
0015/0017) comes AFTER the stall is traced, not before.
