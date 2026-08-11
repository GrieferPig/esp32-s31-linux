# ESP32-S31 Linux S-mode radio payload

This directory links the ESP-IDF Wi-Fi, Bluetooth, coexistence and PHY
objects into a relocatable ILP32F payload consumed by the built-in Linux
driver. OpenSBI only prepares the platform and delegates interrupts; it does
not execute the radio blobs.

`make linux-kbuild` produces
`linux-esp32-s31/drivers/platform/esp32s31-radio-idf.o_shipped`. The payload
uses the small `s31_rtos` compatibility scheduler instead of FreeRTOS. Its
allocators, tasks, queues and timers are backed by the loader-carved internal
HP-SRAM pool managed by the Linux driver.

The `boot_*.txt` and `idf_includes.rsp` files capture the ESP-IDF component
link closure used by this target. Generated objects and symbol reports are
ignored and can be removed with `make clean`.
