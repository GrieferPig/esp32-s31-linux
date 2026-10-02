# ESP32-S31 radio

`make all` builds Linux, rootfs and the matched flash-XIP radio image. The
Linux module is installed in rootfs; `build/radio.bin` contains the prelinked
radio code and its pristine writable-data template. Kernel and radio image
must be built and deployed together. Layout and image CRC checks reject a
mismatch before executing radio code.

The Wi-Fi frontend is a single-STA mac80211 bridge using native MAC CCMP,
bounded TX aggregation, and Linux RX replay/reorder checks. Runtime mode
selection enables Wi-Fi, Bluetooth, or both. Wi-Fi uses 16 static RX buffers,
32 dynamic RX buffers, a 16-frame TX aggregation cap, and a 48-block RX cache.
No throughput claim follows from a successful build.

The firmware uses the ESP-IDF revision and compiler pinned in
`configs/build-versions.mk`. `make radio-package` packages the matched image,
module, overlays, configuration and licenses. Redistribution requirements
are described in `RADIO_BUNDLE_LICENSES.md`.
