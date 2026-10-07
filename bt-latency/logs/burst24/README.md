# Burst-24 isolation and advertisement recovery (2026-10-06)
Board image 94fb9aabbbf452d9, unchanged. Branch bt-latency-fix.
PID 245: PAIRABLE=0, BURST=24, PHY default 1M, DLE enabled.
No flash or partition writes. Initial wlan0 absent; no Wi-Fi reconfiguration.

## Advertisement blocker
Initial full 2421-byte capture parses with zero errors. ADV parameters/data/
scan-response/enable commands 0x2006/2008/2009/200a match last working capture;
enable completes status 0. Classic scan switches 3 -> 0 as expected for
PAIRABLE=0. These are host/controller observations, not RF sniffing.
A fresh 22-second LE scan receives 30:ED:A0:F3:D4:AE at -43 to -45 dBm.
No board restart or adapter power cycle was required. Original disappearance
not reproduced; do not claim its historical cause is established.
Pair failure reproduced as org.bluez.Error.AlreadyExists, with Paired=yes,
Bonded=yes independently confirmed. run-bonded.py accepts this specific result.

## Real link
Bond reuse: 0.0 s; connect: 0.6 s; ff12 characteristic found immediately.
All four subscription attempts: InProgress. CCCD nevertheless starts stream.
Capture: 1,156,264 bytes, 3244 records, 0 parse errors, 0 incomplete ACL.
LE interval 36 units = 45 ms. Direct DLE 0x2022 accepted status 0.
2146 notifications, sequence 0..2145, 507 ATT bytes each (504 value bytes).
Timestamp span 38.888874 s; not a full 40-second observed stream.
Three complete 10-second windows: 26.983887 / 27.578027 / 27.726562 KiB/s.
Combined first 30 s: 1662 notifications, 842634 ATT bytes,
27.429492 KiB/s pump-to-controller (27.267188 KiB/s values).
Banked burst-8 19.26 KiB/s used a different 40.1 s window; a fresh same-image
burst-8 repeat is recorded separately before drawing a causal conclusion.
End-to-end BLE is UNMEASURED. These rates are not host-fd delivery rates.

See commands.txt, run-bonded.txt, capture.pklg, analysis.json and original
scan/pair transcripts. Full UART chunk output is retained locally alongside
capture.pklg; four chunks reassembled to the exact board file size.
