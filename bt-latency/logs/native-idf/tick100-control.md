# Native FreeRTOS tick-rate control
Unchanged native SPP source from bb7264b, CPU320MHz, 990-byte API writes,
same peer and fresh agent/pairing. Only generated sdkconfig changed
CONFIG_FREERTOS_HZ=1000 to100. Native defaults remain1000.
Build: source /home/grieferpig/esp-idf/export.sh; idf.py --preview build.
Flash: /home/grieferpig/.espressif/python_env/idf6.2_py3.13_env/bin/python
bt-latency/native-idf/flash_native.py spp --execute.
Full four-image writes and four explicit digest checks passed; persist excluded.
Exact per-run argv/environment is in command.json.

|Run|Host bytes|Seconds|KiB/s|Sequence gaps|
|---|---:|---:|---:|---:|
|spp-tick100-len990|8820900|40.000210|215.352871|1|
|spp-tick100-len990-repeat|8778330|40.000096|214.314183|0|
|spp-tick100-len990-repeat2|8756550|40.000132|213.782252|0|
All runs had zero duplicates and zero bad complete-frame patterns.
The first run is INVALID: missing sequence1553 between1552 and1554.
Its raw bytes and UART are retained; cause is not established.
Two repeats were error-free, within the native1000Hz baseline213.710117–214.386362KiB/s.
Thus the tick-rate difference does not explain Linux~100–108KiB/s.
Generated config restored to1000 before the next packet-mask control build.
