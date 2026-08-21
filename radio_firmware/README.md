# ESP32-S31 Linux S-mode radio firmware

This directory links the ESP-IDF Wi-Fi, Bluetooth, coexistence and PHY
objects into a relocatable ILP32F payload consumed by the built-in Linux
driver. OpenSBI only prepares the platform and delegates interrupts; it does
not execute the radio blobs.

`make linux-kbuild` produces
`linux-esp32-s31/drivers/platform/esp32s31-radio-idf.o_shipped`. The payload
uses the small `s31_rtos` compatibility scheduler instead of FreeRTOS. Its
allocators, tasks, queues and timers are backed by the loader-carved internal
HP-SRAM pool managed by the Linux driver.

The Linux driver is the only execution owner. TIMG1 ticks, deferred radio
interrupts and typed requests from Linux front ends are all consumed by the
`s31-radio` kthread. Every ESP-IDF entry runs with local interrupts masked,
the ILP32F register state owned by the kernel thread, and the synchronous-trap
stack moved into the reserved SRAM tail. Callers must use the typed API in
`include/linux/esp32s31-radio.h`; raw ESP-IDF symbols are not a driver API.

The current public API only provides a serialized health snapshot. Bluetooth
HCI and cfg80211 operations will be added as explicit typed requests rather
than as a generic function-pointer gateway.

The `boot_*.txt` and `idf_includes.rsp` files capture the ESP-IDF component
link closure used by this target. The Makefile resolves the installed ESP-IDF
and matching picolibc toolchain under `~/.espressif` at build time, and fails
early if either is unavailable. Generated objects and symbol reports are
ignored and can be removed with `make clean`.
