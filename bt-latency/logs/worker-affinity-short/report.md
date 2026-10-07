# CPU0 worker retry with quiet/short startup
Same verified distdd46b7df4a5eef9e as compiler comparison.
S31_RADIO_WORKER_CPU=0 python3 -u bt-latency/optimization_matrix.py bt-latency/logs/worker-affinity-short/cpu0 Os Os

Hardware read-mac reset; quiet shell; paced short commands; controller10,
RX32/TX16, local callbacks1, gate_sleep1, timer40, Os binary.
Setup confirmed ordered workerCPU0 and both executable hashes.
First launch was pinnedCPU1 and returned its command marker. The next short
cat/tmp/opt.log command timed out with no marker. No host throughput test ran.
Thus CPU0 startup failure persists with the improved console harness; no
accepted speed or integrity result exists for this setting. Keep worker-1.
No claim of a fully diagnosed underlying controller/scheduler cause.
