# BLE DLE breakthrough (Stages C/D) — 0x2022 on air, accepted

Board image: dist `a674c1fe2e6dd5cd` (patches 0023–0031, kernel HZ=1000).
Full-slot reflash 6/6 verified. BTstack `S31_BTSTACK_PAIRABLE=0`
(LE-forced), `S31_BTSTACK_DLE` default on, `S31_BTSTACK_PHY` default 1M.
Host fully cleaned (remove + adapter power cycle — clears the kernel's
persisted short-interval params, link back at 45 ms).

## The stuck-task diagnosis (confirmed)

- `gap_le_set_data_length()` returns 0 (pending bit set) at both
  `@connect` and `@mtu`, but 0x2022 never reaches the air; L2CAP
  conn-update (0x12) on the same handle works fine.
- Direct `hci_send_cmd` from inside the HCI event handler also never
  emits: `hci_can_send_command_packet_now()` is false there because the
  shared packet buffer is reserved during event dispatch
  (`S31 BLE DLE: pipeline busy, task path only`).
- Pump-context direct send works: `S31 BLE DLE: direct 0x2022 sent
  from pump` (patch 0031). The per-connection HCI task path is
  effectively wedged on this port (pending set, never scheduled);
  L2CAP/ATT/ACL are unaffected (separate paths).

## Air proof (board pklg `/tmp/ble3.pklg`, 843214 B, 0 parse errors)

- `335.9 CMD> 0x2022 060000fb004808` (251 octets / 2120 us)
- `336.0 COMPLETE 0x2022 status=0x00` — controller ACCEPTED.
- No DATA-LENGTH-CHANGE subevent observed (controller never emits it;
  lengths accepted anyway).
- `334.1 CONNECT iv=36` (45.0 ms — relearned after host clean).
- Link lived 334 s → 632 s (298 s), then `DISCONNECT reason=0x08`
  (supervision timeout, usual RF hole).

## Streaming numbers (pump→controller, same setup)

- 1558 notifies × 507 B = 789,906 B over 40.1 s = **19.26 KiB/s avg**.
- 10 s windows post-DLE: 23.0 / 15.0 / 13.6 / 25.0 KiB/s (controller +
  BlueZ ATT pacing fluctuates; pre-DLE queue burst 72 KiB/s is
  pump→controller queueing, not air).
- End-to-end (host fd) still unmeasured: BlueZ `AcquireNotify` stays
  `InProgress` (its ATT discovery never idles on this link).

## Next knobs (no rebuild needed for PHY)

- `S31_BTSTACK_PHY=2`: isolates air-vs-pump bottleneck.
- Deeper `S31_BLE_STREAM_BURST` (now 8): pump pacing is CAN_SEND-gated;
  air-time per 507 B notify fell ~6x with DLE, so burst depth should now
  dominate. Requires patch + rebuild.
