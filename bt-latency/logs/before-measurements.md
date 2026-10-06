# BEFORE measurements — real host-to-board runs, 2026-10-06 (no simulation)

Board: ESP32-S31, Linux 6.18.0 #38 SMP PREEMPT Oct 4 23:41 PDT (grieferpig@BasedLaptop),
cmdline includes `s31.radio_autoload=0` (recovery; BTstack init-script skipped).
Radio mode `bt` (BT-only, direct_hci=1). BTstack started manually:
  `S31_HCI_DEVICE=/dev/s31-hci S31_BTSTACK_COEX=0 S31_BTSTACK_NAME="S31 Radio"`
  `/usr/sbin/s31-btstack-a2dp -u 0 -l none >/tmp/btstack.log 2>&1 &`
Board BT: BD 30:ED:A0:F3:D4:AE, "S31 Radio", BLE advertising 100 ms
(0x00a0), service 0xff10/char 0xff11 ("ready", 5 B, read-only, ATT_SECURITY_NONE),
Classic pairable enabled, scan cmds 0x0c1a/0x0c1c/0x0c1e status=0.
UART: /dev/ttyUSB0 at 115200 baud (NOT 2000000; mission text says 2M but
Linux console cmdline is `console=ttyS0,115200n8`, HIL default 115200, and
115200 yields a working `~ #` shell; 2M was not used).
Host: BasedSurface, hci0 Marvell F0:6E:0B:D8:CE:45, BlueZ 5.83 (bluetoothd),
unprivileged (no sudo/root; `sudo -n` requires interactive auth, raw HCI/L2CAP
tools fail with Operation not permitted). All host commands below ran as
`grieferpig` without root unless noted.

## 1. Classic discovery (BEFORE, pass)
- Cmd: `timeout 25 hcitool -i hci0 scan` (also `hcitool -i hci0 inq`)
- Raw: `Scanning ... 30:ED:A0:F3:D4:AE S31 Radio` (2/2 runs);
  `inq` adds `clock offset: 0x685b class: 0x240400`.
- Timing: `/usr/bin/time` inquiry run: `inquiry elapsed 11.52s` (standard
  ~10 s inquiry window + processing). Repro: repeat the same `hcitool scan`.

## 2. BLE discovery (BEFORE, pass)
- Cmd: `timeout 30 bluetoothctl --timeout 25 scan on`
- Raw: `[NEW] Device 30:ED:A0:F3:D4:AE S31 Radio` within the window; later
  `devices` lists it; `info` shows `RSSI: 0xffffffd9 (-39)` then `(-38)`,
  `UUID: 00001800…`, `UUID: 0000ff10…`, `AdvertisingFlags: 06`.
- Repro: same `bluetoothctl scan on`; S31 appears among ~15 nearby LE devices.

## 3. LE connect, no pairing (BEFORE, pass, slow)
- Cmd (with background `bluetoothctl scan on` to keep cache fresh):
  `timeout 40 bluetoothctl --timeout 35 connect 30:ED:A0:F3:D4:AE`
- Raw: `Attempting to connect… [CHG] Connected: yes / Connection successful`,
  then Primary Services 0x1800/0x1801/0xff10, Characteristics 0x2a00
  (Device Name) and 0xff11, `ServicesResolved: yes`. `info` after:
  `Connected: yes, Paired: no`.
- Timing: `real 0m10.127s` for connect + service resolution.
- Repro: scan 6 s, then `connect` as above. Pairing is NOT required for this.

## 4. BLE GATT read, char 0xff11 ("ready") (BEFORE, FAIL — stall)
- Cmd: `bluetoothctl` → `menu gatt` → `list-attributes` (shows service0005
  0xff10, char0006 0xff11) → `select-attribute 0000ff11-…` →
  `attribute-info` (`Flags: read, MTU: 0x0205 (517)`) → `read`.
- Raw: each `read` prints only
  `Attempting to read /org/bluez/hci0/dev_30_ED_A0_F3_D4_AE/service0005/char0006`
  with NO value/error before timeout. Tried 3x reads in one session + 1x
  single-read session with `--timeout 55`: all stall.
- Numbers: latency >55 s (timeout), bulk throughput 0 B/s (0/4 reads
  completed). Board `/tmp/btstack.log` shows no new lines for these reads
  (only startup lines). Board `radio_health` HCI ACL counters static
  (acl_rx 116893→116906 over the session; no per-read increment).
