# Native packet-type API control
Native1000Hz, CPU320MHz, same990-byte checked40-second receiver.
Extended optional start control with a16-bit packet mask. Native application
calls esp_bt_gap_set_acl_pkt_types and waits for its successful changed event
before starting transmission. Original5-byte start leaves policy automatic.
The host environment S31_SPP_PACKET_MASK=0xc30e selects Linux's exact mask.

Full four-slot native write and four digest checks passed; persist excluded.
Build after sourcing /home/grieferpig/esp-idf/export.sh: idf.py --preview build.
Flash: /home/grieferpig/.espressif/python_env/idf6.2_py3.13_env/bin/python
bt-latency/native-idf/flash_native.py spp --execute.
Exact receiver commands/environments and raw UART are retained per run.
Both requested-mask runs reported request=ESP_OK and changed status0 mask0xc30e.

|Run|Host bytes|Seconds|KiB/s|
|---|---:|---:|---:|
|spp-mask-default|8784270|40.000184|214.458729|
|spp-mask-c30e|8820900|40.000224|215.352800|
|spp-mask-default-repeat|8832780|40.000110|215.643451|
|spp-mask-c30e-repeat|8807040|40.000164|215.014745|
All four runs: zero sequence gaps, duplicates, and bad complete payloads.
Linux's forced packet policy does not explain its~100–108KiB/s rate.
Native comparison target remains approximately214KiB/s; these runs show
ordinary variation214.46–215.64KiB/s. No claim of measured on-air modulation.
