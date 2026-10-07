# Native ESP-IDF dual-mode controller control
Same ESP-IDF08e0d30a74ad0bfd5a34933142b80f45619ee410 SPP application as the
Classic-only reference. Enable BTDM controller, retain BLE controller memory,
and pass ESP_BT_MODE_BTDM to enable. Bluedroid BLE host remains disabled;
the SPP application starts no BLE advertising. This isolates controller mode,
not all differences between Linux BTstack and native Bluedroid.
Source/default diff scratch-applied exactly. Generated sdkconfig diff retained.
CPU320MHz, FreeRTOS1000Hz, performance optimization, SPP990,
native start control, same BasedSurface adapter, fresh pair each40s.

Build and flashing commands/scripts retained. Full four-slot set written and
four explicit digest matches, all erase/partition bounds below0x1ee000.
No persist access. Six HIL flash-safety tests passed. Flash helper now accepts
explicit build/evidence directories so control variants do not overwrite the
baseline manifest; all address, partition, size, and path guards remain.
Linux build overlapped only native flash/idle time, and completed before
either throughput run. No host build during these measurements.

Actual phase commands:
python3 -u bt-latency/logs/native-btdm/flash-and-boot.py
python3 -u bt-latency/logs/native-btdm/run-bench.py

|Run|Host bytes|Seconds|Host KiB/s|
|---|---:|---:|---:|
|run0|8786250|40.000169|214.507148|
|run1|8602110|40.000184|210.011487|
Both payload streams have zero gaps, duplicates, bad patterns, or partial
frames. Rates are end-to-end host-fd reception.
Native BTDM214.51/210.01 overlaps the earlier Classic-only~214 reference,
and remains above Linux RR20~194-198. Controller dual mode alone does not
explain the remaining Linux gap. Original~214 target is unchanged.
Linux HCI drop counters do not exist in this native run.
