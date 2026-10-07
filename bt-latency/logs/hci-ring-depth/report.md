# BT-only HCI ring depth
Dist2944dc3068ffd7c6; six-slot flash and six digest matches, persist excluded.
Build used the established PATH/ESP_TOOLS/S31_ALLOW_UNPINNED make build image.
Kernel patch applied in scratch and exactly matched source.
Read-only init parameters bt_hci_rx_slots (8/16/32,default8) and
bt_hci_tx_slots (4/8/16,default4). Power-of-two masks maintain wrap/readiness.
Wi-Fi/combo retain8/4. Runtime indices reset before allocation/use.
Each successful boot logged effective ring sizes and controller10x1021B.

Same image, SPP990, controller TX10, CPU1, gate_sleep1, timer40ms,
legacy callbacks, coalescing0, COEX0, pairable1, no pklg, profiler enabled.
Fresh agent/pair and checked40s host receive after each clean reboot.

Commands:
python3 -u bt-latency/hci_ring_matrix.py bt-latency/logs/hci-ring-depth/runs 10:8:4 10:32:4 10:32:16 10:8:4 10:32:16
python3 -u bt-latency/hci_ring_matrix.py bt-latency/logs/hci-ring-depth/retry 10:32:4 10:32:16 10:8:4 10:32:16

Initial RX32 setup timed out with only the echoed command, no bringup output;
console did not respond to CR or Ctrl-C. Read-only ROM read-mac reset recovered
Linux without flash writes. Failed raw evidence retained. Harness now splits
configuration, overlay, and modload phases. All four retry boots succeeded.
Cause of the one console/setup stall is not established.

|Run|Host bytes|Seconds|Host KiB/s|HCI RX rejection increment|
|---|---:|---:|---:|---:|
|runs/buffers10-rx8-tx4-0|7551720|40.000214|184.367177|75|
|retry/buffers10-rx32-tx16-1|7270560|40.000152|177.503232|0|
|retry/buffers10-rx32-tx16-3|7396290|40.000152|180.572800|0|
|retry/buffers10-rx32-tx4-0|7035930|40.000193|171.774806|0|
|retry/buffers10-rx8-tx4-2|7387380|40.000119|180.355419|92|
All completed streams had zero gaps/duplicates/bad patterns/partial frames;
HCI TX drop increments0. RX8 again rejects75/92 VHCI callbacks, despite clean
host payload. RX32 eliminates measured rejections in all three runs.
RX32/TX16 repeats177.50/180.57KiB/s without HCI drops; RX32/TX4 is171.77.
Single RX32/TX4 sample does not establish a precise TX-depth effect.

Current clean Classic result remains below native~214KiB/s. Larger rings use
additional internal SRAM (24 extra RX+12 extra TX frame slots at32/16).
Host-side memory/CPU/heap snapshots retained; CPU window includes connection
and snapshot overhead, whereas reported throughput uses only40s receive.
BLE parity was measured on earlier c252d979f699ebb8 and must be reconfirmed
on the final accepted configuration. Next control repeats local callback
dispatch with the deeper controller/Linux pipeline.
