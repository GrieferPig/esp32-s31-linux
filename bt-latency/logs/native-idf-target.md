# Updated throughput target requested by the user
After the current Linux measurement turn, benchmark the native ESP-IDF BLE
and Classic SPP APIs on the same ESP32-S31 board and BasedSurface peer.
Use their measured optimal sustained speeds as the Linux throughput targets,
per the user's 2026-10-06 instruction. Keep the existing LE 1M peer.
The earlier fixed 100 KiB/s objective is superseded by this native baseline.

Native experiments must preserve the Linux persist region [0x1ee000,0x400000).
Identify the board and inspect complete native image partition maps before
flashing; never use chip erase or an image overlapping persist. Use complete
verified image sets and restore the complete six-slot Linux set afterward.
Retain exact commands, native API/config selections, raw receiver data,
duration/sequence/pattern checks, and distinguish end-to-end from queued bytes.
Measure without packet logging for the performance reference, with separate
diagnostic captures as needed. Match RF peer, payloads, MTUs/PHY and pairing
conditions across native/Linux comparisons or explicitly identify differences.

Current Linux bank:
- BLE host-fd 27.41 KiB/s, 40 s, 2228 notifications, no seq gaps/duplicates,
  packet logging enabled. AcquireNotify-only fixes the receive harness.
- Classic host-fd 42.125308 KiB/s logged; 88.697444 KiB/s without logging,
  40 s each; every complete frame sequence and pattern validated.
Before native experiments, board: unchanged image 94fb9aabbbf452d9; PID 340, PAIRABLE=1,
COEX=0, packet logging disabled, /tmp/spp-nolog.log. Persist untouched.
Potential scheduler busy-poll on pending HCI TX was inspected but NOT changed;
native benchmarks take precedence before attributing remaining loss to it.

Native baselines completed: BLE31.795451 KiB/s (15ms,495B); Classic214.386362 KiB/s (990B), each repeated for40s with zero payload/sequence errors. See native-idf/report.md and summary.json. These replace the fixed100KiB/s target.
