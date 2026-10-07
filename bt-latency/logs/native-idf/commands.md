# Native baseline build/run commands
Host: BasedSurface; cwd /home/grieferpig/esp32-s31-linux; branch bt-latency-fix.
SDK: /home/grieferpig/esp-idf at 08e0d30a74ad0bfd5a34933142b80f45619ee410.
Shell: source /home/grieferpig/esp-idf/export.sh
In each bt-latency/native-idf/{ble,spp}: idf.py --preview set-target esp32s31 build
Subsequent rebuilds: idf.py --preview build
Native complete flash/verification:
python bt-latency/native-idf/flash_native.py ble --execute
python bt-latency/native-idf/flash_native.py spp --execute
Each invocation prints exact esptool argv, address/size/erase-end/SHA256 for
bootloader, partition table, blank benchmark NVS, application. All native
partitions and writes must remain below Linux persist start 0x1ee000.
No combined Linux image or chip erase command is used.

BLE host:
S31_BT_ADDR=30:ED:A0:F3:D4:AD python3 -u bt-latency/fresh_fd_run.py bt-latency/logs/native-idf/ble-default
S31_BT_ADDR=30:ED:A0:F3:D4:AD S31_BLE_INTERVAL=6 S31_BLE_VALUE_LEN=495 python3 -u bt-latency/fresh_fd_run.py bt-latency/logs/native-idf/ble-iv6-len495
S31_BT_ADDR=30:ED:A0:F3:D4:AD S31_BLE_INTERVAL=12 S31_BLE_VALUE_LEN=244 python3 -u bt-latency/fresh_fd_run.py bt-latency/logs/native-idf/ble-iv12-len244

Parallel UART capture (one reader, no modem-line toggles):
python3 bt-latency/native_monitor.py <run>-uart.txt 58
Host data is primary: bytes/sequence/pattern/duration and every receive timestamp
are in each host-rx.json; host-rx.bin stores exact received payload bytes.
The initial native BLE connection-time DLE call failed. Firmware was then
rebuilt to retry DLE at subscription, after feature discovery; both failures
are in the UART evidence. This was a complete reflash/verify, not an app-only
flash. SPP SDK target defaults initially chose a stock partition table; the
guard rejected it before any SPP flash. The project override was corrected.
SPP build keeps vendor source unchanged, relaxing only GCC 16's existing
-Werror=stringop-truncation warning in the bt component.
