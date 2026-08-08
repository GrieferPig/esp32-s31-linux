# ESP-IDF UART0 2 Mbaud benchmark

This standalone ESP-IDF project tests the standard interrupt-driven UART driver
on ESP32-S31 UART0 (TX GPIO58, RX GPIO59) at 2,000,000 baud. The host performs
three 2 MiB device-to-host tests and three 1 MiB host-to-device tests. Every
transfer is checked with CRC16-CCITT, while firmware reports elapsed time and CPU
busy percentage derived from the FreeRTOS idle runtime counter.

TX uses ESP-IDF's documented blocking mode (`tx_buffer_size == 0`); RX uses a
64 KiB software ring buffer.

```sh
. /home/grieferpig/.espressif/master/esp-idf/export.sh
idf.py --preview set-target esp32s31
idf.py --preview build
idf.py --preview -p /dev/ttyUSB0 flash
python3 host/uart_bench.py --port /dev/ttyUSB0 --runs 3
```
