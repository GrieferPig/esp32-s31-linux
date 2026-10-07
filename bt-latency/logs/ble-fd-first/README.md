# First measured BLE host-fd stream (2026-10-06)
Unchanged image 94fb9aabbbf452d9, burst 24, PHY default 1M, DLE enabled,
PAIRABLE=0, COEX=0. Fresh host adapter power cycle, scan, pairing.
Command:
python3 -u bt-latency/fresh_fd_run.py bt-latency/logs/ble-fd-first

Root cause of harness InProgress: it called StartNotify before AcquireNotify.
Calling AcquireNotify alone acquired fd immediately, MTU=517.
This falsifies the earlier claim that BlueZ discovery necessarily never idles.
Independent board trace shows CCCD Write Response 0x13 before notification
streaming. No board patch was needed; starting the pump in the write handler
was not the cause in the inspected trace.

HOST RECEIVED: 1122912 bytes, 2228 notifications, sequences 0..2227,
0 sequence gaps, 0 duplicates, 40.0 s, 27.41 KiB/s application values.
This is end-to-end board-to-host BLE throughput, unlike prior pump metrics.
Payload pattern was not checked in this first run.
Board capture: 1207164 bytes, 3424 records, 0 parse errors, 0 incomplete ACL;
2238 notifies, seq 0..2237. The final 10 queued notifications are beyond
the host's fixed receive window; not a within-window sequence loss claim.

Capture retrieval:
kill -STOP 318
gzip -c /tmp/mission-final.pklg >/tmp/fd-first.pklg.gz
python3 bt-latency/pull_pklg_chunk.py /tmp/fd-first.pklg.gz bt-latency/logs/ble-fd-first/capture.pklg.gz 0 82
gzip decompression validates CRC; analyze_pklg.py validates every record.
The canonical ble_bulk_run.py now uses AcquireNotify alone and select-bounded
nonblocking reads. ble_fd_bench.py is an alias. Board image/persist unchanged.
