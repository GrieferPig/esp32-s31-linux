# STEP 4 — profile-first findings on BT suspects (no guessing; no code changed)

Measured baseline (bt-latency/logs/before-measurements.md +
diagnostics-addendum.md): HCI + L2CAP-Classic signaling OK (inquiry,
LE connect 10.1 s, SDP resolves Audio Sink/AVRCP, A2DP/AVRCP signaling flows,
`acl_packet_type` 0xcc18→0xc30e via 0014 EDR negotiation); LE-ATT GATT reads
stall (>55 s, 0/5); Classic pair `AuthenticationFailed` (9.9 s); SDP browse
stalls standalone. Syscall cost is fine (meta log: read avg ~178 us, max
421 us). MTU 517 negotiated, supervision holds (link stays up minutes).

## 1. Connection interval / supervision timeout
- Where set: S31 is LE peripheral (advertising 100 ms, 0013 patch:
  `gap_advertisements_set_params(0x00a0, 0x00a0, …)`). Central (BlueZ host)
  picks the interval in CONNECT_IND; peripheral may request update via L2CAP.
  BTstack tracks `le_connection_interval` (src/hci.c:3639,3769,3887) and range
  (`gap_get/set_connection_parameter_range`, hci.c:343-370). The demo never
  calls `gap_set_connection_parameter_range`, so BTstack defaults apply —
  read them from `hci_stack->le_connection_parameter_range` init, NOT from
  guesses. Controller minimum comes from
  `hci_le_read_minimum_supported_connection_interval` (hci.c:2492-2950).
- Profile hook: packet log must show LE Connection Complete `conn_interval`
  (hci.c:3639) and any Connection Update Complete (3887). Interval explains
  ms-level latency (e.g. 30 ms vs 7.5 ms), NOT a >55 s stall. Do not retune
  intervals to fix the stall; measure `conn_interval` first, then decide if a
  peripheral update request (new code) is warranted for latency only.

## 2. MTU / packet aggregation
- ATT MTU 517 already negotiated (`attribute-info: MTU 0x0205`). L2CAP ERTM
  MTU 512 in demo (line 178) is Classic signaling, not ATT. Host ACL
  `HCI_HOST_ACL_PACKET_LEN 1691` × 4 (s31_btstack_config.h:18-20) matches
  controller `hci_read_buffer_size` (hci.c:2142,3017) — verify the controller
  reply in the packet log before changing; if the controller reports fewer
 /smaller buffers than 1691×4, fix the config to match, not the reverse.
  MTU/aggregation explain throughput ceilings, NOT the read stall (5-byte
  value fits any MTU).

## 3. TX queueing / aggregation incs
- `s31_softmac_tx_agg.inc` / `s31_softmac_tx_reports.inc` are Wi-Fi-only
  (TID0/STA/HT20/HE-SU20; audit item 36). No BLE/Classic knobs live there.
- BT TX path: kernel HCI TX ring (`esp32s31-radio-smode.c:2079-2114` send,
  `1623-1670` `s31_radio_hci_process_tx` single `s31_radio_vhci_try_send`
  attempt, `-1` = controller busy) + BTstack backpressure retry on POLLOUT
  (0016) + host-completed credit batching at 4
  (`S31_HCI_HOST_COMPLETED_BATCH`, 0017, s31_btstack_config.h:16).
- Profile hooks: count `-1` returns at `s31_radio_vhci_try_send` under load
  (worker-slow vs controller-busy); check `radio_health`
  `hci_host_completed_commands/packets` vs `hci_submit_errors`; confirm
  credit batching cannot deadlock a single-outstanding LE response (pending
  stays 1 < 4 while BlueZ waits — verify `host_completed_packets_pending`
  flush on idle/disconnect, not just on reaching 4).

## 4. Coexistence arbitration (`s31_radio_coex_worker_tick`)
- `firmware/radio/radio_stack.c:239-241` is an empty stub: zero periodic work
  today. Arbitration, if any, is in closed `libcoexist` callbacks
  (`s31_radio_coex_bt_phase_trace`, `s31_radio_coex_wifi_phase_direct`,
  radio_stack.c:376-414) + BTstack-coex VSC 0xfc82 mirroring (0009) + scan
  duty defaults (0010, `0x0800/0x0012` page, `0x1000/0x0012` inquiry) +
  `S31_BTSTACK_PAIRABLE` toggle (0012). Board runs BT-only (`mode=bt`,
  `S31_BTSTACK_COEX=0`), so Wi-Fi coexistence is not in the stall path.
  Do not add work to the stub without profiling; combo-mode phase-timing
  (10 ms cadence requirement, smode.c:2888-2891) is a separate latency item
  for AFTER Wi-Fi checks.

## 5. BTstack port (buildroot-external/package/btstack-s31)
- Transport: direct-HCI char device (`S31_HCI_DEVICE`, 0004), no BlueZ headers
  (0004a), batch RX decode 8/frame-syscall (0008) + runloop drain 8/wakeup
  (0003), retry on backpressure via POLLOUT (0016). TLV persisted at
  `/var/lib/btstack/` (0001, preserved — never wiped), service log
  line-buffered (0006), transport-only SBC (0002/0005, bitpool 53).
- GATT: minimal peripheral (0013) — GAP/GATT + 0xff10/0xff11 read-only
  via `att_db_util` + `att_server_init(...,NULL,NULL)`. Static reads should
  not need user callbacks; the stall (requests apparently not reaching
  BTstack per packet-log grep) points at LE-ACL wakeup/delivery on the
  BT-native path (`worker_passes=0` image) or ATT-server dispatch, NOT at
  GATT DB content. Next trace: full ATT SMP bytes around the BlueZ `read`
  with packet log + root `btmon` (unavailable unprivileged — needs
  CAP_NET_RAW).
- Pairing: board SM `NoInputNoOutput/auth 0` (0013) + `Accepting Pairing -
  TODO: require actual user action` (observed in btstack2.log). Host default
  agent pairing fails `AuthenticationFailed`; LE connect without pairing
  succeeds. Whether stale TLV keys vs SM-vs-agent mismatch needs the SMP
  trace — do not clear pairings (persist) blindly.

## Measure-first checklist for the fix (in order)
1. Packet-log LE Connection Complete `conn_interval` + buffer-size replies.
2. `s31_linux_timer_report` lateness + VHCI `-1` counts under ATT load.
3. Credit-batch flush behavior for single-outstanding LE responses (0017).
4. SMP trace of the `AuthenticationFailed` pairing vs unauthenticated LE
   connect path.
Only then retune: peripheral conn-update request, buffer-size match,
credit-batch flush, or ATT dispatch — each with before/after same-setup
numbers. No change made in this step.
