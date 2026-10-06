# Classic SPP bulk throughput (Stage E) — first real numbers

Board image: dist `7c41df4cdf4dc457` (patches 0023–0028, kernel HZ=1000,
`ENABLE_LE_DATA_LENGTH_EXTENSION`). Full-slot reflash, 6/6 hashes verified
at 921600 baud. BTstack restarted once (known first-start HCI-init stall),
Classic pairability enabled (default; no `S31_BTSTACK_PAIRABLE` override).

Host: unprivileged `AF_BLUETOOTH` RFCOMM socket (`bt-latency/spp_bulk_bench.py`).
Board: SPP streamer RFCOMM ch 1 "S31 SPP Stream" (patch 0028), 1012 B frames
(MTU 1016), u32-LE seq prefix, burst 8 on CAN_SEND_NOW. EDR five-slot mask
`0xc30e` accepted by controller (`S31 BR/EDR packet type: handle=0x0801
status=0x00 type=0xc30e`). Board tx total 697268 B vs host rx 692208 B in
run 1 (5 KB in flight at close — clean, no loss).

## Runs (all real host-to-board, same setup)

- Run 1: `python3 bt-latency/spp_bulk_bench.py 30:ED:A0:F3:D4:AE 1 25`
  `SPP bytes=692208 recvs=682 seq_first=0 seq_last=683 dur=25.0s
  rate=27676 B/s (27.03 KiB/s)` — connect 0.4 s after `pair` BONDED 2.5 s.
- Run 2 (repeat, same link keys): `... 1 25`
  `SPP bytes=102212 recvs=100 seq 0->100 dur=6.8s rate=15068 B/s (14.72 KiB/s)`
  — stream stalled at 6.8 s (recv timeout); next connect `Host is down`,
  board console dead, required hard reset. Sustained-bulk wedge under
  investigation (see below).
- Post-reset, fresh `pair` BONDED 5.1 s (needed host adapter power-cycle;
  host-side stuck, not board):
- Run 3: `... 1 15`
  `SPP bytes=516120 recvs=507 seq 0->509 dur=15.0s rate=34383 B/s (33.58 KiB/s)`
- Run 4: `... 1 25`
  `SPP bytes=821744 recvs=811 seq 0->811 dur=25.0s rate=32850 B/s (32.08 KiB/s)`

Typical stable rate: **~32 KiB/s** board->host, zero seq gaps, full windows.

## vs target

100 KiB/s target: 3x away on Classic. Path is EDR DH5-capable (mask
accepted); current pacing is pump burst 8 + CAN_SEND round trips through
the compat VHCI path. Knobs queued: deeper burst, credit batching,
host-RX pacing check, air-packet (DH5 vs DH3/DM5) verification from pklg.

## Open issue (new)

One sustained-bulk wedge: after run 2 stalled, board console went silent
(no UART output) and Classic paging failed until hard reset. Not yet root
caused — candidate: TX-ring/backpressure deadlock under continuous 5-slot
load, or unrelated console flake (reopen flakiness is known). Repro runs
will use 15 s windows with console checks between runs.
