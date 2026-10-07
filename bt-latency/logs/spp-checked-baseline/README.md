# Classic SPP checked baseline: PacketLogger enabled
Same board image 94fb9aabbbf452d9 and host adapter. PAIRABLE=1, COEX=0.
Host: fresh adapter power cycle, BR/EDR scan, fresh pair before RFCOMM open.
Command: python3 -u bt-latency/fresh_spp_run.py bt-latency/logs/spp-checked-baseline
Board restart command and UART output are retained in this directory.

Real host receive: 1725460 bytes in 40.000171341 seconds,
42.125307836 KiB/s. 1705 complete 1012-byte frames,
0 sequence-gap events, 0 duplicates,
0 corrupt-pattern frames, 0 trailing bytes.
Stopped at the measurement deadline. Raw host byte stream is host-rx.bin.gz;
host-rx.json contains every recv timestamp and byte count. All complete frames
were checked for LE32 sequence and the board's byte pattern (offset XOR seq).
This fixes the old harness's incorrect assumption that recv boundaries were
RFCOMM application-frame boundaries.

Logged run: 42.125308 KiB/s. No-log run: 88.697444 KiB/s.
Single trials show substantial measurement overhead; replicate when comparing
future firmware changes. No-log run includes brief CPU/health UART sampling.
No source/firmware changes between these runs. No flash or persist writes.
