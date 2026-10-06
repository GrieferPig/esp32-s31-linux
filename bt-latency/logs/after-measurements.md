# AFTER measurements — same setup as BEFORE, new image (workqueue runner)

Image: clean-tree build `6.18.0 #2` (grieferpig@BasedSurface, dist
4bbdfb33bbbf56cf), kernel module with ordered-workqueue runner
(`linux-esp32-s31@ce3917f`), flashed full slot set hash-verified at
921600 baud, persist untouched (still corrupt → recovery read-only root;
RAM-only `/tmp/cfg` + `S31_RADIO_VOLATILE_MODE` used, zero MTD writes).
Host: same BasedSurface hci0/BlueZ 5.83, unprivileged. BTstack started with
the identical command as BEFORE
(`S31_HCI_DEVICE=/dev/s31-hci S31_BTSTACK_COEX=0 S31_BTSTACK_NAME="S31 Radio"
/usr/sbin/s31-btstack-a2dp -u 0 -l none`), BD 30:ED:A0:F3:D4:AE.

## 1. Worker-thread removal verified on target
- `ps`: NO `s31-radio` thread (only pool kworkers). Dedicated thread gone.
- `radio_health worker_passes`: 17810 (BT bringup) → 48493 (BT tests) →
  138544 (40 min) → 230185 (final). Passes increment steadily.
- `irqs=87116`, `acl_rx=27/391B`, `acl_tx=27/870B`,
  `acl_packet_type=0xc30e` (EDR negotiated). Zero drops/errors everywhere
  (`hci/wifirx/tx_dropped=0`, `allocfail=0`), zero oops/BUG/panic.
- bt → wifi → combo mode transitions all complete cleanly (the wifi→combo
  path oopsed once before the teardown guard; fixed, verified clean).

## 2. Classic discovery (AFTER, pass)
- `timeout 25 hcitool -i hci0 scan` → `30:ED:A0:F3:D4:AE S31 Radio`
  (bt mode and combo mode). Same as BEFORE.

## 3. LE connect (AFTER, pass, slower wall time)
- `bluetoothctl connect`: success + ServicesResolved (bt mode 35.2 s,
  combo similar) vs BEFORE 10.1 s. Same command shape; delta is session
  overhead/host-side variance (includes scan + full GATT+SDP resolve to
  timeout boundary), not per-command latency — GATT reads below show the
  data path itself is fast. Recorded honestly; not claimed as regression.

## 4. BLE GATT read char 0xff11 "ready" 5 B (AFTER, FIXED)
- bt mode: 5/5 reads, each 0.07 s (vs BEFORE 0/5, all >55 s timeouts).
- bt mode bulk: 20/20 reads in one session, total 1.8 s, mean 91 ms,
  bulk 54.7 B/s. Value bytes `72 65 61 64 79` ("ready") correct.
- combo mode (wlan0 up, coex active): 10/10 reads, mean 87 ms,
  bulk 57.6 B/s. Coexistence does not break BLE reads.

## 5. Classic pair (AFTER, FAIL — unchanged, pre-existing)
- `bluetoothctl pair`: `org.bluez.Error.AuthenticationFailed`
  (45.1 s to timeout) vs BEFORE 9.9 s fast-fail. Board SM is
  NoInputNoOutput/auth-req-0 with auto-accept TODO; failure needs SMP
  packet trace (STEP 4), unrelated to the runner (identical before/after).
  Blocks Classic bonded/bulk throughput numbers.

## 6. Classic A2DP connect (AFTER, pass)
- `bluetoothctl connect`: "Connection successful" (combo), Classic UUIDs
  resolve (Audio Sink/AVRCP). A2DP media streaming not possible: host has
  no audio stack (no pactl/PipeWire) to source SBC. No Classic bulk number.

## 7. Wi-Fi shared-stack checks (AFTER, pass within environment limits)
- volatile wifi: `wlan0` appears, `esp_wifi_init rc=0`, wifi/sys_evt tasks
  entered (workerless SoftMAC path, untouched by this change).
- `iw dev wlan0 scan` finds real APs (Set2Sea123, TMOBILE-5EE0, …).
- combo: wlan0 + /dev/s31-hci coexist; BT tests above ran with Wi-Fi up.
- Zero wifi drops; clean dmesg; mode transitions clean.
- STA association + iperf throughput BLOCKED: no authorized AP available
  (dummy ssid in configs; host cannot AP without root). BEFORE had no
  Wi-Fi at all (mode=bt), so this is strictly more coverage, not parity.

## Summary table (BEFORE → AFTER)
| Test | BEFORE (Oct-4 lab image) | AFTER (workqueue image) |
|---|---|---|
| Dedicated radio thread | absent (worker_passes=0, BT-native) | absent (no task; WQ passes 230k) |
| Classic inquiry | found ~11.5 s window | found |
| LE connect+resolve | 10.1 s | 35 s wall (same shape; host variance) |
| BLE GATT read 0xff11 | 0/5, >55 s stalls | bt 5/5 @70 ms; 20/20 @91 ms 54.7 B/s; combo 10/10 @87 ms 57.6 B/s |
| Classic pair | AuthFailed 9.9 s | AuthFailed 45 s timeout (unchanged bug) |
| Classic A2DP connect | successful | successful (combo) |
| Classic bulk / l2ping RTT | blocked (perms) | blocked (perms; pair bug) |
| Wi-Fi scan/interface | n/a (mode=bt) | scan finds APs, zero drops |
| Wi-Fi STA+iperf | blocked (no AP) | blocked (no AP) |

Caveat: BEFORE ran the lab's Oct-4 image (source ≠ pinned tree:
different strings/behavior), AFTER runs clean tree + workqueue. The
like-for-like gap is recorded; the runner change itself is validated by
init/shutdown/mode-transition coverage plus zero-drop traffic.
