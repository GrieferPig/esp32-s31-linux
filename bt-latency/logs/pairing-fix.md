# Blocker 1 — Classic/LE `pair` AuthenticationFailed: ROOT-CAUSED AND FIXED

Note on transport: although reported as "Classic pairing", the captured
exchanges prove BlueZ drove LE SMP (L2CAP CID 0x0006) in every failing run.
BR/EDR SSP was never attempted on air. The broken path was the LE Security
Manager responder in BTstack; Classic SSP auto-accept already existed.

## Trace method (no root btmon)
- Board BTstack packet log (`-l /tmp/*.pklg`, PacketLogger framing: 13-byte
  header `[len BE32][tv_sec][tv_us][type]`, types 0x00 CMD sent / 0x01 EVT /
  0x02 ACL sent / 0x03 ACL recv / 0xfc LOG), pulled via `base64` over UART
  and decoded on host with exact framing (script in shell history; direction
  preserved by type). Direction + microsecond timestamps included.
- Host bluetoothd syslog (`/var/log/syslog`, readable via adm group).

## Failing exchange (before fix), 3 captures, identical shape
- Host `SMP Pairing Request (0x01)`: IO=0x03 NoIO, AuthReq=0x29
  (BONDING|SC|CT2), MaxKey 16, Init 0x08/0x0d, Resp 0x0a/0x0f.
- Board `SMP Pairing Response (0x02)` in ~1 ms: IO=NoIO, **AuthReq=0x00**
  (no bonding/SC), MaxKey 16, Init/Resp masked to ID-key only.
- Two distinct failure modes observed:
  (a) No BlueZ agent registered: host aborts 89 ms after the response with
      `SMP Pairing Failed (0x05/0x01)`; bluetoothd syslog:
      `new_auth() No agent available for request type 2` +
      `device_confirm_passkey: Operation not permitted`. Host-side cause.
  (b) NoInputNoOutput agent registered: host sends `SMP Confirm (0x03)`;
      board NEVER answers → 30.2 s SMP timeout → Disconnect reason 0x05
      (Authentication Failure) → BlueZ `AuthenticationFailed`.
- Board root causes (both in `buildroot-external/.../0013` BLE setup):
  1. `sm_set_authentication_requirements(0)`: no bonding offered, so no LTK
     distribution and BlueZ `pair` could never complete by construction
     (`sm_key_distribution_flags_for_auth_req` only adds ENC_KEY under
     `SM_AUTHREQ_BONDING`; response offered ID-key only).
  2. Nobody answered `SM_EVENT_JUST_WORKS_REQUEST`: BTstack dispatches SM
     events on the `sm_add_event_handler` list, but the demo registered only
     an HCI handler, so the responder parked in `SM_PH1_W4_USER_RESPONSE`
     forever (BTstack `sm.c:4859-4863`). A case in `hci_packet_handler`
     would be dead code — verified against `sm_dispatch_event`.

## Fix (0022, rebuilt + reflashed, hash-verified)
- `sm_set_authentication_requirements(SM_AUTHREQ_BONDING)` (NoIO kept, so
  method stays Just Works, no MITM).
- Dedicated `s31_sm_packet_handler` on `sm_add_event_handler` that
  auto-confirms JustWorks (NoIO appliance has no user; same policy as the
  existing Classic SSP auto-accept). Forward declarations added after a
  build break (setup_demo precedes the handler).
- Patch verified to apply after 0001–0021; binary contains the new strings;
  running board binary confirmed via `strings`.

## Re-test (same setup, fixed image dist 4b08e6cef5ce76d3)
- `pair` with NoIO agent: **BONDED in 1.6 s, 1.7 s, 1.6 s** (3/3).
  Host `info`: Paired yes, Bonded yes, Connected yes.
- Board log shows `S31 LE Just Works pairing auto-confirmed` exactly once.
- Full key exchange on air: Confirm/Random both ways, board distributes
  LTK (EncInfo) + EDIV/Rand (MasterId) + IRK (IdentInfo) + address; host
  distributes its keys; ATT service discovery then flows on the link.
- Bonded bulk throughput (encrypted GATT reads, 5 B char): **20/20,
  mean 88 ms, 56.6 B/s** (unbonded baseline was 20/20 @91 ms / 54.7 B/s:
  encryption costs nothing measurable at this size).
- Bond reuse: disconnect + reconnect with NO new pairing (auto-confirm
  count stays 1), GATT reads return "ready" on the re-encrypted link.
- Kernel: zero drops/errors, no oops; runner passes kept incrementing.

## Adjacent findings (NOT fixed here, out of scope)
- TLV key files are never written (`/var/lib/btstack/` stays empty):
  bonds live only in BTstack RAM, so a BTstack restart loses them (re-pair
  needed). The TLV init itself looks correct in main.c; root cause of the
  missing write not traced. Follow-up, not this task.
- First BTstack start after radio bringup stalls in HCI init (236 B capture:
  3× HCI_Reset + 2× Read-Version with partial completes, kernel drops 0);
  restart always completes. Startup race, separate from pairing.
- Classic BR/EDR SSP pairing was never exercised on air in any run
  (BlueZ chose LE every time); Classic A2DP connects still work unpaired
  as before. Classic PIN/SSP interop remains untested.
