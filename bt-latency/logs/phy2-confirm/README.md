# PHY mystery resolved on real peer (2026-10-06)
Unchanged flashed image 94fb9aabbbf452d9; BURST=24, PAIRABLE=0, COEX=0,
PHY=2 requested, DLE enabled. Host power cycle + fresh pairing succeeded.
No code patch, rebuild, reflash, or persist partition operation.

## Complete capture
1236406 bytes, 3515 records, 0 sequential-record parse errors, 0 incomplete ACL.
The direct pump send is visible at 2877.030111:
CMD 0x2032, payload 00000002020000 (handle 0, all_phys 0, TX/RX 2M).
All three sends and PHY Update Complete events:
2869.504983 -> 2869.585289
2870.466999 -> 2870.530076
2877.030111 -> 2877.082105
Each update is raw 3e060c1a00000101: status 0x1a, handle 0, TX 1M, RX 1M.
Command Status responses acknowledge Set PHY successfully; subsequent
update fails with UNSUPPORTED_REMOTE_FEATURE_UNSUPPORTED_LMP_FEATURE.
Host hciconfig explicitly reports Marvell HCI/LMP 4.2 (0x8), USB 1286:204c.
This peer cannot negotiate LE 2M. Requesting 2M is not a measured 2M run.
DLE 0x2022 at 2877.088273 is accepted separately.
PacketLogger is a local HCI trace, not an over-the-air sniffer. Received
controller completions establish command processing; no vanished opcode
is reproduced. The older incomplete /tmp/blephy.pklg also contains 0x2032,
but this conclusion relies on the new complete capture.

## Throughput
2292 notifications x 507 ATT bytes = 1162044 bytes in 40.046010 s:
28.337619 KiB/s pump-to-controller, actual PHY still 1M.
10 s windows: 29.756543 / 28.122656 / 27.627539 / 27.776074 KiB/s.
Fresh burst-8: 23.355256 KiB/s / 40.066848 s, same image and adapter reset/
fresh-pair procedure, but no 2M request. Single trials; do not infer a
precise causal speedup. All AcquireNotify-path attempts remain InProgress;
host-fd/end-to-end BLE throughput is UNMEASURED.

## Decision and NEXT
The suspected missing-opcode failure is disproved for this setup. Additional
opcode TX instrumentation and a reflash would not remove a peer capability
limit, so that requested diagnostic rebuild/reflash was deferred.
1. A 2M-capable host adapter is required to measure 2M on this link.
2. BLE host-fd acquisition still fails; a reliable receive path is needed for
   end-to-end claims. Existing harness StartNotify/AcquireNotify sequencing
   is also not independently isolated from BlueZ discovery contention.
3. Neither BLE nor Classic has reached 100 KiB/s. Classic was not rerun here;
   its banked ~32 KiB/s end-to-end result remains historical. Its scheduling/
   credit bottleneck has not been identified by these BLE-only experiments.
4. Known per-connection HCI task wedge was not changed.

## Handoff state
Restored BURST=24, PHY default 1M, PAIRABLE=0, COEX=0, DLE enabled.
PID 318, log /tmp/mission-final.log, pklg /tmp/mission-final.pklg.
Final host LE scan receives S31 Radio at -43/-44 dBm.
Image unchanged. No flash, partition writes, or Wi-Fi reconfiguration.
wlan0 absent on entry and exit; Wi-Fi was not validated in this session.
Next connection after this BTstack restart should use fresh host pairing
(and adapter power cycle if needed); persisted board bond storage remains
out of scope. Untracked BT_DEBUG_MISSION.md was preserved.
