# BTstack CPU affinity control
Same running Linux dist0b467a4026ba9916, profiling enabled throughout,
legacy callbacks, timer40ms, SPP990, no packet log.
Only BTstack task affinity changed with taskset -p. BTDM retains CPU0.
Mask2=CPU1; mask1=CPU0; mask3=either. Exact commands per run.

|Run|Host bytes|Seconds|KiB/s|
|---|---:|---:|---:|
|mask1-1|1105830|40.000122|26.997720|
|mask2-0|5028210|40.000201|122.758417|
|mask2-3|4972770|40.000184|121.404960|
|mask3-2|4395600|40.000200|107.313916|
All40-second transfers passed with zero sequence/pattern errors, duplicates,
or HCI drops. CPU1 repeats121.40–122.76KiB/s beat unpinned107.31;
sharing CPU0 with the controller produced27.00KiB/s.
Restored mask3 after the control. Use CPU1 for subsequent controlled SPP work,
but this remains below native~214KiB/s. No persistent service default changed.