- Repro: connect as in §3, then the `menu gatt` sequence above.

## 5. Classic pair via BlueZ (BEFORE, FAIL)
- Cmd (scan active): `timeout 40 bluetoothctl --timeout 35 pair 30:ED:A0:F3:D4:AE`
- Raw: `Attempting to pair… [CHG] Connected: yes` then
  `Failed to pair: org.bluez.Error.AuthenticationFailed`,
  `[SIGNAL] Disconnected … Reason.Local Connection terminated by local host`,
  `[CHG] Connected: no`. `info` after: `Paired: no, Connected: no`.
- Timing: `real 0m9.926s` to failure.
- Note: board SM is `NoInputNoOutput/auth-req 0` (0013 patch); host agent
  default. Whether the failure is board-SM vs host-agent vs stale TLV
  (`/var/lib/btstack/btstack_30-ED-A0-F3-D4-AE.tlv` exists) is UNRESOLVED —
  needs packet log (`-l` file, not `none`) + SMP trace, not guessing.
- Repro: scan 6 s, then `pair` as above.

## 6. Classic SDP browse (BEFORE, FAIL — stall)
- Cmd: `timeout 20 sdptool browse 30:ED:A0:F3:D4:AE`
- Raw: no output before timeout (empty). Consistent with L2CAP data-path
  stall (§4). (Exit captured via head pipe; no SDP records returned.)
- Repro: same `sdptool browse` while BTstack running.

## 7. Classic L2CAP ping/throughput (BEFORE, BLOCKED — privilege)
- Cmd: `timeout 10 l2ping -i hci0 -c 3 30:ED:A0:F3:D4:AE`
- Raw: `Can't create socket: Operation not permitted`. Same for
  `hciconfig hci0 up`, `hcitool lescan`, `hcitool info` (ACL create:
  `Can't create connection: Operation not permitted`). No sudo (requires
  interactive auth), no CAP_NET_RAW. No Classic RTT/throughput numbers
  obtainable unprivileged; BlueZ `bluetoothctl` path (§3–§5) is the only
  usable channel.
- Repro: run `l2ping` as non-root.

## 8. Wi-Fi station + throughput (BEFORE, BLOCKED — mode/config)
- Board is `mode=bt` (no Wi-Fi frontend rings; dmesg
  `mode=bt direct_hci=1`), wifi.conf is placeholder
  (`ssid_hex=6e6f7420796f75722077696669` = "not your wifi"), and
  `s31.radio_autoload=0` recovery is active. No Wi-Fi STA connect or
  throughput attempted; doing so needs combo mode + real AP + exiting
  recovery (reboot with new cmdline or manual module reload) — all affect
  the BT setup, so deferred until after a code change with re-measurement.
- radio_health (BT-only, BEFORE): `ticks=48036486 worker_passes=0 commands=29
  irqs=240652 heap_used=95120/298992 wifi_rx_dropped=0 wifi_tx_dropped=0
  hci_rx_dropped=0 hci_tx_dropped=0 acl_rx=116906/7670414B acl_tx=23501/7192919B
  acl_type=0xcc18 coex_last_status=-61 coex_bt_status=0x00`.
  Note `worker_passes=0` + dmesg `btdm native task service; no s31-radio
  thread`: this Oct-4 image already runs BT without the "s31-radio" kthread
  in this checkout's terms (string differs from this tree's
  `SoftMAC native task service` at smode.c:3159 — board image source ≠
  pinned 52dc6ea; see status.md).

## Summary table (BEFORE)
| Test | Result | Number |
|---|---|---|
| Classic inquiry discovery | pass | found, ~11.5 s window |
| BLE discovery (bluetoothctl) | pass | found, RSSI -38/-39 |
| LE connect + resolve (no pair) | pass | 10.1 s, 3 services |
| BLE GATT read 0xff11 (5 B) | FAIL (stall) | latency >55 s, 0/4 ok, 0 B/s |
| Classic pair (BlueZ) | FAIL | AuthenticationFailed, 9.9 s |
| Classic SDP browse | FAIL (stall) | no records |
| Classic l2ping/RTT/bulk | BLOCKED (perms) | no numbers |
| Wi-Fi STA/throughput | BLOCKED (mode) | no numbers |

No simulated numbers: every line above came from the runs described.
AFTER numbers require a code change + rebuild + flash + same-setup rerun
(not done in this commit; see status.md NEXT).
